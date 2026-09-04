#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kTag = "A9TAS_KEY_ACTION";
constexpr const char* kEnableMarker =
    "/data/local/tmp/a9tas-enable-keyboard-action-probe";
constexpr const char* kOutput =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-keyboard-actions.bin";
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

// Event-aware intrusive dispatcher. The keyboard action path reaches it via
// a tail branch at 0x661dd98, retaining LR=0x6613fe4.
constexpr std::uintptr_t kDispatchOffset = 0x5ba6660;
constexpr std::uintptr_t kKeyboardReturnOffset = 0x6613fe4;
constexpr std::uint8_t kDispatchSignature[16] = {
    0xff, 0xc3, 0x01, 0xd1, 0xf8, 0x1b, 0x00, 0xf9,
    0xf7, 0x5b, 0x04, 0xa9, 0xf5, 0x53, 0x05, 0xa9,
};

constexpr std::size_t kCapacity = 256;
using DispatchFn = void (*)(void*, const void*);

struct Record {
    std::uint64_t sequence;
    std::uint64_t caller_offset;
    std::uint32_t action;
    std::uint32_t pressed;
    std::uint64_t container;
    std::uint64_t subscriber_count;
    std::uint64_t object;
    std::uint64_t vtable_offset;
    std::uint64_t callback_offset;
    std::uint8_t object_bytes[0x100];
    std::uint8_t vtable_bytes[0x100];
    std::uint8_t callback_bytes[0x100];
};

struct DumpHeader {
    char magic[8];
    std::uint8_t build_id[20];
    std::uint32_t record_size;
    std::uint64_t guest_base;
    std::uint64_t keyboard_events;
    std::uint32_t record_count;
    std::uint32_t reserved;
};

struct GameMapping {
    std::uintptr_t base{};
    char path[1024]{};
};

std::atomic<std::uintptr_t> g_guest_base{0};
std::atomic<std::uintptr_t> g_trampoline{0};
std::atomic<std::uint64_t> g_keyboard_events{0};
std::atomic<std::uint32_t> g_record_count{0};
std::atomic<std::uint32_t> g_dump_request{0};
Record g_records[kCapacity]{};

bool FindGameMapping(GameMapping* mapping) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[2048]{};
    bool found = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char permissions[5]{}, path[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &start, &end,
            permissions, &offset, path);
        if (fields == 5 && offset == 0 &&
            std::strstr(path, "libAsphalt9.so")) {
            char* clean = path;
            while (*clean == ' ') ++clean;
            mapping->base = static_cast<std::uintptr_t>(start);
            std::snprintf(mapping->path, sizeof(mapping->path), "%s", clean);
            found = true;
            break;
        }
    }
    std::fclose(maps);
    return found;
}

int FindGuestGameModule(dl_phdr_info* info, size_t, void*) {
    if (info && info->dlpi_name &&
        std::strstr(info->dlpi_name, "libAsphalt9.so")) {
        g_guest_base.store(static_cast<std::uintptr_t>(info->dlpi_addr),
                           std::memory_order_release);
        return 1;
    }
    return 0;
}

