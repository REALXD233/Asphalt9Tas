// A9TAS layer-3 observation-only tick probe (ARM64 guest payload).
//
// Purpose: prove UpdatePerTick thread / frequency / event order and observe
// the values that actually flow through the control chain. This payload NEVER
// writes a control value. The only writes it performs are:
//   1. 12-byte inline observation patches at the seven verified prologue
//      signatures below (each patch is restored byte-for-byte by nothing else
//      in this version; the trampoline executes the displaced 12 bytes),
//   2. a binary dump file in the game's files directory.
//
// Hook points (offsets from libAsphalt9.so guest base, BuildID e5dd7ef2...):
//   0x386b1e0 GameplayInputController_UpdatePerTick      (tick anchor)
//   0x385a7d0 KeyboardAxisSource_GetValueA               (entry order only)
//   0x385a7ec KeyboardAxisSource_GetValueB               (entry order only)
//   0x367b1f0 VehicleControlSink_SetValueB               (x1 -> float)
//   0x367b21c VehicleControlSink_SetValueA               (x1 -> float)
//   0x36934b8 VehicleDynamics_SetValueC98_Vtable2A8      (x1 -> float)
//   0x36934e0 VehicleDynamics_SetValueC9C_Vtable2B0      (x1 -> float)
//
// Enabled only when /data/local/tmp/a9tas-enable-tick-observer exists.
#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr const char* kTag = "A9TAS_TICK_OBS";
constexpr const char* kEnableMarker = "/data/local/tmp/a9tas-enable-tick-observer";
constexpr const char* kConfigPath = "/data/local/tmp/a9tas-tick-observer.cfg";
constexpr const char* kOutput =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-tick-observer-v1.bin";
constexpr const char* kIdentityOutput =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-g3-begin-identity-v1.txt";
constexpr const char* kArmReceipt =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-g3-begin-identity-arm-v1.txt";
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

constexpr std::size_t kCapacity = 16384;

enum class EventKind : std::uint16_t {
    kTick = 1,
    kGetterA = 2,
    kGetterB = 3,
    kSinkB = 4,
    kSinkA = 5,
    kFinalC98 = 6,
    kFinalC9C = 7,
};

struct Record {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::uint64_t x0;
    std::uint64_t x1;
    std::uint64_t caller;
    std::uint32_t tid;
    std::uint16_t kind;
    std::uint16_t flags;      // bit0: value readable, bit1: x0 word readable
    float value;              // setter input float (kinds 4..7)
    std::uint32_t x0_word;    // first word of x0 (vtable pointer) when readable
    std::uint32_t reserved;
};

static_assert(sizeof(Record) == 64, "keep Record small and stable");

struct DumpHeader {
    char magic[8];            // "A9TOBSV1"
    std::uint8_t build_id[20];
    std::uint32_t record_size;
    std::uint32_t record_count;
    std::uint64_t guest_base;
    std::uint64_t events;
    std::uint64_t dropped;
    std::uint64_t reserved[4];
};

struct GameMapping {
    std::uintptr_t base{};
    char path[1024]{};
};

struct ReadableRange {
    std::uintptr_t begin;
    std::uintptr_t end;
};

struct HookSpec {
    std::uint32_t offset;
    EventKind kind;
    const char* name;
    std::uint8_t signature[12];
};

