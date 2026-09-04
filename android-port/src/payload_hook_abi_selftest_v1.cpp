#include "physics_token_gate.h"

#include <android/log.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char* kTag = "A9TAS_HOOK_ABI_SELFTEST_V1";
constexpr const char* kStatusPath =
    "/data/local/tmp/a9tas-hook-abi-selftest-v1.status";
constexpr std::uint8_t kExpectedPrologue[16] = {
    0xff, 0x83, 0x01, 0xd1, 0xf5, 0x53, 0x04, 0xa9,
    0xf3, 0x7b, 0x05, 0xa9, 0xf4, 0x03, 0x08, 0xaa,
};

a9tas::PhysicsTokenGate g_gate;
thread_local std::uint64_t g_shadow_token{};
std::atomic<std::uint64_t> g_hook_calls{};
std::atomic<std::uint32_t> g_filter_sp_mod16{0xff};
std::atomic<bool> g_installed{false};
std::uint8_t g_original[16]{};
void* g_trampoline = nullptr;
std::size_t g_page_size = 0;
int g_original_protection = 0;
extern "C" std::uintptr_t g_hook_abi_selftest_continue = 0;

extern "C" __attribute__((naked, noinline)) void HookAbiSelftestTarget() {
    __asm__ volatile(
        "sub sp, sp, #0x60\n"
        "stp x21, x20, [sp, #0x40]\n"
        "stp x19, x30, [sp, #0x50]\n"
        "mov x20, x8\n"
        "mov x19, x1\n"
        "ldr x9, [x19]\n"
        "str x9, [x20]\n"
        "add x0, x9, #7\n"
        "ldp x19, x30, [sp, #0x50]\n"
        "ldp x21, x20, [sp, #0x40]\n"
        "add sp, sp, #0x60\n"
        "ret\n");
}

extern "C" __attribute__((noinline)) const std::uint64_t* HookAbiSelftestFilter(
    const std::uint64_t* token) {
    std::uintptr_t sp = 0;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    g_filter_sp_mod16.store(static_cast<std::uint32_t>(sp & 0xf),
                            std::memory_order_relaxed);
    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
    const auto decision = g_gate.Filter(token ? *token : 0);
    g_shadow_token = decision.token_us;
    return &g_shadow_token;
}

extern "C" __attribute__((naked)) void HookAbiSelftestEntry() {
    __asm__ volatile(
        "sub sp, sp, #0x30\n"
        "stp x21, x20, [sp, #0x10]\n"
        "stp x19, x30, [sp, #0x20]\n"
        "mov x20, x8\n"
        "mov x21, x0\n"
        "mov x19, x1\n"
        "mov x0, x19\n"
        "bl HookAbiSelftestFilter\n"
        "mov x1, x0\n"
        "mov x0, x21\n"
        "mov x8, x20\n"
        "ldp x19, x30, [sp, #0x20]\n"
        "ldp x21, x20, [sp, #0x10]\n"
        "add sp, sp, #0x30\n"
        "adrp x17, :got:g_hook_abi_selftest_continue\n"
        "ldr x17, [x17, #:got_lo12:g_hook_abi_selftest_continue]\n"
        "ldr x17, [x17]\n"
        "br x17\n");
}

extern "C" __attribute__((naked, noinline)) std::uint64_t HookAbiInvokeRaw(
    void*, const std::uint64_t*, std::uint64_t*) {
    __asm__ volatile("mov x8, x2\n" "b HookAbiSelftestTarget\n");
}

void AbsoluteJump(std::uint8_t output[16], std::uintptr_t destination) {
    constexpr std::uint32_t kLoadX17 = 0x58000051;
    constexpr std::uint32_t kBranchX17 = 0xd61f0220;
    std::memcpy(output, &kLoadX17, 4);
    std::memcpy(output + 4, &kBranchX17, 4);
    std::memcpy(output + 8, &destination, 8);
}

int MappingProtection(const void* address) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return 0;
    char line[2048]{};
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    int protection = 0;
    while (std::fgets(line, sizeof(line), maps)) {
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

bool SetTargetProtection(int protection) {
    auto* target = reinterpret_cast<std::uint8_t*>(&HookAbiSelftestTarget);
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    const auto page = address & ~static_cast<std::uintptr_t>(g_page_size - 1);
    if ((address + sizeof(g_original) - 1) / g_page_size != address / g_page_size)
        return false;
    return mprotect(reinterpret_cast<void*>(page), g_page_size, protection) == 0;
}

bool RestoreOriginal() {
    if (!g_installed.load(std::memory_order_acquire)) return true;
    auto* target = reinterpret_cast<std::uint8_t*>(&HookAbiSelftestTarget);
    if (!SetTargetProtection(g_original_protection | PROT_WRITE)) return false;
    std::memcpy(target, g_original, sizeof(g_original));
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + sizeof(g_original)));
    const bool bytes_restored =
        std::memcmp(target, g_original, sizeof(g_original)) == 0;
    const bool protection_restored = SetTargetProtection(g_original_protection);
    if (bytes_restored && protection_restored)
        g_installed.store(false, std::memory_order_release);
    return bytes_restored && protection_restored;
}

