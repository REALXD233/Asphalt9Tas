#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kTag = "A9TAS_BOOTSTRAP";
#ifndef A9TAS_PAYLOAD_PATH
#define A9TAS_PAYLOAD_PATH "/data/local/tmp/liba9tas_payload.so"
#endif
// Tests compile an isolated bootstrap with a unique path. This prevents a
// late-attach experiment from accidentally loading the historical production
// payload or overwriting it on device.
constexpr const char* kPayloadPath = A9TAS_PAYLOAD_PATH;
#ifdef A9TAS_SECOND_PAYLOAD_PATH
constexpr const char* kSecondPayloadPath = A9TAS_SECOND_PAYLOAD_PATH;
#endif
#if defined(A9TAS_THIRD_PAYLOAD_PATH) && !defined(A9TAS_SECOND_PAYLOAD_PATH)
#error "A9TAS_THIRD_PAYLOAD_PATH requires A9TAS_SECOND_PAYLOAD_PATH"
#endif
#ifdef A9TAS_THIRD_PAYLOAD_PATH
constexpr const char* kThirdPayloadPath = A9TAS_THIRD_PAYLOAD_PATH;
#endif
#ifndef A9TAS_ENABLE_UNSAFE_LATE_ARM_LOAD
#define A9TAS_ENABLE_UNSAFE_LATE_ARM_LOAD 0
#endif
#ifndef A9TAS_ENABLE_SAME_THREAD_PROBE
#define A9TAS_ENABLE_SAME_THREAD_PROBE 0
#endif
#ifndef A9TAS_SAME_THREAD_PROBE_SYMBOL
#define A9TAS_SAME_THREAD_PROBE_SYMBOL "a9tas_same_thread_probe_v1"
#endif
#ifndef A9TAS_SAME_THREAD_PROBE_SHORTY
#define A9TAS_SAME_THREAD_PROBE_SHORTY "J"
#endif
std::atomic<bool> g_loaded{false};
std::atomic<std::uintptr_t> g_same_thread_probe_trampoline{0};
std::atomic<int> g_same_thread_probe_status{0};

enum class BootstrapStage : int {
    kCold = 0,
    kInstalling = 1,
    kArmed = 2,
    kClaimed = 3,
    kCallbacksRestored = 4,
    kPayloadLoaded = 5,
    kExpiredRestored = 6,
    kPayloadLoadFailed = -1,
    kCallbackRestoreFailed = -2,
    kArmFailed = -3,
    kTooLate = -4,
};
std::atomic<BootstrapStage> g_stage{BootstrapStage::kCold};

using LoadLibraryFn = void* (*)(const char*, int);
using LoadLibraryExtFn = void* (*)(const char*, int, void*);
LoadLibraryFn g_real_load_library = nullptr;
LoadLibraryExtFn g_real_load_library_ext = nullptr;

struct NativeBridgeCallbacksV4 {
    std::uint32_t version;
    bool (*initialize)(const void*, const char*, const char*);
    LoadLibraryFn loadLibrary;
    void* (*getTrampoline)(void*, const char*, const char*, std::uint32_t);
    bool (*isSupported)(const char*);
    const void* (*getAppEnv)(const char*);
    bool (*isCompatibleWith)(std::uint32_t);
    void* (*getSignalHandler)(int);
    int (*unloadLibrary)(void*);
    const char* (*getError)();
    bool (*isPathSupported)(const char*);
    bool (*initAnonymousNamespace)(const char*, const char*);
    void* (*createNamespace)(const char*, const char*, const char*, std::uint64_t,
                             const char*, void*);
    bool (*linkNamespaces)(void*, void*, const char*);
    LoadLibraryExtFn loadLibraryExt;
    void* (*getVendorNamespace)();
};
NativeBridgeCallbacksV4* g_bridge = nullptr;

static_assert(sizeof(NativeBridgeCallbacksV4) == 128);

bool IsGameLibrary(const char* path) {
    return path != nullptr && std::strstr(path, "libAsphalt9.so") != nullptr;
}

int MappingProtection(void* address);
bool RestoreNativeBridgeCallbacks();

const char* BridgeError() {
    if (g_bridge == nullptr || g_bridge->getError == nullptr) return "unavailable";
    const char* error = g_bridge->getError();
    return error != nullptr ? error : "unavailable";
}

