// Gateway probe (10C.3, R1-fixed).
//
// Codex audit fixes applied (see CODEX_AUDIT_CORRECTIONS_FOR_DEEPSEEK_20260813.md):
//  F1: trampoline now relocates out-of-block CBZ/B.cond targets that converge
//      on block_end (rewrite imm19=1 so both paths land on the abs-jump thunk);
//      unsupported out-of-block branches fail the install instead of copying.
//  F2: event flag renamed released_flag (was pressed); the closure carries it
//      via a 64-bit copy (0x5baf6c8 ldr x8,[x4,#0x10]; 0x5baf6d0 str x8,[x0,#0x40]).
//  F3: 0x35f79cc is a NO-PARAM notifier; GenericNotifyProbe no longer records
//      w1 as a semantic value.
//  F4: EnumerateList uses a /proc/self/maps built readable-range table and
//      strict acquire/release (signature-verified) around subscriber reads.
//  F5: Install is transactional: pre-check all, build all, write+readback each,
//      rollback in reverse on any failure.
//
// Hooks: gateway 0x385a0e8 / +0x120 list 0x385a800 / +0x170 list 0x3680cc0 /
// generic 0x35f79cc (caller-filtered) / first dispatcher 0x5ba6660.
// Off by default: marker /data/local/tmp/a9tas-enable-gateway-probe
// NOTE: DO NOT DEPLOY before static re-review (Codex R1 gate).
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

constexpr const char* kTag = "A9TAS_GATEWAY";
constexpr const char* kEnableMarker =
    "/data/local/tmp/a9tas-enable-gateway-probe";
constexpr const char* kOutput =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/a9tas-gateway.bin";
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

// Gateway hook point: 0x385a104 (downstream basic block, cbz-taken path).
// The 0x385a0e8 entry itself is NOT used: its cbz target lies outside a
// 16-byte copied block (0x385a104) while the not-taken path is the
// early-return (0x385a0f8), so the paths do NOT converge — copying the entry
// would require a two-thunk trampoline. The downstream block has no
// PC-relative instructions (Codex preferred option). Consequence: events with
// released_flag != 0 (early return, no action processing) are not captured —
// that path does not process actions, so no state-relevant information is
// lost.
constexpr std::uint8_t kSigGateway[16] = {
    0x28, 0x10, 0x40, 0xb9,  // ldr w8, [x1, #0x10]
    0xf4, 0x03, 0x01, 0xaa,  // mov x20, x1
    0xf3, 0x03, 0x00, 0xaa,  // mov x19, x0
    0x1f, 0x1d, 0x00, 0x71,  // cmp w8, #7
};
constexpr std::uint8_t kSigNotifier[16] = {
    0xff, 0x83, 0x01, 0xd1, 0xf7, 0x5b, 0x03, 0xa9,
    0xf5, 0x53, 0x04, 0xa9, 0xf3, 0x7b, 0x05, 0xa9,
};
constexpr std::uint8_t kSigDispatcher[16] = {
    0xff, 0xc3, 0x01, 0xd1, 0xf8, 0x1b, 0x00, 0xf9,
    0xf7, 0x5b, 0x04, 0xa9, 0xf5, 0x53, 0x05, 0xa9,
};
// acquire/release/finalize of the first-level list (and twin) — verified at
// install time; used for strict lifecycle protection in enumeration.
constexpr std::uint8_t kSigAcquire[12] = {
    0x08, 0x00, 0x40, 0xf9, 0x09, 0x10, 0x40, 0xf9,
    0x08, 0x01, 0x00, 0x91,
};
constexpr std::uint8_t kSigRelease[8] = {
    0x08, 0x00, 0x40, 0xf9, 0x08, 0x0c, 0x00, 0xf1,
};

constexpr std::uintptr_t kGatewayCallSiteA = 0x385a160;
constexpr std::uintptr_t kGatewayCallSiteB = 0x385a1ac;
constexpr std::uintptr_t kAcquireOffset = 0x35fc578;
constexpr std::uintptr_t kReleaseOffset = 0x35fc5a4;

constexpr std::size_t kCapacity = 2048;
constexpr std::size_t kMaxSubs = 16;
constexpr std::uintptr_t kLibSize = 0xa5d8168;

