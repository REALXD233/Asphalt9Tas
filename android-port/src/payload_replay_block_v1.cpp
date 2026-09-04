// Blocking per-tick replay payload (Android / Houdini guest), mirroring the
// upstream AluTasV2 ActiveBlockThread design.
//
// Hooks:
//   0x386b1e0 UpdatePerTick                  tick anchor + blocking wait
//   0x367b21c VehicleControlSink_SetValueA   steering override (*x1)
//   0x367b1f0 VehicleControlSink_SetValueB   brake override (*x1)
//
// Tick flow:
//   UpdatePerTick entry -> TickCapture:
//      tick = g_tick++;
//      publish previous tick's observed steer/brake for the host recorder;
//      if replay mode: bridge.state=WAITING; poll until host writes a frame
//        for this tick and sets state=FRAME_READY; copy frame; state=IDLE.
//   SinkA/SinkB Capture: if a replay frame is valid, overwrite *x1 before the
//   original setter runs. The original value is still recorded afterwards.
//
// Hook mechanics: 12-byte adrp/add/br patches at each target. The absolute
// jump stub and the 12-byte continuation trampoline live in an anonymous page
// allocated in a free hole close to the game library (inside +-4GB), so the
// adrp range never overflows and no game padding/code is overwritten.
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

constexpr const char* kTag = "A9TAS_REPLAY_BLOCK";
constexpr const char* kEnableMarker = "/data/local/tmp/a9tas-enable-replay-block";
constexpr const char* kBridgePath =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-replay-bridge.addr";
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

constexpr std::uint32_t kBridgeMagic = 0xA9B2D001;
enum BridgeState : std::uint32_t { kIdle = 0, kWaiting = 1, kFrameReady = 2 };

struct ReplayFrame {
    std::uint64_t tick;
    std::uint64_t monotonic_ms;
    std::uint32_t steer_bits;
    std::uint32_t brake_bits;
    std::uint32_t valid;
    std::uint32_t reserved;
};

// Plain shared bridge; the host writes it through /proc/pid/mem. The guest
// side only writes before/after its own polling loops; the host polls the
// same fields. This is a strict two-party handshake, so plain loads/stores
// are sufficient in practice.
struct Bridge {
    std::uint32_t magic;
    std::uint32_t state;          // BridgeState
    std::uint32_t mode;           // 0 record, 1 replay
    std::uint32_t reserved;
    std::uint64_t waiting_tick;
    std::uint64_t published_seq;
    std::uint64_t published_tick;
    std::uint32_t published_steer_bits;
    std::uint32_t published_brake_bits;
    std::uint32_t published_valid;
    std::uint32_t pad;
    ReplayFrame frame;
};

static_assert(offsetof(Bridge, waiting_tick) == 16, "bridge ABI");
static_assert(offsetof(Bridge, frame) == 56, "bridge ABI");
static_assert(sizeof(Bridge) == 88, "bridge ABI");
static_assert(sizeof(ReplayFrame) == 32, "frame ABI");

struct HookSpec {
    std::uint32_t offset;
    std::uint32_t index;  // 0 tick, 1 sinkA, 2 sinkB
    const char* name;
    std::uint8_t signature[12];
};

constexpr HookSpec kHooks[] = {
    {0x386b1e0, 0, "UpdatePerTick",
     {0xff, 0x43, 0x01, 0xd1, 0xf4, 0x1b, 0x00, 0xf9, 0xf3, 0x7b, 0x04, 0xa9}},
#ifndef A9TAS_TICK_ONLY
    {0x367b21c, 1, "SinkA",
     {0x08, 0x00, 0x40, 0xf9, 0x29, 0x00, 0x40, 0xb9, 0x08, 0x81, 0x5c, 0xf8}},
    {0x367b1f0, 2, "SinkB",
     {0x08, 0x00, 0x40, 0xf9, 0x29, 0x00, 0x40, 0xb9, 0x08, 0x01, 0x5d, 0xf8}},
#endif
};
constexpr std::size_t kHookCount = sizeof(kHooks) / sizeof(kHooks[0]);

