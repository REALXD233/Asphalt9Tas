// Second-hop dispatcher probe (10C.1).
//
// Hooks 0x5baf314, the twin of the event dispatcher 0x5ba6660 that runs on the
// queue-drain thread after the keyboard subscriber posts its action closure.
// Entry convention:  add x0,x0,#0x18; b +4; sub sp,sp,#0x70; str x24,[sp,#0x30]
// The relative `b +4` is safe to relocate because the whole 16-byte block is
// copied contiguously (offset preserved). See 10A_STATIC_AUDIT.md section 10C.
//
// Captures every invocation (no LR filter): entry x0/x1, caller LR, tid, a
// 0x80-byte snapshot around x0 (closure candidates) and an attempted
// enumeration of the second-level subscriber list (container = x0+0x18,
// eligibility mirrors acquire 0x363a644: owner[+0x20]!=0 && removed[+0x28]==0,
// callback = [[object]+0x40], next = [node]).
//
// Off by default: enabled only by marker
//   /data/local/tmp/a9tas-enable-second-hop-probe
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

constexpr const char* kTag = "A9TAS_SECOND_HOP";
constexpr const char* kEnableMarker =
    "/data/local/tmp/a9tas-enable-second-hop-probe";
constexpr const char* kOutput =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/a9tas-second-hop-v2.bin";
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

constexpr std::uintptr_t kDispatchOffset = 0x5baf314;
constexpr std::uint8_t kDispatchSignature[16] = {
    0x00, 0x60, 0x00, 0x91,  // add x0, x0, #0x18
    0x01, 0x00, 0x00, 0x14,  // b +4 (relocatable within copied block)
    0xff, 0xc3, 0x01, 0xd1,  // sub sp, sp, #0x70
    0xf8, 0x1b, 0x00, 0xf9,  // str x24, [sp, #0x30]
};

constexpr std::size_t kCapacity = 128;
constexpr std::size_t kMaxSubs = 8;
constexpr std::size_t kMaxReadableRanges = 512;
using DispatchFn = void (*)(void*, const void*);

struct ReadableRange {
    std::uintptr_t begin;
    std::uintptr_t end;
};

struct Subscriber {
    std::uint64_t node;
    std::uint64_t object;
    std::uint64_t owner;
    std::uint8_t removed;
    std::uint64_t refcnt;
    std::uint64_t vtable_offset;
    std::uint64_t callback_offset;
    std::uint8_t object_snapshot[0x80];
    std::uint8_t vtable_snapshot[0x80];
};

struct Record {
    std::uint64_t sequence;
    std::uint64_t x0;
    std::uint64_t x1;
    std::uint64_t caller;
    std::uint32_t tid;
    std::uint32_t sub_count;
    std::uint32_t eligible_count;
    std::uint32_t reserved;
    std::uint64_t head;
    std::uint64_t count_candidate;
    std::uint8_t snapshot[0x80];  // bytes at x0-0x18 .. x0+0x67 (closure area)
    Subscriber subs[kMaxSubs];
};

struct DumpHeader {
    char magic[8];
    std::uint8_t build_id[20];
    std::uint32_t record_size;
    std::uint64_t guest_base;
    std::uint64_t events;
    std::uint32_t record_count;
    std::uint32_t reserved;
};

struct GameMapping {
    std::uintptr_t base{};
    char path[1024]{};
};

std::atomic<std::uintptr_t> g_guest_base{0};
std::atomic<std::uintptr_t> g_trampoline{0};
std::atomic<std::uint64_t> g_events{0};
std::atomic<std::uint32_t> g_next_slot{0};
std::atomic<std::uint32_t> g_dump_request{0};
Record g_records[kCapacity]{};
Record g_dump_records[kCapacity]{};
std::atomic<std::uint8_t> g_ready[kCapacity]{};
ReadableRange g_readable_ranges[kMaxReadableRanges]{};
std::size_t g_readable_range_count = 0;

bool LoadReadableRanges() {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[2048]{};
    std::size_t count = 0;
    while (count < kMaxReadableRanges && std::fgets(line, sizeof(line), maps)) {
        unsigned long long begin = 0, end = 0;
        char permissions[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &begin, &end, permissions) ==
                3 &&
            permissions[0] == 'r' && begin < end) {
            g_readable_ranges[count++] = {
                static_cast<std::uintptr_t>(begin),
                static_cast<std::uintptr_t>(end),
            };
        }
    }
    std::fclose(maps);
    g_readable_range_count = count;
    return count != 0;
}

bool IsReadable(std::uintptr_t address, std::size_t size) {
    if (!address || !size || address > UINTPTR_MAX - size) return false;
    const auto end = address + size;
    for (std::size_t i = 0; i < g_readable_range_count; ++i) {
        if (address >= g_readable_ranges[i].begin &&
            end <= g_readable_ranges[i].end)
            return true;
    }
    return false;
}

std::uint32_t PublishedRecordCount() {
    std::uint32_t claimed = g_next_slot.load(std::memory_order_acquire);
    if (claimed > kCapacity) claimed = kCapacity;
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < claimed; ++i) {
        if (g_ready[i].load(std::memory_order_acquire)) ++count;
    }
    return count;
}

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