// NOTE: getter hooks (0x385a7d0/0x385a7ec) were deliberately removed from the
// first dynamic build. They use a custom out-of-line return ABI with the
// return pointer in x8, and a C++ probe clobbers x8 across the Capture call.
// Tick/sink/final only need x0/x1, which are explicitly restored before the
// trampoline branch (see JumpToTrampoline2 below).
constexpr HookSpec kHooks[] = {
    {0x386b1e0, EventKind::kTick, "UpdatePerTick",
     {0xff, 0x43, 0x01, 0xd1, 0xf4, 0x1b, 0x00, 0xf9, 0xf3, 0x7b, 0x04, 0xa9}},
    {0x367b1f0, EventKind::kSinkB, "SinkB",
     {0x08, 0x00, 0x40, 0xf9, 0x29, 0x00, 0x40, 0xb9, 0x08, 0x01, 0x5d, 0xf8}},
    {0x367b21c, EventKind::kSinkA, "SinkA",
     {0x08, 0x00, 0x40, 0xf9, 0x29, 0x00, 0x40, 0xb9, 0x08, 0x81, 0x5c, 0xf8}},
    {0x36934b8, EventKind::kFinalC98, "FinalC98",
     {0x08, 0x00, 0x40, 0xf9, 0x29, 0x00, 0x40, 0xb9, 0x08, 0x61, 0x0b, 0xd1}},
    {0x36934e0, EventKind::kFinalC9C, "FinalC9C",
     {0x08, 0x00, 0x40, 0xf9, 0x29, 0x00, 0x40, 0xb9, 0x08, 0x81, 0x0b, 0xd1}},
};

constexpr std::size_t kHookCount = sizeof(kHooks) / sizeof(kHooks[0]);

std::atomic<std::uintptr_t> g_guest_base{0};
// Plain array read by the naked probe tails (`ldr x17, [x17]`). Written
// BEFORE the corresponding target patch, and never changed afterwards.
extern "C" std::uintptr_t g_tick_observer_continue[kHookCount] = {};
std::atomic<std::uint64_t> g_events{0};
std::atomic<std::uint64_t> g_dropped{0};
std::atomic<std::uint32_t> g_next_slot{0};
std::atomic<std::uint32_t> g_dump_request{0};
Record g_records[kCapacity]{};
Record g_dump_records[kCapacity]{};
std::atomic<std::uint8_t> g_ready[kCapacity]{};
// First real UpdatePerTick entry identity.  The gameplay thread only publishes
// register values; the reporter thread performs all /proc/self/mem reads and
// file I/O after the entry has returned to the original code path.
std::atomic<std::uint32_t> g_identity_state{0};  // 0 empty, 1 claim, 2 ready, 3 dumped
std::uintptr_t g_identity_x0 = 0;
std::uintptr_t g_identity_x1 = 0;
std::uintptr_t g_identity_caller = 0;
std::uint32_t g_identity_tid = 0;
ReadableRange g_readable_ranges[512]{};
std::size_t g_readable_range_count = 0;

std::uint64_t MonotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