enum HookId : std::uint32_t {
    kHookGateway = 1,
    kHookList120 = 2,
    kHookList170 = 3,
    kHookGeneric = 4,
    kHookDispatch = 5,
};

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
    std::uint32_t hook;
    std::uint32_t tid;
    std::uint64_t x0;
    std::uint64_t x1;
    std::uint64_t caller;
    std::uint32_t action;       // gateway: [x1+0x10]
    std::uint32_t released_flag; // gateway: [x1+0x14] (0=down/held, 1=release)
    std::uint32_t w1;           // notifiers with explicit param (list120/170)
    std::uint32_t reserved;
    std::uint64_t head;
    std::uint64_t count_candidate;
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

std::atomic<std::uintptr_t> g_tramp[5]{};
std::atomic<std::uint64_t> g_events{0};
std::atomic<std::uint32_t> g_record_count{0};
std::atomic<std::uint32_t> g_dump_request{0};
std::atomic<std::uintptr_t> g_guest_base{0};
Record g_records[kCapacity]{};

// Readable guest ranges built once in Worker from /proc/self/maps.
struct Range {
    std::uintptr_t start;
    std::uintptr_t end;
};
Range g_readable[64]{};
std::size_t g_readable_count{0};

using DispatchFn = void (*)(void*, void*);
using NotifyFn = void (*)(void*, std::uint32_t);
using AcquireFn = std::uint32_t (*)(void*);
using ReleaseFn = void (*)(void*);
AcquireFn g_acquire{nullptr};
ReleaseFn g_release{nullptr};

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

void BuildReadableRanges() {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return;
    char line[2048]{};
    g_readable_count = 0;
    while (std::fgets(line, sizeof(line), maps) &&
           g_readable_count < 64) {
        unsigned long long start = 0, end = 0;
        char perms[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &start, &end, perms) == 3 &&
            perms[0] == 'r') {
            g_readable[g_readable_count].start =
                static_cast<std::uintptr_t>(start);
            g_readable[g_readable_count].end =
                static_cast<std::uintptr_t>(end);
            ++g_readable_count;
        }
    }
    std::fclose(maps);
}

inline bool IsReadable(std::uintptr_t p, std::size_t n) {
    for (std::size_t i = 0; i < g_readable_count; ++i) {
        if (p >= g_readable[i].start &&
            p + n <= g_readable[i].end &&
            p + n >= p)  // no overflow
            return true;
    }
    return false;
}