bool ReadBuildId(const char* path, std::uint8_t output[20]) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    Elf64_Ehdr h{};
    if (std::fread(&h, sizeof(h), 1, file) != 1 ||
        std::memcmp(h.e_ident, ELFMAG, SELFMAG) != 0 ||
        h.e_ident[EI_CLASS] != ELFCLASS64 || h.e_machine != EM_AARCH64) {
        std::fclose(file);
        return false;
    }
    bool found = false;
    for (std::uint16_t i = 0; i < h.e_phnum && !found; ++i) {
        Elf64_Phdr p{};
        if (std::fseek(file, static_cast<long>(h.e_phoff) +
                                static_cast<long>(i) * sizeof(p),
                       SEEK_SET) != 0 ||
            std::fread(&p, sizeof(p), 1, file) != 1)
            break;
        if (p.p_type != PT_NOTE || p.p_filesz > 1024 * 1024) continue;
        std::uint64_t cursor = p.p_offset, end = p.p_offset + p.p_filesz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr n{};
            std::fseek(file, static_cast<long>(cursor), SEEK_SET);
            if (std::fread(&n, sizeof(n), 1, file) != 1) break;
            cursor += sizeof(n);
            const auto ns = (n.n_namesz + 3u) & ~3u;
            const auto ds = (n.n_descsz + 3u) & ~3u;
            if (cursor + ns + ds > end) break;
            char name[16]{};
            if (n.n_namesz < sizeof(name)) {
                std::fseek(file, static_cast<long>(cursor), SEEK_SET);
                std::fread(name, 1, n.n_namesz, file);
            }
            cursor += ns;
            if (n.n_type == NT_GNU_BUILD_ID && n.n_descsz == 20 &&
                std::strcmp(name, "GNU") == 0) {
                std::fseek(file, static_cast<long>(cursor), SEEK_SET);
                found = std::fread(output, 20, 1, file) == 1;
                break;
            }
            cursor += ds;
        }
    }
    std::fclose(file);
    return found;
}

void WriteAbsoluteJump(std::uint8_t out[16], std::uintptr_t target) {
    constexpr std::uint32_t ldr = 0x58000051, br = 0xd61f0220;
    std::memcpy(out, &ldr, 4);
    std::memcpy(out + 4, &br, 4);
    std::memcpy(out + 8, &target, 8);
}

bool WriteProcMem(std::uintptr_t address, const std::uint8_t bytes[16]) {
    const int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) return false;
    const ssize_t n = pwrite(fd, bytes, 16, static_cast<off_t>(address));
    close(fd);
    return n == 16;
}