bool LoadReadableRanges() {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[2048]{};
    std::size_t count = 0;
    while (count < 512 && std::fgets(line, sizeof(line), maps)) {
        unsigned long long begin = 0, end = 0;
        char permissions[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &begin, &end, permissions) == 3 &&
            permissions[0] == 'r' && begin < end) {
            g_readable_ranges[count++] = {static_cast<std::uintptr_t>(begin),
                                          static_cast<std::uintptr_t>(end)};
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

// Optional config file, one `key=value` per line:
//   delay_ms=<n>   wait after libAsphalt9.so is mapped before installing
//   mask=0x1f      bit i enables kHooks[i] (LSB = UpdatePerTick)
void ReadConfig(int* delay_ms, std::uint32_t* mask) {
    *delay_ms = 30000;
    *mask = 0x1f;
    FILE* file = std::fopen(kConfigPath, "re");
    if (!file) return;
    char line[128]{};
    while (std::fgets(line, sizeof(line), file)) {
        unsigned long long value = 0;
        if (std::sscanf(line, "delay_ms=%llu", &value) == 1) {
            if (value <= 300000) *delay_ms = static_cast<int>(value);
        } else if (std::sscanf(line, "mask=%llx", &value) == 1) {
            *mask = static_cast<std::uint32_t>(value & 0x1f);
        }
    }
    std::fclose(file);
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

// 16-byte absolute jump: ldr x17,#8; br x17; .quad destination. No range
// limit, used for stubs and trampoline tail jumps.
void EncodeAbsoluteJump16(std::uint8_t out[16], std::uintptr_t destination) {
    constexpr std::uint32_t ldr = 0x58000051;  // ldr x17, #8
    constexpr std::uint32_t br = 0xd61f0220;   // br x17
    std::memcpy(out, &ldr, 4);
    std::memcpy(out + 4, &br, 4);
    std::memcpy(out + 8, &destination, 8);
}

bool WriteProcMem(std::uintptr_t address, const std::uint8_t* bytes,
                  std::size_t size) {
    const int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) return false;
    const ssize_t n = pwrite(fd, bytes, size, static_cast<off_t>(address));
    close(fd);
    return n == static_cast<ssize_t>(size);
}

bool ReadProcMem(std::uintptr_t address, void* buffer, std::size_t size) {
    const int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t n = pread(fd, buffer, size, static_cast<off_t>(address));
    close(fd);
    return n == static_cast<ssize_t>(size);
}

// b <target> (26-bit signed word offset), used for the 4-byte entry patch.
bool EncodeBranchImm(std::uint8_t out[4], std::uintptr_t pc,
                     std::uintptr_t target) {
    const std::int64_t delta =
        (static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc)) >> 2;
    if (delta < -(std::int64_t{1} << 25) ||
        delta >= (std::int64_t{1} << 25))
        return false;
    const std::uint32_t insn =
        0x14000000u | (static_cast<std::uint32_t>(delta) & 0x03ffffffu);
    std::memcpy(out, &insn, 4);
    return true;
}

// Trampoline that replays exactly the first 4 bytes of the target, then
// absolutely jumps back to target+4. The 4-byte entry patch never overlaps
// the next function, regardless of function layout.
std::uintptr_t BuildTrampoline4(std::uintptr_t target) {
    auto* code = static_cast<std::uint8_t*>(
        mmap(nullptr, 32, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == MAP_FAILED) return 0;
    std::memcpy(code, reinterpret_cast<const void*>(target), 4);
    std::uint8_t jump_back[16]{};
    EncodeAbsoluteJump16(jump_back, target + 4);
    std::memcpy(code + 4, jump_back, sizeof(jump_back));
    __builtin___clear_cache(reinterpret_cast<char*>(code),
                            reinterpret_cast<char*>(code + 32));
    if (mprotect(code, 32, PROT_READ | PROT_EXEC) != 0) {
        munmap(code, 32);
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(code);
}

// Find 16 bytes of consecutive `ret` padding inside libAsphalt9.so within the
// 26-bit `b` branch range of the hook target. The returned offset is relative
// to the guest base and points to an existing alignment-padding run; it never
// overlaps any hook target or a previously chosen stub.
bool FindRetStubOffset(const char* lib_path, std::uint32_t target_off,
                       const std::uint32_t* avoid_offsets,
                       std::size_t avoid_count, std::uint32_t* stub_off) {
    FILE* file = std::fopen(lib_path, "rb");
    if (!file) return false;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    const long file_size_long = std::ftell(file);
    if (file_size_long <= 0) {
        std::fclose(file);
        return false;
    }
    const auto file_size = static_cast<std::uint64_t>(file_size_long);
    constexpr std::uint64_t kWindow = std::uint64_t{1} << 27;  // +/-128MB
    const std::uint64_t window_begin =
        target_off > kWindow ? target_off - kWindow : 0;
    const std::uint64_t window_end =
        std::min<std::uint64_t>(file_size, target_off + kWindow);

    constexpr std::uint8_t kRet[4] = {0xc0, 0x03, 0x5f, 0xd6};
    std::uint8_t chunk[65536]{};
    std::uint64_t cursor = window_begin;
    bool found = false;
    while (cursor < window_end) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(sizeof(chunk), window_end - cursor));
        if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) != 0 ||
            std::fread(chunk, 1, want, file) != want) {
            break;
        }
        for (std::size_t i = 0; i + 4 <= want; ++i) {
            if (std::memcmp(chunk + i, kRet, 4) != 0) continue;
            std::size_t run = 4;
            while (i + run + 4 <= want &&
                   std::memcmp(chunk + i + run, kRet, 4) == 0)
                run += 4;
            // A ret run frequently begins with the REAL return instruction
            // of the preceding function, followed by alignment rets. Only use
            // a run with at least one spare ret and place the stub AFTER the
            // first ret so the preceding function's epilogue stays intact.
            if (run < 20) {
                i += run - 1;
                continue;
            }
            const std::uint64_t run_file_off = cursor + i + 4;
            if (run_file_off + 16 > file_size) {
                i += run - 1;
                continue;
            }
            bool conflict = false;
            for (std::size_t a = 0; a < avoid_count; ++a) {
                const std::uint64_t avoid = avoid_offsets[a];
                if (run_file_off + 16 > avoid && run_file_off < avoid + 16) {
                    conflict = true;
                    break;
                }
            }
            if (!conflict) {
                *stub_off = static_cast<std::uint32_t>(run_file_off);
                found = true;
                break;
            }
            i += run - 1;
        }
        if (found) break;
        // 3-byte overlap so a run split by the chunk boundary is found in
        // the next chunk.
        cursor += want > 3 ? want - 3 : want;
    }
    std::fclose(file);
    return found;
}