Bridge g_bridge{};
std::uint64_t g_tick = 0;
std::uint32_t g_allow_override = 0;
ReplayFrame g_current_frame{};
float g_last_steer = 0.0F;
float g_last_brake = 0.0F;
extern "C" std::uintptr_t g_replay_block_continue[kHookCount] = {};

bool g_maps_loaded = false;
struct ReadableRange {
    std::uintptr_t begin;
    std::uintptr_t end;
};
ReadableRange g_readable_ranges[512]{};
std::size_t g_readable_range_count = 0;

std::uint64_t MonotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ULL;
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

bool LoadMaps() {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[2048]{};
    std::size_t count = 0;
    while (count < 512 && std::fgets(line, sizeof(line), maps)) {
        unsigned long long begin = 0, end = 0;
        char perms[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &begin, &end, perms) == 3 &&
            perms[0] == 'r') {
            g_readable_ranges[count++] = {static_cast<std::uintptr_t>(begin),
                                          static_cast<std::uintptr_t>(end)};
        }
    }
    std::fclose(maps);
    g_readable_range_count = count;
    g_maps_loaded = true;
    return count != 0;
}

struct GameMapping {
    std::uintptr_t base{};
    char path[1024]{};
};

std::uintptr_t g_guest_dlpi_base = 0;

int FindGuestGameModule(dl_phdr_info* info, size_t, void*) {
    if (info && info->dlpi_name &&
        std::strstr(info->dlpi_name, "libAsphalt9.so")) {
        g_guest_dlpi_base = static_cast<std::uintptr_t>(info->dlpi_addr);
        return 1;
    }
    return 0;
}

bool FindGameMapping(GameMapping* mapping) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (!maps) return false;
    char line[2048]{};
    bool found = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{}, path[1024]{};
        const int fields = std::sscanf(
            line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &start, &end,
            perms, &offset, path);
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

bool ReadProcMem(std::uintptr_t address, void* buffer, std::size_t size) {
    const int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t n = pread(fd, buffer, size, static_cast<off_t>(address));
    close(fd);
    return n == static_cast<ssize_t>(size);
}

bool WriteProcMem(std::uintptr_t address, const void* buffer, std::size_t size) {
    const int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) return false;
    const ssize_t n = pwrite(fd, buffer, size, static_cast<off_t>(address));
    close(fd);
    return n == static_cast<ssize_t>(size);
}

void EncodeAbsoluteJump16(std::uint8_t out[16], std::uintptr_t destination) {
    constexpr std::uint32_t ldr = 0x58000051;  // ldr x17, #8
    constexpr std::uint32_t br = 0xd61f0220;   // br x17
    std::memcpy(out, &ldr, 4);
    std::memcpy(out + 4, &br, 4);
    std::memcpy(out + 8, &destination, 8);
}

void EncodeLiteralJump16(std::uint8_t out[16], std::uintptr_t destination) {
    constexpr std::uint32_t ldr = 0x58000050;  // ldr x16, #8
    constexpr std::uint32_t br = 0xd61f0200;   // br x16
    std::memcpy(out, &ldr, 4);
    std::memcpy(out + 4, &br, 4);
    std::memcpy(out + 8, &destination, 8);
}

bool EncodeAbsoluteJump12(std::uint8_t out[12], std::uintptr_t pc,
                          std::uintptr_t target) {
    const std::uintptr_t pc_page = pc & ~std::uintptr_t{0xfff};
    const std::uintptr_t target_page = target & ~std::uintptr_t{0xfff};
    const std::int64_t page_delta =
        static_cast<std::int64_t>(target_page - pc_page) >> 12;
    if (page_delta < -0x100000 || page_delta > 0xfffff) return false;
    const auto imm = static_cast<std::uint32_t>(page_delta & 0x1fffff);
    const std::uint32_t adrp =
        0x90000000u | ((imm & 0x3u) << 29) |
        (((imm >> 2) & 0x7ffffu) << 5) | 16u;  // x16
    const std::uint32_t add =
        0x91000000u | ((static_cast<std::uint32_t>(target) & 0xfffu) << 10) |
        (16u << 5) | 16u;                       // add x16,x16,#lo12
    const std::uint32_t br = 0xd61f0200u;       // br x16
    std::memcpy(out, &adrp, 4);
    std::memcpy(out + 4, &add, 4);
    std::memcpy(out + 8, &br, 4);
    return true;
}