void PublishSameThreadProbeTrampoline(void* payload) {
#if A9TAS_ENABLE_SAME_THREAD_PROBE
    if (payload == nullptr || g_bridge == nullptr ||
        g_bridge->getTrampoline == nullptr) {
        g_same_thread_probe_status.store(-1, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "same-thread probe unavailable: payload=%p bridge=%p",
                            payload, g_bridge);
        return;
    }
    constexpr const char* kSymbol = A9TAS_SAME_THREAD_PROBE_SYMBOL;
    constexpr const char* kShorty = A9TAS_SAME_THREAD_PROBE_SHORTY;
    void* trampoline = g_bridge->getTrampoline(
        payload, kSymbol, kShorty,
        static_cast<std::uint32_t>(std::strlen(kShorty)));
    if (trampoline == nullptr) {
        g_same_thread_probe_status.store(-2, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "same-thread probe trampoline failed symbol=%s shorty=%s "
                            "bridge_error=%s",
                            kSymbol, kShorty, BridgeError());
        return;
    }
    g_same_thread_probe_trampoline.store(
        reinterpret_cast<std::uintptr_t>(trampoline),
        std::memory_order_release);
    g_same_thread_probe_status.store(1, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "same-thread probe trampoline ready address=%p symbol=%s "
                        "shorty=%s",
                        trampoline, kSymbol, kShorty);
#else
    (void)payload;
#endif
}

void* HookedLoadLibrary(const char* path, int flags) {
    BootstrapStage expected = BootstrapStage::kArmed;
    if (IsGameLibrary(path) &&
        g_stage.compare_exchange_strong(expected, BootstrapStage::kClaimed,
                                        std::memory_order_acq_rel)) {
        // The callback table is process-global. Restore it before any nested
        // guest load so the interception is strictly one-shot and cannot
        // poison later NativeBridge activity.
        if (!RestoreNativeBridgeCallbacks()) {
            g_stage.store(BootstrapStage::kCallbackRestoreFailed,
                          std::memory_order_release);
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "game loadLibrary intercepted but callback restore failed; "
                                "payload load suppressed");
        } else {
            g_stage.store(BootstrapStage::kCallbacksRestored,
                          std::memory_order_release);
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "game loadLibrary intercepted; callbacks restored; "
                                "loading payload in normal guest-load context");
            void* payload = g_real_load_library(kPayloadPath, RTLD_NOW | RTLD_LOCAL);
#ifdef A9TAS_SECOND_PAYLOAD_PATH
            void* second_payload = payload != nullptr
                ? g_real_load_library(kSecondPayloadPath,
                                      RTLD_NOW | RTLD_LOCAL)
                : nullptr;
            if (second_payload == nullptr) payload = nullptr;
#endif
#ifdef A9TAS_THIRD_PAYLOAD_PATH
            void* third_payload = payload != nullptr
                ? g_real_load_library(kThirdPayloadPath,
                                      RTLD_NOW | RTLD_LOCAL)
                : nullptr;
            if (third_payload == nullptr) payload = nullptr;
#endif
            PublishSameThreadProbeTrampoline(payload);
            g_stage.store(payload != nullptr ? BootstrapStage::kPayloadLoaded
                                             : BootstrapStage::kPayloadLoadFailed,
                          std::memory_order_release);
            __android_log_print(payload != nullptr ? ANDROID_LOG_INFO
                                                   : ANDROID_LOG_ERROR,
                                kTag,
#ifdef A9TAS_THIRD_PAYLOAD_PATH
                                "payload set via loadLibrary first=%s second=%s "
                                "third=%s status=%s bridge_error=%s",
                                kPayloadPath, kSecondPayloadPath,
                                kThirdPayloadPath,
                                payload != nullptr ? "loaded" : "failed",
#elif defined(A9TAS_SECOND_PAYLOAD_PATH)
                                "payload pair via loadLibrary first=%s second=%s "
                                "status=%s bridge_error=%s",
                                kPayloadPath, kSecondPayloadPath,
                                payload != nullptr ? "loaded" : "failed",
#else
                                "payload via loadLibrary handle=%p bridge_error=%s",
                                payload,
#endif
                                payload != nullptr ? "none" : BridgeError());
        }
    }
    return g_real_load_library(path, flags);
}