constexpr EventKind kIndexToKind[kHookCount] = {
    EventKind::kTick,     EventKind::kSinkB,   EventKind::kSinkA,
    EventKind::kFinalC98, EventKind::kFinalC9C,
};

extern "C" __attribute__((noinline)) void TickObserverCapture(
    void* entry_x0, void* entry_x1, std::uint32_t kind_u32,
    std::uintptr_t original_lr) {
    const auto kind = kind_u32 < kHookCount
                          ? kIndexToKind[kind_u32]
                          : EventKind::kTick;
    const std::uint64_t sequence =
        g_events.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t slot = g_next_slot.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kCapacity) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto x0 = reinterpret_cast<std::uintptr_t>(entry_x0);
    const auto x1 = reinterpret_cast<std::uintptr_t>(entry_x1);

    if (kind == EventKind::kTick) {
        std::uint32_t empty = 0;
        if (g_identity_state.compare_exchange_strong(
                empty, 1, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            g_identity_x0 = x0;
            g_identity_x1 = x1;
            g_identity_caller = original_lr;
            g_identity_tid = static_cast<std::uint32_t>(gettid());
            g_identity_state.store(2, std::memory_order_release);
        }
    }

    Record& r = g_records[slot];
    r.sequence = sequence;
    r.monotonic_ns = MonotonicNs();
    r.x0 = x0;
    r.x1 = x1;
    r.caller = original_lr;
    r.tid = static_cast<std::uint32_t>(gettid());
    r.kind = static_cast<std::uint16_t>(kind);
    r.flags = 0;
    r.value = 0.0f;
    r.x0_word = 0;
    r.reserved = 0;

    if (kind == EventKind::kSinkB || kind == EventKind::kSinkA ||
        kind == EventKind::kFinalC98 || kind == EventKind::kFinalC9C) {
        if (x1 != 0 && IsReadable(x1, sizeof(float))) {
            r.value = *reinterpret_cast<const float*>(x1);
            r.flags |= 1u;
        }
    }
    if (x0 != 0 && IsReadable(x0, sizeof(std::uint32_t))) {
        std::memcpy(&r.x0_word, reinterpret_cast<const void*>(x0),
                    sizeof(std::uint32_t));
        r.flags |= 2u;
    }
    g_ready[slot].store(1, std::memory_order_release);
    g_dump_request.store(1, std::memory_order_release);
}

// Probe entry ABI, full-context version. The tiny per-hook naked entry only
// loads the hook index into x16 (scratch) and branches to ObserverCommon.
// ObserverCommon saves EVERY caller-saved register (x0-x17, q0-q31) plus x30,
// calls the normal C++ capture, restores everything, and branches to the
// hook's continuation trampoline. This is required because sink/final are
// tiny leaf functions: their callers are compiled assuming those leaves do
// not clobber FP/SIMD registers, so a probe that clobbers them corrupts the
// caller. The full save/restore makes the probe register-neutral.
extern "C" __attribute__((noinline)) void TickObserverCapture(
    void* entry_x0, void* entry_x1, std::uint32_t hook_index,
    std::uintptr_t original_lr);