void Capture(void* entry_x0, const void* entry_x1, std::uintptr_t caller) {
    const std::uintptr_t base = g_guest_base.load(std::memory_order_relaxed);
    if (!base) return;
    const auto x0 = reinterpret_cast<std::uintptr_t>(entry_x0);
    const auto x1 = reinterpret_cast<std::uintptr_t>(entry_x1);
    const std::uint64_t sequence =
        g_events.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t slot = g_next_slot.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kCapacity) {
        return;
    }

    Record& r = g_records[slot];
    r.sequence = sequence;
    r.x0 = x0;
    r.x1 = x1;
    r.caller = caller;
    r.tid = static_cast<std::uint32_t>(gettid());
    r.sub_count = 0;
    r.eligible_count = 0;
    r.head = 0;
    r.count_candidate = 0;

    // Snapshot closure candidates: x0-0x18 .. x0+0x67 (covers container too).
    const auto snapshot_start = x0 >= 0x18 ? x0 - 0x18 : x0;
    std::memset(r.snapshot, 0, sizeof(r.snapshot));
    if (IsReadable(snapshot_start, sizeof(r.snapshot))) {
        std::memcpy(r.snapshot, reinterpret_cast<const void*>(snapshot_start),
                    sizeof(r.snapshot));
    }

    // Second-level container = x0+0x18 (twin's adjusted this). Enumerate only
    // if head/count look sane; strict mirror of the twin dispatcher.
    const auto container = x0 + 0x18;
    if (!IsReadable(container, 0x18)) {
        g_ready[slot].store(1, std::memory_order_release);
        g_dump_request.store(1, std::memory_order_release);
        return;
    }
    const auto head = *reinterpret_cast<const std::uintptr_t*>(container);
    const auto count = *reinterpret_cast<const std::uint64_t*>(container + 0x10);
    r.head = head;
    r.count_candidate = count;
    if (count == 0 || count > 256 || !IsReadable(head, sizeof(std::uintptr_t))) {
        g_ready[slot].store(1, std::memory_order_release);
        g_dump_request.store(1, std::memory_order_release);
        return;
    }

    auto node = head;
    for (std::uint64_t i = 0; i < count && r.sub_count < kMaxSubs; ++i) {
        if (node < 0x10 || !IsReadable(node - 0x10, 0x80)) break;
        const auto object = node - 0x10;
        Subscriber& s = r.subs[r.sub_count];
        s.node = node;
        s.object = object;
        s.owner = *reinterpret_cast<const std::uintptr_t*>(object + 0x20);
        s.removed = *reinterpret_cast<const std::uint8_t*>(object + 0x28);
        s.refcnt = *reinterpret_cast<const std::uint64_t*>(object + 0x30);
        const auto vtable =
            *reinterpret_cast<const std::uintptr_t*>(object);
        std::memcpy(s.object_snapshot, reinterpret_cast<const void*>(object),
                    sizeof(s.object_snapshot));
        s.vtable_offset =
            (vtable >= base && vtable < base + 0xa5d8168) ? vtable - base : 0;
        if (IsReadable(vtable, sizeof(s.vtable_snapshot))) {
            std::memcpy(s.vtable_snapshot,
                        reinterpret_cast<const void*>(vtable),
                        sizeof(s.vtable_snapshot));
        }
        if (s.owner != 0 && s.removed == 0 &&
            vtable <= UINTPTR_MAX - 0x40 &&
            IsReadable(vtable + 0x40, sizeof(std::uintptr_t))) {
            ++r.eligible_count;
            const auto callback =
                *reinterpret_cast<const std::uintptr_t*>(vtable + 0x40);
            s.callback_offset = (callback >= base && callback < base + 0xa5d8168)
                                    ? callback - base
                                    : 0;
        }
        ++r.sub_count;
        node = *reinterpret_cast<const std::uintptr_t*>(node);
    }
    g_ready[slot].store(1, std::memory_order_release);
    g_dump_request.store(1, std::memory_order_release);
}

extern "C" __attribute__((noinline)) void SecondHopProbe(
    void* x0, const void* x1) {
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    Capture(x0, x1, caller);
    reinterpret_cast<DispatchFn>(g_trampoline.load(std::memory_order_acquire))(
        x0, x1);
}

bool Dump() {
    std::uint32_t claimed = g_next_slot.load(std::memory_order_acquire);
    if (claimed > kCapacity) claimed = kCapacity;
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < claimed; ++i) {
        if (!g_ready[i].load(std::memory_order_acquire)) continue;
        g_dump_records[count++] = g_records[i];
    }
    FILE* file = std::fopen(kOutput, "wb");
    if (!file) return false;
    DumpHeader h{{'A', '9', 'S', 'H', 'P', '2', '\0', '\0'}, {},
                 sizeof(Record),
                 g_guest_base.load(std::memory_order_relaxed),
                 g_events.load(std::memory_order_relaxed), count, 0};
    std::memcpy(h.build_id, kExpectedBuildId, sizeof(kExpectedBuildId));
    const bool ok = std::fwrite(&h, sizeof(h), 1, file) == 1 &&
                    (count == 0 ||
                     std::fwrite(g_dump_records, sizeof(Record), count, file) ==
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
                g_events.load(std::memory_order_relaxed)),
            PublishedRecordCount(),
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
                      reinterpret_cast<std::uintptr_t>(&SecondHopProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch))
        return false;
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "probe installed target=%p (second-hop dispatcher)",
                        target);
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
        !ReadBuildId(mapping.path, build) || !LoadReadableRanges() ||
        std::memcmp(build, kExpectedBuildId, sizeof(build)) != 0 ||
        access(kEnableMarker, F_OK) != 0 || !Install(base)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "precondition/install failed; probe disabled");
    }
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded passive=1 candidate=second-hop-v2");
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, Worker, nullptr) == 0)
        pthread_detach(worker);
}

}  // namespace

extern "C" __attribute__((visibility("default")))
std::uint32_t a9tas_payload_protocol() {
    return 2;
}