void* HookedLoadLibraryExt(const char* path, int flags, void* ns) {
    BootstrapStage expected = BootstrapStage::kArmed;
    if (IsGameLibrary(path) &&
        g_stage.compare_exchange_strong(expected, BootstrapStage::kClaimed,
                                        std::memory_order_acq_rel)) {
        if (!RestoreNativeBridgeCallbacks()) {
            g_stage.store(BootstrapStage::kCallbackRestoreFailed,
                          std::memory_order_release);
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "game loadLibraryExt intercepted but callback restore failed; "
                                "payload load suppressed");
        } else {
            g_stage.store(BootstrapStage::kCallbacksRestored,
                          std::memory_order_release);
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "game loadLibraryExt intercepted ns=%p; callbacks restored; "
                                "loading payload in normal guest-load context", ns);
            void* payload =
                g_real_load_library_ext(kPayloadPath, RTLD_NOW | RTLD_LOCAL, ns);
#ifdef A9TAS_SECOND_PAYLOAD_PATH
            void* second_payload = payload != nullptr
                ? g_real_load_library_ext(kSecondPayloadPath,
                                          RTLD_NOW | RTLD_LOCAL, ns)
                : nullptr;
            if (second_payload == nullptr) payload = nullptr;
#endif
#ifdef A9TAS_THIRD_PAYLOAD_PATH
            void* third_payload = payload != nullptr
                ? g_real_load_library_ext(kThirdPayloadPath,
                                          RTLD_NOW | RTLD_LOCAL, ns)
                : nullptr;
            if (third_payload == nullptr) payload = nullptr;
#endif
            PublishSameThreadProbeTrampoline(payload);
            g_stage.store(payload != nullptr ? BootstrapStage::kPayloadLoaded
                                             : BootstrapStage::kPayloadLoadFailed,
                          std::memory_order_release);
            __android_log_print(payload != nullptr ? ANDROID_LOG_INFO
                                                   : ANDROID_LOG_ERROR,
                                kTag,
 #ifdef A9TAS_THIRD_PAYLOAD_PATH
                                "payload set via loadLibraryExt first=%s "
                                "second=%s third=%s ns=%p status=%s "
                                "bridge_error=%s",
                                kPayloadPath, kSecondPayloadPath,
                                kThirdPayloadPath, ns,
                                payload != nullptr ? "loaded" : "failed",
 #elif defined(A9TAS_SECOND_PAYLOAD_PATH)
                                "payload pair via loadLibraryExt first=%s "
                                "second=%s ns=%p status=%s bridge_error=%s",
                                kPayloadPath, kSecondPayloadPath, ns,
                                payload != nullptr ? "loaded" : "failed",
 #else
                                "payload via loadLibraryExt handle=%p ns=%p bridge_error=%s",
                                payload, ns,
 #endif
                                payload != nullptr ? "none" : BridgeError());
        }
    }
    return g_real_load_library_ext(path, flags, ns);
}

int LogModule(dl_phdr_info* info, size_t, void*) {
    if (info == nullptr || info->dlpi_name == nullptr) return 0;
    const char* name = info->dlpi_name;
    if (std::strstr(name, "houdini") != nullptr ||
        std::strstr(name, "libnb.so") != nullptr ||
        std::strstr(name, "libAsphalt9.so") != nullptr) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "module base=%p path=%s",
                            reinterpret_cast<void*>(info->dlpi_addr), name);
    }
    return 0;
}

int MappingProtection(void* address) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) return 0;
    char line[2048]{};
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    int protection = 0;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long start = 0, end = 0;
        char perms[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &start, &end, perms) == 3 &&
            target >= start && target < end) {
            if (perms[0] == 'r') protection |= PROT_READ;
            if (perms[1] == 'w') protection |= PROT_WRITE;
            if (perms[2] == 'x') protection |= PROT_EXEC;
            break;
        }
    }
    std::fclose(maps);
    return protection;
}