extern "C" __attribute__((naked)) void TickProbe() {
    __asm__ volatile("mov w16, #0\n"
                     "b ObserverCommon\n");
}
extern "C" __attribute__((naked)) void SinkBProbe() {
    __asm__ volatile("mov w16, #1\n"
                     "b ObserverCommon\n");
}
extern "C" __attribute__((naked)) void SinkAProbe() {
    __asm__ volatile("mov w16, #2\n"
                     "b ObserverCommon\n");
}
extern "C" __attribute__((naked)) void FinalC98Probe() {
    __asm__ volatile("mov w16, #3\n"
                     "b ObserverCommon\n");
}
extern "C" __attribute__((naked)) void FinalC9CProbe() {
    __asm__ volatile("mov w16, #4\n"
                     "b ObserverCommon\n");
}

extern "C" __attribute__((naked)) void ObserverCommon() {
    __asm__ volatile(
        "sub sp, sp, #0x2a0\n"
        "stp x0, x1, [sp, #0x00]\n"
        "stp x2, x3, [sp, #0x10]\n"
        "stp x4, x5, [sp, #0x20]\n"
        "stp x6, x7, [sp, #0x30]\n"
        "stp x8, x9, [sp, #0x40]\n"
        "stp x10, x11, [sp, #0x50]\n"
        "stp x12, x13, [sp, #0x60]\n"
        "stp x14, x15, [sp, #0x70]\n"
        "stp x16, x17, [sp, #0x80]\n"
        "str x30, [sp, #0x90]\n"
        "str w16, [sp, #0x98]\n"
        "stp q0, q1, [sp, #0xa0]\n"
        "stp q2, q3, [sp, #0xc0]\n"
        "stp q4, q5, [sp, #0xe0]\n"
        "stp q6, q7, [sp, #0x100]\n"
        "stp q8, q9, [sp, #0x120]\n"
        "stp q10, q11, [sp, #0x140]\n"
        "stp q12, q13, [sp, #0x160]\n"
        "stp q14, q15, [sp, #0x180]\n"
        "stp q16, q17, [sp, #0x1a0]\n"
        "stp q18, q19, [sp, #0x1c0]\n"
        "stp q20, q21, [sp, #0x1e0]\n"
        "stp q22, q23, [sp, #0x200]\n"
        "stp q24, q25, [sp, #0x220]\n"
        "stp q26, q27, [sp, #0x240]\n"
        "stp q28, q29, [sp, #0x260]\n"
        "stp q30, q31, [sp, #0x280]\n"
        // Capture(entry_x0, entry_x1, hook_index, original_lr). x0/x1 are
        // still the entry values; the C call may clobber anything.
        "mov w2, w16\n"
        "ldr x3, [sp, #0x90]\n"
        "bl TickObserverCapture\n"
        // Restore all caller-saved state. x16/x17 are scratch and were saved,
        // but we reload x16 with the hook index and x17 with the continuation
        // base, so restoring their original values is deliberately skipped.
        "ldp x0, x1, [sp, #0x00]\n"
        "ldp x2, x3, [sp, #0x10]\n"
        "ldp x4, x5, [sp, #0x20]\n"
        "ldp x6, x7, [sp, #0x30]\n"
        "ldp x8, x9, [sp, #0x40]\n"
        "ldp x10, x11, [sp, #0x50]\n"
        "ldp x12, x13, [sp, #0x60]\n"
        "ldp x14, x15, [sp, #0x70]\n"
        "ldp q0, q1, [sp, #0xa0]\n"
        "ldp q2, q3, [sp, #0xc0]\n"
        "ldp q4, q5, [sp, #0xe0]\n"
        "ldp q6, q7, [sp, #0x100]\n"
        "ldp q8, q9, [sp, #0x120]\n"
        "ldp q10, q11, [sp, #0x140]\n"
        "ldp q12, q13, [sp, #0x160]\n"
        "ldp q14, q15, [sp, #0x180]\n"
        "ldp q16, q17, [sp, #0x1a0]\n"
        "ldp q18, q19, [sp, #0x1c0]\n"
        "ldp q20, q21, [sp, #0x1e0]\n"
        "ldp q22, q23, [sp, #0x200]\n"
        "ldp q24, q25, [sp, #0x220]\n"
        "ldp q26, q27, [sp, #0x240]\n"
        "ldp q28, q29, [sp, #0x260]\n"
        "ldp q30, q31, [sp, #0x280]\n"
        "ldr x30, [sp, #0x90]\n"
        "ldr w16, [sp, #0x98]\n"
        "adrp x17, :got:g_tick_observer_continue\n"
        "ldr x17, [x17, #:got_lo12:g_tick_observer_continue]\n"
        "ldr x17, [x17, x16, lsl #3]\n"
        "add sp, sp, #0x2a0\n"
        "br x17\n"
        ::: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
            "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
            "x30", "cc", "memory");
}