// Find a free page in a +-3GB window around the game base using the guest's
// own /proc/self/maps, and mmap it MAP_FIXED. Used for jump stubs and
// trampolines so the 12-byte adrp patches always stay in range.
void* AllocateNearPage(std::uintptr_t game_base) {
    struct Range {
        std::uintptr_t begin, end;
    };
    std::vector<Range> ranges;
    FILE* maps = std::fopen("/proc/self/maps", "re");
    char line[2048]{};
    while (maps && std::fgets(line, sizeof(line), maps)) {
        unsigned long long begin = 0, end = 0;
        if (std::sscanf(line, "%llx-%llx", &begin, &end) == 2)
            ranges.push_back({static_cast<std::uintptr_t>(begin),
                              static_cast<std::uintptr_t>(end)});
    }
    if (maps) std::fclose(maps);

    constexpr std::uintptr_t kWindow = std::uintptr_t{3} << 30;  // 3GB
    const std::uintptr_t lo = game_base > kWindow ? game_base - kWindow : 0x10000;
    const std::uintptr_t hi = game_base + kWindow;
    const std::uintptr_t kPage = 4096;
    for (std::uintptr_t candidate = lo; candidate < hi;
         candidate += 0x1000000) {
        const std::uintptr_t page = candidate & ~(kPage - 1);
        bool overlap = false;
        for (const auto& r : ranges) {
            if (page + kPage > r.begin && page < r.end) {
                overlap = true;
                break;
            }
        }
        if (overlap) continue;
        void* mapped = mmap(reinterpret_cast<void*>(page), kPage,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (mapped != MAP_FAILED) return mapped;
    }
    return nullptr;
}

extern "C" __attribute__((noinline)) void ReplayBlockCapture(
    void* entry_x0, void* entry_x1, std::uint32_t hook_index,
    std::uintptr_t original_lr) {
    (void)original_lr;
    const std::uintptr_t x1 = reinterpret_cast<std::uintptr_t>(entry_x1);

    if (hook_index == 0) {
        // Tick anchor. Every entry is one logic tick.
        const std::uint64_t tick = g_tick++;
        if ((tick % 200) == 0) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "tick=%llu tid=%d",
                                static_cast<unsigned long long>(tick),
                                static_cast<int>(gettid()));
        }

        // Publish the previous tick's observed values for the host recorder.
        g_bridge.published_seq = g_bridge.published_seq + 1;
        g_bridge.published_tick = tick;
        std::memcpy(&g_bridge.published_steer_bits, &g_last_steer, 4);
        std::memcpy(&g_bridge.published_brake_bits, &g_last_brake, 4);
        g_bridge.published_valid = 1;

        if (g_bridge.mode == 1) {
            g_bridge.waiting_tick = tick;
            g_bridge.state = BridgeState::kWaiting;
            while (g_bridge.state != BridgeState::kFrameReady) {
                usleep(100);
            }
            g_current_frame = g_bridge.frame;
            g_bridge.state = BridgeState::kIdle;
            g_allow_override = 1;
        } else {
            g_current_frame.valid = 0;
            g_allow_override = 0;
        }
    } else if ((hook_index == 1 || hook_index == 2) &&
               g_allow_override && g_current_frame.valid &&
               g_current_frame.tick == g_tick - 1) {
        float* value = reinterpret_cast<float*>(x1);
        if (IsReadable(x1, sizeof(float))) {
            if (hook_index == 1) {
                std::memcpy(value, &g_current_frame.steer_bits, 4);
                std::memcpy(&g_last_steer, value, 4);
            } else if (hook_index == 2) {
                std::memcpy(value, &g_current_frame.brake_bits, 4);
                std::memcpy(&g_last_brake, value, 4);
                g_allow_override = 0;
            }
        }
    } else {
        if (IsReadable(x1, sizeof(float))) {
            float observed = 0.0F;
            std::memcpy(&observed, reinterpret_cast<void*>(x1), 4);
            if (hook_index == 1) g_last_steer = observed;
            if (hook_index == 2) g_last_brake = observed;
        }
    }
}