bool ArmNativeBridgeCallbacks() {
    BootstrapStage expected = BootstrapStage::kCold;
    if (!g_stage.compare_exchange_strong(expected, BootstrapStage::kInstalling,
                                         std::memory_order_acq_rel)) {
        return false;
    }
    void* nb = dlopen("libnb.so", RTLD_NOW | RTLD_NOLOAD);
    if (nb == nullptr) nb = dlopen("/system/lib64/libnb.so", RTLD_NOW | RTLD_NOLOAD);
    auto* bridge = nb != nullptr
        ? static_cast<NativeBridgeCallbacksV4*>(dlsym(nb, "NativeBridgeItf"))
        : nullptr;
    if (bridge == nullptr || bridge->version < 3 || bridge->version > 4 ||
        bridge->loadLibrary == nullptr || bridge->loadLibraryExt == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "native bridge callback table unavailable");
        g_stage.store(BootstrapStage::kArmFailed, std::memory_order_release);
        return false;
    }

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        g_stage.store(BootstrapStage::kArmFailed, std::memory_order_release);
        return false;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(bridge);
    void* page = reinterpret_cast<void*>(address & ~static_cast<std::uintptr_t>(page_size - 1));
    const int original_protection = MappingProtection(bridge);
    if (original_protection == 0 ||
        mprotect(page, static_cast<size_t>(page_size),
                 original_protection | PROT_WRITE) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "native bridge table mprotect failed");
        g_stage.store(BootstrapStage::kArmFailed, std::memory_order_release);
        return false;
    }

    g_real_load_library = bridge->loadLibrary;
    g_real_load_library_ext = bridge->loadLibraryExt;
    g_bridge = bridge;
    __atomic_store_n(&bridge->loadLibrary, &HookedLoadLibrary, __ATOMIC_RELEASE);
    __atomic_store_n(&bridge->loadLibraryExt, &HookedLoadLibraryExt, __ATOMIC_RELEASE);
    const bool installed =
        __atomic_load_n(&bridge->loadLibrary, __ATOMIC_ACQUIRE) ==
            &HookedLoadLibrary &&
        __atomic_load_n(&bridge->loadLibraryExt, __ATOMIC_ACQUIRE) ==
            &HookedLoadLibraryExt;
    const bool protection_restored =
        mprotect(page, static_cast<size_t>(page_size), original_protection) == 0;
    if (!installed || !protection_restored) {
        // Best-effort rollback. This path is not expected, but leaving a
        // half-installed process-global callback table is worse than failing
        // the bootstrap.
        if (protection_restored) {
            RestoreNativeBridgeCallbacks();
        } else {
            __atomic_store_n(&bridge->loadLibrary, g_real_load_library,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&bridge->loadLibraryExt, g_real_load_library_ext,
                             __ATOMIC_RELEASE);
            mprotect(page, static_cast<size_t>(page_size), original_protection);
        }
        g_stage.store(BootstrapStage::kArmFailed, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "native bridge callback installation verification failed");
        return false;
    }
    g_stage.store(BootstrapStage::kArmed, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "native bridge callbacks armed version=%u", bridge->version);
    return true;
}

bool RestoreNativeBridgeCallbacks() {
    if (g_bridge == nullptr || g_real_load_library == nullptr ||
        g_real_load_library_ext == nullptr) {
        return false;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(g_bridge);
    void* page = reinterpret_cast<void*>(
        address & ~static_cast<std::uintptr_t>(page_size - 1));
    const int original_protection = MappingProtection(g_bridge);
    if (original_protection == 0 ||
        mprotect(page, static_cast<size_t>(page_size),
                 original_protection | PROT_WRITE) != 0) {
        return false;
    }
    __atomic_store_n(&g_bridge->loadLibrary, g_real_load_library,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_bridge->loadLibraryExt, g_real_load_library_ext,
                     __ATOMIC_RELEASE);
    const bool restored =
        __atomic_load_n(&g_bridge->loadLibrary, __ATOMIC_ACQUIRE) ==
            g_real_load_library &&
        __atomic_load_n(&g_bridge->loadLibraryExt, __ATOMIC_ACQUIRE) ==
            g_real_load_library_ext;
    const bool protection_restored =
        mprotect(page, static_cast<size_t>(page_size), original_protection) == 0;
    return restored && protection_restored;
}

bool GameLibraryMapped() {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) return false;
    char line[2048]{};
    bool found = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        if (std::strstr(line, "libAsphalt9.so") != nullptr) {
            found = true;
            break;
        }
    }
    std::fclose(maps);
    return found;
}

void* CallbackWatchdog(void*) {
    // The game library normally follows process creation quickly. A callback
    // table hook that has not fired within 15 seconds is stale and must not
    // remain process-global for the rest of the session.
    for (int attempt = 0; attempt < 1500; ++attempt) {
        if (g_stage.load(std::memory_order_acquire) != BootstrapStage::kArmed) {
            return nullptr;
        }
        usleep(10000);
    }

    BootstrapStage expected = BootstrapStage::kArmed;
    if (!g_stage.compare_exchange_strong(expected, BootstrapStage::kClaimed,
                                         std::memory_order_acq_rel)) {
        return nullptr;
    }
    if (RestoreNativeBridgeCallbacks()) {
        g_stage.store(BootstrapStage::kExpiredRestored,
                      std::memory_order_release);
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "normal-load interception expired; callbacks restored");
    } else {
        g_stage.store(BootstrapStage::kCallbackRestoreFailed,
                      std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "normal-load interception expired; callback restore failed");
    }
    return nullptr;
}