using ProbeFn = void (*)();
constexpr ProbeFn kProbeFns[kHookCount] = {
    &TickProbe, &SinkBProbe, &SinkAProbe, &FinalC98Probe, &FinalC9CProbe,
};


std::uint32_t PublishedRecordCount() {
    std::uint32_t claimed = g_next_slot.load(std::memory_order_acquire);
    if (claimed > kCapacity) claimed = kCapacity;
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < claimed; ++i) {
        if (g_ready[i].load(std::memory_order_acquire)) ++count;
    }
    return count;
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
    DumpHeader h{};
    std::memcpy(h.magic, "A9TOBSV1", 8);
    std::memcpy(h.build_id, kExpectedBuildId, sizeof(kExpectedBuildId));
    h.record_size = sizeof(Record);
    h.record_count = count;
    h.guest_base = g_guest_base.load(std::memory_order_relaxed);
    h.events = g_events.load(std::memory_order_relaxed);
    h.dropped = g_dropped.load(std::memory_order_relaxed);
    const bool ok =
        std::fwrite(&h, sizeof(h), 1, file) == 1 &&
        (count == 0 || std::fwrite(g_dump_records, sizeof(Record), count,
                                   file) == count);
    std::fclose(file);
    return ok;
}

__attribute__((noinline)) bool DumpBeginIdentity() {
    if (g_identity_state.load(std::memory_order_acquire) != 2) return true;
    const std::uintptr_t base =
        g_guest_base.load(std::memory_order_acquire);
    const std::uintptr_t object = g_identity_x0;
    std::uintptr_t vptr = 0;
    std::uintptr_t source = 0;
    std::uintptr_t source_vptr = 0;
    std::uintptr_t slot40 = 0;
    std::uintptr_t slot70 = 0;
    const bool object_ok = object != 0 &&
        ReadProcMem(object, &vptr, sizeof(vptr)) && vptr != 0;
    const bool source_ok = object_ok && object <= UINTPTR_MAX - 0x48 &&
        ReadProcMem(object + 0x48, &source, sizeof(source)) && source != 0 &&
        ReadProcMem(source, &source_vptr, sizeof(source_vptr)) &&
        source_vptr != 0;
    const bool slot40_ok = object_ok && vptr <= UINTPTR_MAX - 0x40 &&
        ReadProcMem(vptr + 0x40, &slot40, sizeof(slot40));
    const bool slot70_ok = object_ok && vptr <= UINTPTR_MAX - 0x70 &&
        ReadProcMem(vptr + 0x70, &slot70, sizeof(slot70));
    FILE* file = std::fopen(kIdentityOutput, "we");
    if (!file) return false;
    const int written = std::fprintf(
        file,
        "magic=A9G3BI1\n"
        "guest_base=0x%" PRIxPTR "\n"
        "x0=0x%" PRIxPTR "\n"
        "x1=0x%" PRIxPTR "\n"
        "caller=0x%" PRIxPTR "\n"
        "tid=%u\n"
        "vptr=0x%" PRIxPTR "\n"
        "source=0x%" PRIxPTR "\n"
        "source_vptr=0x%" PRIxPTR "\n"
        "slot40=0x%" PRIxPTR "\n"
        "slot70=0x%" PRIxPTR "\n"
        "object_read=%u\nsource_read=%u\nslot40_read=%u\nslot70_read=%u\n",
        base, object, g_identity_x1, g_identity_caller, g_identity_tid,
        vptr, source, source_vptr, slot40, slot70, object_ok ? 1u : 0u,
        source_ok ? 1u : 0u, slot40_ok ? 1u : 0u,
        slot70_ok ? 1u : 0u);
    const bool ok = written > 0 && std::fclose(file) == 0;
    if (ok) g_identity_state.store(3, std::memory_order_release);
    return ok;
}

