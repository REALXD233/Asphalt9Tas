// Dual-hop probe (10C.2).
//
// 10C.1 result: second-hop dispatcher 0x5baf314 installed (signature matched,
// patch verified intact) but was NEVER executed during manual keys. Two
// hypotheses: (H1) Houdini translation cache created before the +120s patch
// (startup events already ran 0x5baf314); (H2) keyboard path does not route
// through 0x5baf314. This probe distinguishes them by:
//   * installing EARLY (worker sleeps 10s, not 120s) so the patch lands before
//     startup execution could translate either target;
//   * hooking BOTH dispatchers: hop 1 (0x5ba6660, LR-filtered to the keyboard
//     path base+0x6613fe4, proven in 10B) and hop 2 (0x5baf314, all calls).
// Interpretation: hop1 fires + hop2 fires -> H1; hop1 fires + hop2 silent ->
// H2 (keyboard never reaches 0x5baf314); neither -> input did not register.
//
// Off by default: marker /data/local/tmp/a9tas-enable-dual-hop-probe.
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

constexpr const char* kTag = "A9TAS_DUAL_HOP";
constexpr const char* kEnableMarker =
    "/data/local/tmp/a9tas-enable-dual-hop-probe";
constexpr const char* kOutput =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/a9tas-dual-hop.bin";
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

// Hop 1: event dispatcher (keyboard path keeps LR=base+0x6613fe4).
constexpr std::uintptr_t kHop1Offset = 0x5ba6660;
constexpr std::uintptr_t kKeyboardReturnOffset = 0x6613fe4;
constexpr std::uint8_t kHop1Signature[16] = {
    0xff, 0xc3, 0x01, 0xd1, 0xf8, 0x1b, 0x00, 0xf9,
    0xf7, 0x5b, 0x04, 0xa9, 0xf5, 0x53, 0x05, 0xa9,
};
// Hop 2: second-level dispatcher (twin, on the queue-drain thread).
constexpr std::uintptr_t kHop2Offset = 0x5baf314;
constexpr std::uint8_t kHop2Signature[16] = {
    0x00, 0x60, 0x00, 0x91,  // add x0, x0, #0x18
    0x01, 0x00, 0x00, 0x14,  // b +4 (relocatable: whole block copied)
    0xff, 0xc3, 0x01, 0xd1,  // sub sp, sp, #0x70
    0xf8, 0x1b, 0x00, 0xf9,  // str x24, [sp, #0x30]
};

constexpr std::size_t kCapacity = 1024;
constexpr std::size_t kMaxSubs = 8;
using DispatchFn = void (*)(void*, const void*);

struct Subscriber {
    std::uint64_t node;
    std::uint64_t object;
    std::uint64_t owner;
    std::uint8_t removed;
    std::uint64_t refcnt;
    std::uint64_t vtable_offset;
    std::uint64_t callback_offset;
};

struct Record {
    std::uint64_t sequence;
    std::uint32_t hop;  // 1 = first dispatcher (keyboard LR), 2 = second
    std::uint32_t tid;
    std::uint64_t x0;
    std::uint64_t x1;
    std::uint64_t caller;
    // hop-1 fields
    std::uint32_t action;
    std::uint32_t pressed;
    std::uint64_t container;
    std::uint64_t subscriber_count;
    std::uint64_t object;
    std::uint64_t vtable_offset;
    std::uint64_t callback_offset;
    // hop-2 fields
    std::uint64_t head;
    std::uint64_t count_candidate;
    std::uint8_t snapshot[0x80];  // bytes at x0-0x18 .. x0+0x67
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
std::atomic<std::uintptr_t> g_tramp1{0};
std::atomic<std::uintptr_t> g_tramp2{0};
std::atomic<std::uint64_t> g_events{0};
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

inline bool LooksHeap(std::uintptr_t p) {
    return (p & 0xffff000000000000ULL) == 0x7fff000000000000ULL;
}

inline bool LooksLib(std::uintptr_t base, std::uintptr_t p) {
    return p >= base && p < base + 0xa5d8168;
}

Record* NextRecord(std::uint32_t hop) {
    const std::uint64_t sequence =
        g_events.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t slot = g_record_count.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kCapacity) {
        g_record_count.store(kCapacity, std::memory_order_relaxed);
        return nullptr;
    }
    Record* r = &g_records[slot];
    std::memset(r, 0, sizeof(*r));
    r->sequence = sequence;
    r->hop = hop;
    r->tid = static_cast<std::uint32_t>(gettid());
    return r;
}