std::uintptr_t BuildTrampoline(std::uintptr_t target) {
    auto* stub = static_cast<std::uint8_t*>(
        mmap(nullptr, 32, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (stub == MAP_FAILED) return 0;
    std::memcpy(stub, reinterpret_cast<const void*>(target), 16);
    WriteAbsoluteJump(stub + 16, target + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(stub),
                            reinterpret_cast<char*>(stub + 32));
    if (mprotect(stub, 32, PROT_READ | PROT_EXEC) != 0) {
        munmap(stub, 32);
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(stub);
}

void Capture(void* container_pointer, const void* event_pointer,
             std::uintptr_t caller) {
    const std::uintptr_t base = g_guest_base.load(std::memory_order_relaxed);
    if (caller != base + kKeyboardReturnOffset || !event_pointer) return;
    const std::uint64_t sequence =
        g_keyboard_events.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t slot = g_record_count.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kCapacity) {
        g_record_count.store(kCapacity, std::memory_order_relaxed);
        return;
    }

    Record& r = g_records[slot];
    r.sequence = sequence;
    r.caller_offset = caller - base;
    std::memcpy(&r.action,
                static_cast<const std::uint8_t*>(event_pointer) + 0x10, 4);
    std::memcpy(&r.pressed,
                static_cast<const std::uint8_t*>(event_pointer) + 0x14, 4);
    r.container = reinterpret_cast<std::uintptr_t>(container_pointer);
    const auto container = r.container;
    const auto head = *reinterpret_cast<const std::uintptr_t*>(container);
    r.subscriber_count =
        *reinterpret_cast<const std::uint64_t*>(container + 0x10);
    if (r.subscriber_count != 0 && head != 0) {
        r.object = head - 0x10;
        const auto owner =
            *reinterpret_cast<const std::uintptr_t*>(r.object + 0x20);
        const auto removed =
            *reinterpret_cast<const std::uint8_t*>(r.object + 0x28);
        if (owner != 0 && removed == 0) {
            const auto vtable =
                *reinterpret_cast<const std::uintptr_t*>(r.object);
            const auto callback =
                *reinterpret_cast<const std::uintptr_t*>(vtable + 0x40);
            r.vtable_offset = vtable >= base ? vtable - base : vtable;
            r.callback_offset = callback >= base ? callback - base : callback;
            std::memcpy(r.object_bytes,
                        reinterpret_cast<const void*>(r.object),
                        sizeof(r.object_bytes));
            std::memcpy(r.vtable_bytes,
                        reinterpret_cast<const void*>(vtable),
                        sizeof(r.vtable_bytes));
            if (callback != 0 && callback >= base) {
                const auto callback_ptr = reinterpret_cast<const void*>(callback);
                if (callback_ptr != nullptr) {
                    std::memcpy(r.callback_bytes, callback_ptr,
                                sizeof(r.callback_bytes));
                }
            }
        }
    }
    g_dump_request.store(1, std::memory_order_release);
}

extern "C" __attribute__((noinline)) void KeyboardActionProbe(
    void* container, const void* event) {
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    Capture(container, event, caller);
    reinterpret_cast<DispatchFn>(g_trampoline.load(std::memory_order_acquire))(
        container, event);
}

bool Dump() {
    std::uint32_t count = g_record_count.load(std::memory_order_acquire);
    if (count > kCapacity) count = kCapacity;
    FILE* file = std::fopen(kOutput, "wb");
    if (!file) return false;
    DumpHeader h{{'A', '9', 'K', 'A', 'P', '1', '\0', '\0'}, {},
                 sizeof(Record),
                 g_guest_base.load(std::memory_order_relaxed),
                 g_keyboard_events.load(std::memory_order_relaxed), count, 0};
    std::memcpy(h.build_id, kExpectedBuildId, sizeof(kExpectedBuildId));
    const bool ok = std::fwrite(&h, sizeof(h), 1, file) == 1 &&
                    (count == 0 ||
                     std::fwrite(g_records, sizeof(Record), count, file) ==
                         count);
    std::fclose(file);
    return ok;
}

void* Reporter(void*) {
    while (true) {
        sleep(1);
        if (!g_dump_request.exchange(0, std::memory_order_acq_rel)) continue;
        const bool ok = Dump();
        __android_log_print(
            ok ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
            "events=%llu records=%u dump=%s path=%s",
            static_cast<unsigned long long>(
                g_keyboard_events.load(std::memory_order_relaxed)),
            g_record_count.load(std::memory_order_relaxed),
            ok ? "ok" : "failed", kOutput);
    }
}

bool Install(std::uintptr_t base) {
    auto* target = reinterpret_cast<std::uint8_t*>(base + kDispatchOffset);
    if (std::memcmp(target, kDispatchSignature, 16) != 0) return false;
    const auto trampoline =
        BuildTrampoline(reinterpret_cast<std::uintptr_t>(target));
    if (!trampoline) return false;
    g_trampoline.store(trampoline, std::memory_order_release);
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch,
                      reinterpret_cast<std::uintptr_t>(&KeyboardActionProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch))
        return false;
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "probe installed target=%p filter_lr=0x%zx", target,
                        kKeyboardReturnOffset);
    pthread_t reporter{};
    if (pthread_create(&reporter, nullptr, Reporter, nullptr) == 0)
        pthread_detach(reporter);
    return true;
}

void* Worker(void*) {
    sleep(120);
    GameMapping mapping{};
    dl_iterate_phdr(FindGuestGameModule, nullptr);
    const auto base = g_guest_base.load(std::memory_order_acquire);
    std::uint8_t build[20]{};
    if (!FindGameMapping(&mapping) || !base ||
        !ReadBuildId(mapping.path, build) ||
        std::memcmp(build, kExpectedBuildId, sizeof(build)) != 0 ||
        access(kEnableMarker, F_OK) != 0 || !Install(base)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "precondition/install failed; probe disabled");
    }
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded passive=1 candidate=keyboard-action-v1");
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, Worker, nullptr) == 0)
        pthread_detach(worker);
}

}  // namespace

extern "C" __attribute__((visibility("default")))
std::uint32_t a9tas_payload_protocol() {
    return 2;
}