void* Reporter(void*) {
    while (true) {
        sleep(2);
        const bool identity_ok = DumpBeginIdentity();
        if (!identity_ok) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "begin identity dump failed");
        }
        if (!g_dump_request.exchange(0, std::memory_order_acq_rel)) continue;
        const bool ok = Dump();
        __android_log_print(ok ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                            "events=%llu records=%u dropped=%llu dump=%s",
                            static_cast<unsigned long long>(
                                g_events.load(std::memory_order_relaxed)),
                            PublishedRecordCount(),
                            static_cast<unsigned long long>(
                                g_dropped.load(std::memory_order_relaxed)),
                            ok ? "ok" : "failed");
    }
}

bool VerifyAllSignatures(std::uintptr_t base) {
    for (std::size_t i = 0; i < kHookCount; ++i) {
        const auto target = reinterpret_cast<const std::uint8_t*>(
            base + kHooks[i].offset);
        if (std::memcmp(target, kHooks[i].signature, 12) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "signature mismatch hook=%s offset=0x%x",
                                kHooks[i].name, kHooks[i].offset);
            return false;
        }
    }
    return true;
}

bool Install(std::uintptr_t base, std::uint32_t mask, const char* lib_path) {
    if (!VerifyAllSignatures(base)) return false;

    // Avoid every hook target first 16 bytes when choosing ret-padding stubs,
    // and avoid reusing the same padding run for two hooks.
    std::vector<std::uint32_t> avoid;
    for (const HookSpec& hook : kHooks) avoid.push_back(hook.offset);

    for (std::size_t i = 0; i < kHookCount; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        const std::uintptr_t target = base + kHooks[i].offset;

        std::uint32_t stub_off = 0;
        if (!FindRetStubOffset(lib_path, kHooks[i].offset, avoid.data(),
                               avoid.size(), &stub_off)) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "ret padding stub not found hook=%s",
                                kHooks[i].name);
            return false;
        }
        const std::uintptr_t stub_addr = base + stub_off;

        // Safety: the chosen padding must really be 16 bytes of `ret` before
        // we overwrite it.
        std::uint8_t padding_check[16]{};
        constexpr std::uint8_t kRet[4] = {0xc0, 0x03, 0x5f, 0xd6};
        if (!ReadProcMem(stub_addr, padding_check, sizeof(padding_check)) ||
            std::memcmp(padding_check, kRet, 4) != 0 ||
            std::memcmp(padding_check + 4, kRet, 4) != 0 ||
            std::memcmp(padding_check + 8, kRet, 4) != 0 ||
            std::memcmp(padding_check + 12, kRet, 4) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "ret padding mismatch hook=%s stub=0x%x",
                                kHooks[i].name, stub_off);
            return false;
        }

        const std::uintptr_t trampoline = BuildTrampoline4(target);
        if (!trampoline) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "trampoline build failed hook=%s",
                                kHooks[i].name);
            return false;
        }
        g_tick_observer_continue[i] = trampoline;

        std::uint8_t stub_jump[16]{};
        EncodeAbsoluteJump16(stub_jump,
                             reinterpret_cast<std::uintptr_t>(kProbeFns[i]));
        if (!WriteProcMem(stub_addr, stub_jump, sizeof(stub_jump))) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "stub write failed hook=%s", kHooks[i].name);
            return false;
        }
        std::uint8_t stub_verify[16]{};
        if (!ReadProcMem(stub_addr, stub_verify, sizeof(stub_verify)) ||
            std::memcmp(stub_verify, stub_jump, sizeof(stub_jump)) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "stub verify failed hook=%s", kHooks[i].name);
            return false;
        }
        __builtin___clear_cache(reinterpret_cast<char*>(stub_addr),
                                reinterpret_cast<char*>(stub_addr + 16));

        std::uint8_t patch[4]{};
        if (!EncodeBranchImm(patch, target, stub_addr)) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "branch encode failed hook=%s", kHooks[i].name);
            return false;
        }
        if (!WriteProcMem(target, patch, sizeof(patch))) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "entry patch write failed hook=%s (earlier "
                                "hooks left installed; fail closed)",
                                kHooks[i].name);
            return false;
        }
        std::uint8_t patch_verify[4]{};
        if (!ReadProcMem(target, patch_verify, sizeof(patch_verify)) ||
            std::memcmp(patch_verify, patch, sizeof(patch)) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "entry patch verify failed hook=%s",
                                kHooks[i].name);
            return false;
        }
        __builtin___clear_cache(reinterpret_cast<char*>(target),
                                reinterpret_cast<char*>(target + 4));
        avoid.push_back(stub_off);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "probe installed hook=%s target=%p stub_off=0x%x",
                            kHooks[i].name, reinterpret_cast<void*>(target),
                            stub_off);
    }
    pthread_t reporter{};
    if (pthread_create(&reporter, nullptr, Reporter, nullptr) == 0)
        pthread_detach(reporter);
    return true;
}