void CaptureHop1(void* container_pointer, const void* event_pointer,
                 std::uintptr_t caller) {
    const std::uintptr_t base = g_guest_base.load(std::memory_order_relaxed);
    if (base == 0 || caller != base + kKeyboardReturnOffset ||
        !event_pointer)
        return;
    Record* r = NextRecord(1);
    if (!r) return;
    r->x0 = reinterpret_cast<std::uintptr_t>(container_pointer);
    r->x1 = reinterpret_cast<std::uintptr_t>(event_pointer);
    r->caller = caller;
    std::memcpy(&r->action,
                static_cast<const std::uint8_t*>(event_pointer) + 0x10, 4);
    std::memcpy(&r->pressed,
                static_cast<const std::uint8_t*>(event_pointer) + 0x14, 4);
    const auto container = r->x0;
    r->container = container;
    const auto head = *reinterpret_cast<const std::uintptr_t*>(container);
    r->subscriber_count =
        *reinterpret_cast<const std::uint64_t*>(container + 0x10);
    if (r->subscriber_count != 0 && head != 0 && LooksHeap(head)) {
        r->object = head - 0x10;
        const auto owner =
            *reinterpret_cast<const std::uintptr_t*>(r->object + 0x20);
        const auto removed =
            *reinterpret_cast<const std::uint8_t*>(r->object + 0x28);
        if (owner != 0 && removed == 0) {
            const auto vtable =
                *reinterpret_cast<const std::uintptr_t*>(r->object);
            const auto callback =
                *reinterpret_cast<const std::uintptr_t*>(vtable + 0x40);
            r->vtable_offset =
                LooksLib(base, vtable) ? vtable - base : vtable;
            r->callback_offset =
                LooksLib(base, callback) ? callback - base : callback;
        }
    }
    g_dump_request.store(1, std::memory_order_release);
}

void CaptureHop2(void* entry_x0, const void* entry_x1,
                 std::uintptr_t caller) {
    const std::uintptr_t base = g_guest_base.load(std::memory_order_relaxed);
    if (base == 0) return;
    Record* r = NextRecord(2);
    if (!r) return;
    const auto x0 = reinterpret_cast<std::uintptr_t>(entry_x0);
    const auto x1 = reinterpret_cast<std::uintptr_t>(entry_x1);
    r->x0 = x0;
    r->x1 = x1;
    r->caller = caller;

    const auto snapshot_start = x0 >= 0x18 ? x0 - 0x18 : x0;
    if (LooksHeap(snapshot_start) || snapshot_start >= base) {
        std::memcpy(r->snapshot, reinterpret_cast<const void*>(snapshot_start),
                    sizeof(r->snapshot));
    }

    const auto container = x0 + 0x18;
    const auto head = *reinterpret_cast<const std::uintptr_t*>(container);
    const auto count = *reinterpret_cast<const std::uint64_t*>(container + 0x10);
    r->head = head;
    r->count_candidate = count;
    if (count == 0 || count > 256 || !LooksHeap(head)) return;

    auto node = head;
    std::uint32_t sub_idx = 0;
    for (std::uint64_t i = 0; i < count && sub_idx < kMaxSubs; ++i) {
        if (!LooksHeap(node)) break;
        const auto object = node - 0x10;
        Subscriber& s = r->subs[sub_idx];
        s.node = node;
        s.object = object;
        s.owner = *reinterpret_cast<const std::uintptr_t*>(object + 0x20);
        s.removed = *reinterpret_cast<const std::uint8_t*>(object + 0x28);
        s.refcnt = *reinterpret_cast<const std::uint64_t*>(object + 0x30);
        const auto vtable =
            *reinterpret_cast<const std::uintptr_t*>(object);
        s.vtable_offset =
            LooksLib(base, vtable) ? vtable - base : 0;
        if (s.owner != 0 && s.removed == 0) {
            const auto callback =
                *reinterpret_cast<const std::uintptr_t*>(vtable + 0x40);
            s.callback_offset =
                LooksLib(base, callback) ? callback - base : 0;
        }
        ++sub_idx;
        node = *reinterpret_cast<const std::uintptr_t*>(node);
    }
    g_dump_request.store(1, std::memory_order_release);
}