extern "C" __attribute__((naked)) void TickReplayProbe() {
    __asm__ volatile("mov w16, #0\n"
                     "b ReplayBlockCommon\n");
}
extern "C" __attribute__((naked)) void SinkAReplayProbe() {
    __asm__ volatile("mov w16, #1\n"
                     "b ReplayBlockCommon\n");
}
extern "C" __attribute__((naked)) void SinkBReplayProbe() {
    __asm__ volatile("mov w16, #2\n"
                     "b ReplayBlockCommon\n");
}

extern "C" __attribute__((naked)) void ReplayBlockCommon() {
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
        "mov w2, w16\n"
        "ldr x3, [sp, #0x90]\n"
        "bl ReplayBlockCapture\n"
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
        "adrp x17, :got:g_replay_block_continue\n"
        "ldr x17, [x17, #:got_lo12:g_replay_block_continue]\n"
        "ldr x17, [x17, x16, lsl #3]\n"
        "add sp, sp, #0x2a0\n"
        "br x17\n"
        ::: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
            "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
            "x30", "cc", "memory");
}

using ProbeFn = void (*)();
ProbeFn g_probes[kHookCount] = {&TickReplayProbe
#ifndef A9TAS_TICK_ONLY
                                , &SinkAReplayProbe, &SinkBReplayProbe
#endif
};

// Continuation targets: absolute addresses where each hook's continuation
// must land after replaying the original 16 bytes (game-relative offsets).
// SinkA/SinkB 的第 4 条指令是 PC 相对 b（+0xCBD8 / +0xCBF8），在续体里必须
// 绝对化跳转到原目标地址，因此续体是编译期函数（payload.so 内，Houdini
// 已注册模块），不再使用匿名近页。
extern "C" std::uintptr_t g_cont_target[kHookCount] = {};
constexpr std::uint32_t kContOffsets[kHookCount] = {
    0x0,
#ifndef A9TAS_TICK_ONLY
    0xCBD8, 0xCBF8
#endif
};

extern "C" __attribute__((naked)) void ContinuationUpdatePerTick() {
    __asm__ volatile(
        "sub sp, sp, #0x50\n"
        "str x20, [sp, #0x30]\n"
        "stp x19, x30, [sp, #0x8]\n"
        "mov x20, x0\n"
        "adrp x17, :got:g_cont_target\n"
        "ldr x17, [x17, #:got_lo12:g_cont_target]\n"
        "ldr x17, [x17, #0]\n"
        "br x17\n"
        ::: "x19", "x20", "x30", "x17", "memory");
}
#ifndef A9TAS_TICK_ONLY
extern "C" __attribute__((naked)) void ContinuationSinkA() {
    __asm__ volatile(
        "ldr x8, [x0]\n"
        "ldr w9, [x1]\n"
        "ldr x8, [x8, x12, lsl #3]\n"
        "adrp x17, :got:g_cont_target\n"
        "ldr x17, [x17, #:got_lo12:g_cont_target]\n"
        "ldr x17, [x17, #8]\n"
        "br x17\n"
        ::: "x8", "x9", "x17", "memory");
}
extern "C" __attribute__((naked)) void ContinuationSinkB() {
    __asm__ volatile(
        "ldr x8, [x0]\n"
        "ldr w9, [x1]\n"
        "ldr x8, [x8, x13, lsl #3]\n"
        "adrp x17, :got:g_cont_target\n"
        "ldr x17, [x17, #:got_lo12:g_cont_target]\n"
        "ldr x17, [x17, #16]\n"
        "br x17\n"
        ::: "x8", "x9", "x17", "memory");
}
#endif