void* Worker(void*) {
    int delay_ms = 30000;
    std::uint32_t mask = 0x1f;
    ReadConfig(&delay_ms, &mask);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "config delay_ms=%d mask=0x%x", delay_ms, mask);
    for (int attempt = 0; attempt < 1200; ++attempt) {
        GameMapping mapping{};
        dl_iterate_phdr(FindGuestGameModule, nullptr);
        const auto base = g_guest_base.load(std::memory_order_acquire);
        std::uint8_t build[20]{};
        if (FindGameMapping(&mapping) && base &&
            ReadBuildId(mapping.path, build) && LoadReadableRanges()) {
            if (std::memcmp(build, kExpectedBuildId, sizeof(build)) != 0) {
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                                    "build id mismatch; probe disabled");
                return nullptr;
            }
            if (access(kEnableMarker, F_OK) != 0) {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "enable marker absent; probe disabled");
                return nullptr;
            }
            if (delay_ms > 0) {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "waiting %d ms before hook install",
                                    delay_ms);
                usleep(static_cast<useconds_t>(delay_ms) * 1000);
            }
            if (Install(base, mask, mapping.path)) {
                FILE* receipt = std::fopen(kArmReceipt, "we");
                if (receipt) {
                    std::fprintf(receipt,
                                 "magic=A9G3AR1\npid=%d\nguest_base=0x%" PRIxPTR
                                 "\nmask=0x%x\n",
                                 getpid(), base, mask);
                    std::fclose(receipt);
                }
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "tick observer armed base=%p mask=0x%x",
                                    reinterpret_cast<void*>(base), mask);
                return nullptr;
            }
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "install failed; probe disabled");
            return nullptr;
        }
        usleep(100000);  // 100 ms
    }
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "game module not found within 120s");
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded passive=1 observer=tick-v1 hooks=observation-only");
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, Worker, nullptr) == 0)
        pthread_detach(worker);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_payload_protocol() {
    return 2;
}