extern "C" __attribute__((noinline)) void DualHopProbe1(void* c, const void* e) {
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    CaptureHop1(c, e, caller);
    reinterpret_cast<DispatchFn>(g_tramp1.load(std::memory_order_acquire))(c, e);
}

extern "C" __attribute__((noinline)) void DualHopProbe2(void* x0, const void* x1) {
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    CaptureHop2(x0, x1, caller);
    reinterpret_cast<DispatchFn>(g_tramp2.load(std::memory_order_acquire))(x0, x1);
}

bool Dump() {
    std::uint32_t count = g_record_count.load(std::memory_order_acquire);
    if (count > kCapacity) count = kCapacity;
    FILE* file = std::fopen(kOutput, "wb");
    if (!file) return false;
    DumpHeader h{{'A', '9', 'D', 'H', 'P', '1', '\0', '\0'}, {},
                 sizeof(Record),
                 g_guest_base.load(std::memory_order_relaxed),
                 g_events.load(std::memory_order_relaxed), count, 0};
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
                g_events.load(std::memory_order_relaxed)),
            g_record_count.load(std::memory_order_relaxed),
            ok ? "ok" : "failed", kOutput);
    }
}

bool InstallOne(std::uintptr_t base, std::uintptr_t offset,
                const std::uint8_t signature[16],
                std::atomic<std::uintptr_t>* tramp_slot,
                void (*probe)(void*, const void*)) {
    auto* target = reinterpret_cast<std::uint8_t*>(base + offset);
    if (std::memcmp(target, signature, 16) != 0) return false;
    const auto trampoline =
        BuildTrampoline(reinterpret_cast<std::uintptr_t>(target));
    if (!trampoline) return false;
    tramp_slot->store(trampoline, std::memory_order_release);
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(probe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch))
        return false;
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    return true;
}

bool Install(std::uintptr_t base) {
    const bool ok1 = InstallOne(base, kHop1Offset, kHop1Signature, &g_tramp1,
                                &DualHopProbe1);
    const bool ok2 = InstallOne(base, kHop2Offset, kHop2Signature, &g_tramp2,
                                &DualHopProbe2);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "probe installed hop1=%d hop2=%d target1=%p target2=%p",
                        ok1 ? 1 : 0, ok2 ? 1 : 0,
                        reinterpret_cast<void*>(base + kHop1Offset),
                        reinterpret_cast<void*>(base + kHop2Offset));
    if (!ok1 && !ok2) return false;
    pthread_t reporter{};
    if (pthread_create(&reporter, nullptr, Reporter, nullptr) == 0)
        pthread_detach(reporter);
    return true;
}

void* Worker(void*) {
    sleep(10);  // EARLY install: before startup events can translate targets
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
                        "loaded passive=1 candidate=dual-hop-v1");
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, Worker, nullptr) == 0)
        pthread_detach(worker);
}

}  // namespace

extern "C" __attribute__((visibility("default")))
std::uint32_t a9tas_payload_protocol() {
    return 2;
}