bool Install(std::uintptr_t base) {
    for (const HookSpec& hook : kHooks) {
        const std::uintptr_t target = base + hook.offset;
        std::uint8_t actual[12]{};
        if (!ReadProcMem(target, actual, sizeof(actual)) ||
            std::memcmp(actual, hook.signature, 12) != 0) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "signature mismatch hook=%s base=%p target=%p "
                                "actual=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
                                hook.name, reinterpret_cast<void*>(base),
                                reinterpret_cast<void*>(target), actual[0],
                                actual[1], actual[2], actual[3], actual[4],
                                actual[5], actual[6], actual[7], actual[8],
                                actual[9], actual[10], actual[11]);
            return false;
        }
    }

    // 16-byte literal patch at each game function entry:
    //   ldr x16, [pc, #8]; br x16; <u64 probe addr>
    // 分支目标 = payload.so 内的探针（Houdini 已注册模块）。不再使用匿名
    // 近页 stub/trampoline（Houdini 对匿名页分支目标的重解析会在比赛开始的
    // 翻译缓存风暴中触发 0xdead0000 崩溃）。
    for (std::size_t i = 0; i < kHookCount; ++i) {
        const std::uintptr_t target = base + kHooks[i].offset;
        std::uint8_t patch[16]{};
        EncodeLiteralJump16(patch,
                            reinterpret_cast<std::uintptr_t>(g_probes[i]));
        if (!WriteProcMem(target, patch, sizeof(patch))) return false;
        __builtin___clear_cache(reinterpret_cast<char*>(target),
                                reinterpret_cast<char*>(target + 16));
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "probe installed hook=%s target=%p probe=%p",
                            kHooks[i].name, reinterpret_cast<void*>(target),
                            reinterpret_cast<void*>(g_probes[i]));
    }

    // Continuation targets (absolute, game-relative offsets applied).
    for (std::size_t i = 0; i < kHookCount; ++i) {
        g_cont_target[i] =
            base + kHooks[i].offset + 16 + kContOffsets[i];
        if (i == 0) {
            g_replay_block_continue[i] =
                reinterpret_cast<std::uintptr_t>(&ContinuationUpdatePerTick);
#ifndef A9TAS_TICK_ONLY
        } else if (i == 1) {
            g_replay_block_continue[i] =
                reinterpret_cast<std::uintptr_t>(&ContinuationSinkA);
        } else {
            g_replay_block_continue[i] =
                reinterpret_cast<std::uintptr_t>(&ContinuationSinkB);
#endif
        }
    }

    g_bridge.magic = kBridgeMagic;
    g_bridge.state = BridgeState::kIdle;
    g_bridge.mode = 0;
    FILE* addr_file = std::fopen(kBridgePath, "w");
    if (addr_file) {
        std::fprintf(addr_file, "bridge=0x%" PRIxPTR "\n",
                     reinterpret_cast<std::uintptr_t>(&g_bridge));
        std::fclose(addr_file);
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "bridge=%p armed",
                        reinterpret_cast<void*>(&g_bridge));
    return true;
}

void* Worker(void*) {
    for (int attempt = 0; attempt < 1200; ++attempt) {
        GameMapping mapping{};
        dl_iterate_phdr(FindGuestGameModule, nullptr);
        std::uint8_t build[20]{};
        if (g_guest_dlpi_base != 0 && FindGameMapping(&mapping) &&
            ReadBuildId(mapping.path, build) && LoadMaps()) {
            if (std::memcmp(build, kExpectedBuildId, sizeof(build)) != 0) {
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                                    "build id mismatch; disabled");
                return nullptr;
            }
            if (access(kEnableMarker, F_OK) != 0) {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "enable marker absent; disabled");
                return nullptr;
            }
            // dlpi_addr is the real guest execution base. /proc/self/maps in
            // the Houdini guest may report a shifted base (observed +0xCA000),
            // which breaks signature checks.
            const std::uintptr_t base =
                g_guest_dlpi_base != 0 ? g_guest_dlpi_base : mapping.base;
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "bases maps=0x%llx dlpi=0x%llx using=0x%llx",
                                static_cast<unsigned long long>(mapping.base),
                                static_cast<unsigned long long>(
                                    g_guest_dlpi_base),
                                static_cast<unsigned long long>(base));
            // Wait until the startup translation storm settles.
            for (int delay = 0; delay < 300; ++delay) usleep(100000);
            if (Install(base)) return nullptr;
            __android_log_print(ANDROID_LOG_ERROR, kTag, "install failed");
            return nullptr;
        }
        usleep(100000);
    }
    __android_log_print(ANDROID_LOG_ERROR, kTag, "game module not found");
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded replay-block passive=1");
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, Worker, nullptr) == 0)
        pthread_detach(worker);
}

}  // namespace