inline bool LooksLib(std::uintptr_t base, std::uintptr_t p) {
    return p >= base && p < base + kLibSize;
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

bool ReadProcMem(std::uintptr_t address, std::uint8_t bytes[16]) {
    const int fd = open("/proc/self/mem", O_RDONLY);
    if (fd < 0) return false;
    const ssize_t n = pread(fd, bytes, 16, static_cast<off_t>(address));
    close(fd);
    return n == 16;
}

// F1: relocate out-of-block PC-relative branches inside a copied 16-byte
// block. Supported: branches whose target == block_end (target+16); those are
// rewritten to jump to the thunk at +0x10 (imm19/imm26 = 1). Anything else
// with an out-of-block target fails the install.
bool RelocateBlock(std::uint8_t block[16], std::uintptr_t target) {
    for (std::size_t off = 0; off < 16; off += 4) {
        std::uint32_t w;
        std::memcpy(&w, block + off, 4);
        std::uint32_t kind = w & 0x7c000000;
        const bool is_cbz = (w & 0x7f000000) == 0x34000000;
        const bool is_cbnz = (w & 0x7f000000) == 0x35000000;
        const bool is_tbz = (w & 0x7e000000) == 0x36000000;
        const bool is_cond = (w & 0xff000010) == 0x54000000;
        const bool is_b = (w & 0xfc000000) == 0x14000000;
        const bool is_bl = (w & 0xfc000000) == 0x94000000;
        if (!is_cbz && !is_cbnz && !is_tbz && !is_cond && !is_b && !is_bl)
            continue;
        std::int64_t disp = 0;
        if (is_cbz || is_cbnz) {
            disp = static_cast<std::int32_t>(w & 0x00ffffe0) >> 5;  // 19-bit
        } else if (is_tbz) {
            disp = static_cast<std::int32_t>(w & 0x0007ffe0) >> 5;  // 14-bit
        } else if (is_cond) {
            disp = static_cast<std::int32_t>(w & 0x00ffffe0) >> 5;  // 19-bit
        } else {
            disp = static_cast<std::int32_t>(w & 0x03ffffff) << 2;  // 26-bit
            disp >>= 2;
        }
        const std::uintptr_t pc = target + off;
        const std::uintptr_t branch_target =
            pc + static_cast<std::uintptr_t>(disp << 2);
        const bool in_block =
            branch_target >= target && branch_target < target + 16;
        if (in_block) continue;
        if (branch_target != target + 16) return false;  // unsupported
        // Both paths converge on block_end: rewrite to +0x10 (imm = 1).
        w &= 0xff00001f;
        w |= 1u << 5;
        if (is_tbz) {
            w &= 0xfff8001f;  // tbz/tbnz: imm14 at [18:5]
            w |= 1u << 5;
        }
        std::memcpy(block + off, &w, 4);
    }
    return true;
}

// F5: trampoline build with relocation validation.
std::uintptr_t BuildTrampoline(std::uintptr_t target) {
    auto* stub = static_cast<std::uint8_t*>(
        mmap(nullptr, 32, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (stub == MAP_FAILED) return 0;
    std::memcpy(stub, reinterpret_cast<const void*>(target), 16);
    if (!RelocateBlock(stub, target)) {
        munmap(stub, 32);
        return 0;
    }
    WriteAbsoluteJump(stub + 16, target + 16);
    __builtin___clear_cache(reinterpret_cast<char*>(stub),
                            reinterpret_cast<char*>(stub + 32));
    if (mprotect(stub, 32, PROT_READ | PROT_EXEC) != 0) {
        munmap(stub, 32);
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(stub);
}

void EnumerateList(Record* r, std::uintptr_t container, std::uintptr_t base) {
    if (!IsReadable(container, 0x18)) return;
    const auto head = *reinterpret_cast<const std::uintptr_t*>(container);
    const auto count =
        *reinterpret_cast<const std::uint64_t*>(container + 0x10);
    r->head = head;
    r->count_candidate = count;
    if (count == 0 || count > 64 || !IsReadable(head, 0x18)) return;
    auto node = head;
    std::uint32_t sub_idx = 0;
    for (std::uint64_t i = 0; i < count && sub_idx < kMaxSubs; ++i) {
        if (!IsReadable(node, 0x18)) break;
        const auto object = node - 0x10;
        if (!IsReadable(object, 0x38)) break;
        // F4: strict lifecycle protection — acquire before reading, release
        // after. Mirrors the dispatcher; refcount keeps the object alive.
        if (!g_acquire || !g_release || g_acquire(reinterpret_cast<void*>(object)) == 0) {
            node = *reinterpret_cast<const std::uintptr_t*>(node);
            continue;
        }
        Subscriber& s = r->subs[sub_idx];
        s.node = node;
        s.object = object;
        s.owner = *reinterpret_cast<const std::uintptr_t*>(object + 0x20);
        s.removed = *reinterpret_cast<const std::uint8_t*>(object + 0x28);
        s.refcnt = *reinterpret_cast<const std::uint64_t*>(object + 0x30);
        const auto vtable =
            *reinterpret_cast<const std::uintptr_t*>(object);
        s.vtable_offset = LooksLib(base, vtable) ? vtable - base : 0;
        if (s.owner != 0 && s.removed == 0 && IsReadable(vtable + 0x40, 8)) {
            const auto callback =
                *reinterpret_cast<const std::uintptr_t*>(vtable + 0x40);
            s.callback_offset =
                LooksLib(base, callback) ? callback - base : 0;
        }
        ++sub_idx;
        g_release(reinterpret_cast<void*>(object));
        node = *reinterpret_cast<const std::uintptr_t*>(node);
    }
}

Record* NextRecord(std::uint32_t hook) {
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
    r->hook = hook;
    r->tid = static_cast<std::uint32_t>(gettid());
    return r;
}

void CaptureGateway(void* x0v, void* x1v, std::uintptr_t caller) {
    const std::uintptr_t base = g_guest_base.load(std::memory_order_relaxed);
    if (!base || !x1v) return;
    Record* r = NextRecord(kHookGateway);
    if (!r) return;
    r->x0 = reinterpret_cast<std::uintptr_t>(x0v);
    r->x1 = reinterpret_cast<std::uintptr_t>(x1v);
    r->caller = caller;
    std::memcpy(&r->action, static_cast<const std::uint8_t*>(x1v) + 0x10, 4);
    std::memcpy(&r->released_flag,
                static_cast<const std::uint8_t*>(x1v) + 0x14, 4);
    g_dump_request.store(1, std::memory_order_release);
}

void CaptureNotifier(void* x0v, std::uint32_t w1, std::uint32_t hook,
                     std::uintptr_t caller) {
    const std::uintptr_t base = g_guest_base.load(std::memory_order_relaxed);
    if (!base) return;
    Record* r = NextRecord(hook);
    if (!r) return;
    r->x0 = reinterpret_cast<std::uintptr_t>(x0v);
    r->w1 = w1;
    r->caller = caller;
    EnumerateList(r, r->x0, base);
    g_dump_request.store(1, std::memory_order_release);
}

void CaptureDispatch(void* x0v, void* x1v, std::uintptr_t caller) {
    const std::uintptr_t base = g_guest_base.load(std::memory_order_relaxed);
    if (!base) return;
    Record* r = NextRecord(kHookDispatch);
    if (!r) return;
    r->x0 = reinterpret_cast<std::uintptr_t>(x0v);
    r->x1 = reinterpret_cast<std::uintptr_t>(x1v);
    r->caller = caller;
    EnumerateList(r, r->x0, base);
    g_dump_request.store(1, std::memory_order_release);
}

extern "C" __attribute__((noinline)) void GatewayProbe(void* x0, void* x1) {
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    CaptureGateway(x0, x1, caller);
    reinterpret_cast<DispatchFn>(g_tramp[0].load(std::memory_order_acquire))(
        x0, x1);
}

extern "C" __attribute__((noinline)) void Notifier120Probe(void* x0,
                                                          std::uint32_t w1) {
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    CaptureNotifier(x0, w1, kHookList120, caller);
    reinterpret_cast<NotifyFn>(g_tramp[1].load(std::memory_order_acquire))(x0,
                                                                           w1);
}

extern "C" __attribute__((noinline)) void Notifier170Probe(void* x0,
                                                          std::uint32_t w1) {
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    CaptureNotifier(x0, w1, kHookList170, caller);
    reinterpret_cast<NotifyFn>(g_tramp[2].load(std::memory_order_acquire))(x0,
                                                                           w1);
}

// F3: 0x35f79cc is a NO-PARAM notifier; w1 has no reliable meaning here.
extern "C" __attribute__((noinline)) void GenericNotifyProbe(void* x0,
                                                            std::uint32_t) {
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const std::uintptr_t base = g_guest_base.load(std::memory_order_relaxed);
    if (base && (caller == base + kGatewayCallSiteA ||
                 caller == base + kGatewayCallSiteB)) {
        CaptureNotifier(x0, 0, kHookGeneric, caller);
    }
    reinterpret_cast<NotifyFn>(g_tramp[3].load(std::memory_order_acquire))(x0,
                                                                           0);
}

extern "C" __attribute__((noinline)) void DispatchProbe(void* x0, void* x1) {
    const auto caller =
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    CaptureDispatch(x0, x1, caller);
    reinterpret_cast<DispatchFn>(g_tramp[4].load(std::memory_order_acquire))(
        x0, x1);
}

bool Dump() {
    std::uint32_t count = g_record_count.load(std::memory_order_acquire);
    if (count > kCapacity) count = kCapacity;
    FILE* file = std::fopen(kOutput, "wb");
    if (!file) return false;
    DumpHeader h{{'A', '9', 'G', 'W', 'Y', '1', '\0', '\0'}, {},
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

// F5: transactional install — pre-check all, build all, write+readback each,
// rollback in reverse on any failure. No reporter until all succeeded.
struct HookPlan {
    std::uintptr_t offset;
    const std::uint8_t* signature;
    std::size_t tramp_index;
    void* probe;
};

bool Install(std::uintptr_t base) {
    pthread_t reporter{};  // created only on full success
    const HookPlan plans[5] = {
        {0x385a104, kSigGateway, 0, reinterpret_cast<void*>(&GatewayProbe)},
        {0x385a800, kSigNotifier, 1, reinterpret_cast<void*>(&Notifier120Probe)},
        {0x3680cc0, kSigNotifier, 2, reinterpret_cast<void*>(&Notifier170Probe)},
        {0x35f79cc, kSigNotifier, 3, reinterpret_cast<void*>(&GenericNotifyProbe)},
        {0x5ba6660, kSigDispatcher, 4, reinterpret_cast<void*>(&DispatchProbe)},
    };
    // 1. Pre-check signatures + acquire/release helpers.
    for (const auto& p : plans) {
        auto* target = reinterpret_cast<std::uint8_t*>(base + p.offset);
        if (std::memcmp(target, p.signature, 16) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "precheck signature mismatch at %p",
                                target);
            return false;
        }
    }
    auto* acquire_raw =
        reinterpret_cast<std::uint8_t*>(base + kAcquireOffset);
    auto* release_raw =
        reinterpret_cast<std::uint8_t*>(base + kReleaseOffset);
    if (std::memcmp(acquire_raw, kSigAcquire, sizeof(kSigAcquire)) != 0 ||
        std::memcmp(release_raw, kSigRelease, sizeof(kSigRelease)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "precheck acquire/release signature mismatch");
        return false;
    }
    g_acquire = reinterpret_cast<AcquireFn>(acquire_raw);
    g_release = reinterpret_cast<ReleaseFn>(release_raw);

    // 2. Build all trampolines first (relocation validated inside).
    std::uintptr_t trampolines[5]{};
    for (std::size_t i = 0; i < 5; ++i) {
        trampolines[i] =
            BuildTrampoline(base + plans[i].offset);
        if (!trampolines[i]) {
            for (std::size_t j = 0; j < i; ++j)
                munmap(reinterpret_cast<void*>(trampolines[j]), 32);
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "trampoline build/relocate failed at index %zu",
                                i);
            return false;
        }
    }

    // 3. Write + readback each; rollback in reverse on failure.
    std::uint8_t originals[5][16]{};
    std::uint8_t patch[16]{};
    std::size_t written = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        auto* target = reinterpret_cast<std::uint8_t*>(base + plans[i].offset);
        if (!ReadProcMem(reinterpret_cast<std::uintptr_t>(target),
                         originals[i])) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "readback-original failed at %p", target);
            goto rollback;
        }
        WriteAbsoluteJump(patch, trampolines[i]);
        if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "write failed at %p", target);
            goto rollback;
        }
        std::uint8_t verify[16]{};
        if (!ReadProcMem(reinterpret_cast<std::uintptr_t>(target), verify) ||
            std::memcmp(verify, patch, 16) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "readback-verify failed at %p", target);
            goto rollback;
        }
        ++written;
    }
    for (std::size_t i = 0; i < 5; ++i)
        g_tramp[i].store(trampolines[i], std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "probe installed (all 5 hooks transactional)");
    if (pthread_create(&reporter, nullptr, Reporter, nullptr) == 0)
        pthread_detach(reporter);
    return true;

rollback:
    for (std::size_t i = written; i-- > 0;) {
        auto* target = reinterpret_cast<std::uint8_t*>(base + plans[i].offset);
        WriteProcMem(reinterpret_cast<std::uintptr_t>(target), originals[i]);
    }
    for (std::size_t i = 0; i < 5; ++i)
        if (trampolines[i])
            munmap(reinterpret_cast<void*>(trampolines[i]), 32);
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "install rolled back (%zu of 5 written)", written);
    return false;
}

void* Worker(void*) {
    sleep(10);  // EARLY install (Houdini translation-cache lesson, 10C.2)
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
        return nullptr;
    }
    BuildReadableRanges();
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded passive=1 candidate=gateway-v2-r1");
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, Worker, nullptr) == 0)
        pthread_detach(worker);
}

}  // namespace

extern "C" __attribute__((visibility("default")))
std::uint32_t a9tas_payload_protocol() {
    return 2;
}