bool Install() {
    auto* target = reinterpret_cast<std::uint8_t*>(&HookAbiSelftestTarget);
    if (g_installed.load(std::memory_order_acquire) ||
        std::memcmp(target, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0)
        return false;
    g_page_size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    if (g_page_size == 0 || (g_page_size & (g_page_size - 1)) != 0) return false;
    g_original_protection = MappingProtection(target);
    if ((g_original_protection & (PROT_READ | PROT_EXEC)) !=
        (PROT_READ | PROT_EXEC)) return false;
    std::memcpy(g_original, target, sizeof(g_original));

    auto* trampoline = static_cast<std::uint8_t*>(mmap(
        nullptr, 32, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (trampoline == MAP_FAILED) return false;
    std::memcpy(trampoline, g_original, sizeof(g_original));
    AbsoluteJump(trampoline + 16,
                 reinterpret_cast<std::uintptr_t>(target + 16));
    __builtin___clear_cache(reinterpret_cast<char*>(trampoline),
                            reinterpret_cast<char*>(trampoline + 32));
    if (mprotect(trampoline, 32, PROT_READ | PROT_EXEC) != 0) {
        munmap(trampoline, 32);
        return false;
    }
    g_trampoline = trampoline;
    g_hook_abi_selftest_continue = reinterpret_cast<std::uintptr_t>(trampoline);

    std::uint8_t patch[16]{};
    AbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&HookAbiSelftestEntry));
    if (!SetTargetProtection(g_original_protection | PROT_WRITE)) {
        munmap(g_trampoline, 32);
        g_trampoline = nullptr;
        g_hook_abi_selftest_continue = 0;
        return false;
    }
    std::memcpy(target, patch, sizeof(patch));
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + sizeof(patch)));
    g_installed.store(true, std::memory_order_release);
    const bool bytes_installed = std::memcmp(target, patch, sizeof(patch)) == 0;
    const bool protection_restored = SetTargetProtection(g_original_protection);
    if (!bytes_installed || !protection_restored) {
        RestoreOriginal();
        return false;
    }
    return true;
}

std::uint64_t Invoke(std::uint64_t token, std::uint64_t* observed) {
    return HookAbiInvokeRaw(nullptr, &token, observed);
}

struct Results {
    int checks{};
    int failures{};
    bool original_restored{};
};

void Check(Results* results, bool condition) {
    ++results->checks;
    if (!condition) ++results->failures;
}

Results RunSelftest() {
    Results results{};
    Check(&results, Install());
    if (results.failures) return results;

    std::uint64_t observed = 0;
    std::uint64_t result = Invoke(12345, &observed);
    Check(&results, observed == 12345 && result == 12352);
    Check(&results, g_filter_sp_mod16.load(std::memory_order_relaxed) == 0);

    g_gate.Freeze();
    result = Invoke(99999, &observed);
    Check(&results, observed == 0 && result == 7);
    Check(&results, g_gate.RequestSteps());
    result = Invoke(99999, &observed);
    Check(&results, observed == 16666 && result == 16673);
    result = Invoke(99999, &observed);
    Check(&results, observed == 0 && result == 7);

    g_gate.Resume();
    bool stress_ok = true;
    for (std::uint64_t i = 0; i < 100000; ++i) {
        result = Invoke(i, &observed);
        if (observed != i || result != i + 7) {
            stress_ok = false;
            break;
        }
    }
    Check(&results, stress_ok);
    Check(&results, g_hook_calls.load(std::memory_order_relaxed) == 100004);
    results.original_restored = RestoreOriginal();
    Check(&results, results.original_restored);
    if (results.original_restored && g_trampoline) {
        munmap(g_trampoline, 32);
        g_trampoline = nullptr;
        g_hook_abi_selftest_continue = 0;
    }
    return results;
}

void WriteStatus(const Results& results) {
    const int fd = open(kStatusPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                        S_IRUSR | S_IWUSR);
    if (fd < 0) return;
    char status[512]{};
    const int size = std::snprintf(
        status, sizeof(status),
        "protocol=hook-abi-selftest-v1\nabi=arm64-v8a\n"
        "scope=self-only\ngame_modules_scanned=0\ngame_addresses_touched=0\n"
        "threads_created=0\nchecks=%d\nfailures=%d\n"
        "calls=%llu\nsp_aligned=%d\noriginal_restored=%d\nresult=%s\n",
        results.checks, results.failures,
        static_cast<unsigned long long>(g_hook_calls.load()),
        g_filter_sp_mod16.load() == 0 ? 1 : 0,
        results.original_restored ? 1 : 0,
        results.failures == 0 ? "PASS" : "FAIL");
    if (size > 0) write(fd, status, static_cast<std::size_t>(size));
    fsync(fd);
    close(fd);
}

__attribute__((constructor)) void OnLoad() {
    const Results results = RunSelftest();
    WriteStatus(results);
    __android_log_print(results.failures == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                        kTag, "self-only checks=%d failures=%d restored=%d",
                        results.checks, results.failures,
                        results.original_restored ? 1 : 0);
}
}  // namespace

extern "C" __attribute__((visibility("default"))) int
    a9tas_hook_abi_selftest_v1_protocol() {
    return 1;
}