// Late-attach fallback: the injector may arm the callbacks after the game
// already dlopen'ed libAsphalt9.so, so the loadLibrary hook would never fire.
// The payload is self-sufficient (its worker polls /proc/self/maps for the
// game base), so dlopen'ing it directly once the game library is mapped is
// enough — no loadLibrary interception is required for correctness.
[[maybe_unused]] void* FallbackLoader(void*) {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (GameLibraryMapped()) break;
        usleep(10000);
    }
    if (GameLibraryMapped() && !A9TAS_ENABLE_UNSAFE_LATE_ARM_LOAD) {
        __android_log_print(
            ANDROID_LOG_WARN, kTag,
            "late ARM load disabled: LDPlayer9/Houdini v3 rejects or crashes "
            "cross-bridge loads initiated from an x86-created thread");
        return nullptr;
    }
    BootstrapStage expected = BootstrapStage::kArmed;
    if (GameLibraryMapped() &&
        g_stage.compare_exchange_strong(expected, BootstrapStage::kClaimed,
                                        std::memory_order_acq_rel)) {
        if (!RestoreNativeBridgeCallbacks()) {
            g_stage.store(BootstrapStage::kCallbackRestoreFailed,
                          std::memory_order_release);
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "late attach claimed but callback restore failed; "
                                "unsafe payload load suppressed");
            return nullptr;
        }
        g_stage.store(BootstrapStage::kCallbacksRestored,
                      std::memory_order_release);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "late attach: game library mapped; loading payload directly");
        // Houdini NativeBridge v3 rejects the deprecated loadLibrary callback
        // for late guest loads. Use loadLibraryExt with the vendor namespace,
        // matching the supported v3 interface.
        void* ns = g_bridge != nullptr && g_bridge->getVendorNamespace != nullptr
            ? g_bridge->getVendorNamespace() : nullptr;
        void* payload = g_real_load_library_ext != nullptr
            ? g_real_load_library_ext(kPayloadPath, RTLD_NOW | RTLD_LOCAL, ns)
            : nullptr;
        const char* bridge_error = g_bridge != nullptr && g_bridge->getError != nullptr
            ? g_bridge->getError() : nullptr;
        g_stage.store(payload != nullptr ? BootstrapStage::kPayloadLoaded
                                         : BootstrapStage::kPayloadLoadFailed,
                      std::memory_order_release);
        __android_log_print(payload != nullptr ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                            kTag, "payload via fallback-ext handle=%p ns=%p bridge_error=%s",
                            payload, ns,
                            payload != nullptr ? "none" :
                            (bridge_error != nullptr ? bridge_error : "unavailable"));
    }
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    g_loaded.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded pid=%d abi=x86_64 passive=1 protocol=1", getpid());
    dl_iterate_phdr(LogModule, nullptr);
    if (GameLibraryMapped()) {
        g_stage.store(BootstrapStage::kTooLate, std::memory_order_release);
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "game library already mapped; normal-load interception skipped");
    } else {
        if (ArmNativeBridgeCallbacks()) {
            pthread_t watchdog{};
            if (pthread_create(&watchdog, nullptr, CallbackWatchdog, nullptr) == 0) {
                pthread_detach(watchdog);
            } else {
                BootstrapStage expected = BootstrapStage::kArmed;
                if (g_stage.compare_exchange_strong(expected, BootstrapStage::kClaimed,
                                                    std::memory_order_acq_rel)) {
                    const bool restored = RestoreNativeBridgeCallbacks();
                    g_stage.store(restored ? BootstrapStage::kExpiredRestored
                                           : BootstrapStage::kCallbackRestoreFailed,
                                  std::memory_order_release);
                }
            }
        }
    }
#if A9TAS_ENABLE_UNSAFE_LATE_ARM_LOAD
    pthread_t fallback{};
    if (pthread_create(&fallback, nullptr, FallbackLoader, nullptr) == 0) {
        pthread_detach(fallback);
    }
#endif
}

}  // namespace

extern "C" __attribute__((visibility("default"))) int a9tas_bootstrap_status() {
    return g_loaded.load(std::memory_order_acquire) ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) int a9tas_bootstrap_stage() {
    return static_cast<int>(g_stage.load(std::memory_order_acquire));
}

extern "C" __attribute__((visibility("default"))) int
a9tas_bootstrap_same_thread_probe_status() {
    return g_same_thread_probe_status.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_bootstrap_same_thread_probe_trampoline() {
    return g_same_thread_probe_trampoline.load(std::memory_order_acquire);
}
