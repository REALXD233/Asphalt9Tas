// A9TAS source-proxy replay prototype for the ARM64 guest.
//
// UNSAFE NEGATIVE EXPERIMENT -- DO NOT DEPLOY ON LDPLAYER/HOUDINI.
// Live tests on 2026-08-16 produced repeatable SIGSEGV failures when translated
// guest control flow entered/exited this payload, both from a function-entry
// hook and from the registry dispatch callsite. Retained only as auditable
// source-level design evidence; the external HWBP tick controller is the live
// mainline.
//
// This is deliberately separate from every deployed payload. It installs one
// 4-byte branch-with-link at the proven registry vtable dispatch callsite,
// filters the original x8 target to UpdatePerTick, selects a complete
// replay frame at tick begin, temporarily substitutes controller+0x48 with a
// tiny source object, calls the unmodified game function, restores the real
// source at tick end, then commits the frame number. Sink/final setters and
// their translated leaf functions are never patched.
//
// Nothing happens unless the marker and an exact mode config both exist;
// replay mode additionally requires a valid replay file:
//   /data/local/tmp/a9tas-enable-source-proxy
//   /data/local/tmp/a9tas-source-proxy-v1.cfg  (mode=observe|mode=replay)
//   /data/local/tmp/a9tas-source-proxy-v1.bin

// This prototype covers the proven keyboard axes only:
//   value_A = steering, value_B = negative longitudinal / S, value_C = 0.
// Nitro, respawn, transform, velocity and RNG are intentionally not claimed.

// Build-specific target: Asphalt 9 CN 600k, Build ID e5dd7ef2...

#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr const char* kTag = "A9TAS_SOURCE_PROXY";
constexpr const char* kEnableMarker =
    "/data/local/tmp/a9tas-enable-source-proxy";
constexpr const char* kConfigPath =
    "/data/local/tmp/a9tas-source-proxy-v1.cfg";
constexpr const char* kReplayPath =
    "/data/local/tmp/a9tas-source-proxy-v1.bin";
constexpr char kReplayMagic[8] = {'A', '9', 'S', 'P', 'R', '1', '\0', '\0'};
constexpr std::uint32_t kReplayVersion = 1;
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

constexpr std::uintptr_t kUpdatePerTickRva = 0x386B1E0;
constexpr std::uintptr_t kDispatchCallsiteRva = 0x37D98C0;
constexpr std::uintptr_t kTickStubRva = 0x35C37DC;
constexpr std::uintptr_t kControllerVtableRva = 0x80C4DA8;
constexpr std::uintptr_t kKeyboardSourceVtableRva = 0x80BF1C0;
constexpr std::uintptr_t kControllerSourceOffset = 0x48;
constexpr std::size_t kMaxFrames = 36000;

constexpr std::uint8_t kUpdatePerTickSignature[32] = {
    0xFF, 0x43, 0x01, 0xD1, 0xF4, 0x1B, 0x00, 0xF9,
    0xF3, 0x7B, 0x04, 0xA9, 0xF4, 0x03, 0x01, 0xAA,
    0xF3, 0x03, 0x00, 0xAA, 0xCC, 0xFE, 0xFF, 0x97,
    0x68, 0x2E, 0x40, 0xF9, 0xE8, 0x00, 0x00, 0xB4,
};
constexpr std::uint8_t kTickStubPaddingSignature[16] = {
    0xC0, 0x03, 0x5F, 0xD6, 0xC0, 0x03, 0x5F, 0xD6,
    0xC0, 0x03, 0x5F, 0xD6, 0xC0, 0x03, 0x5F, 0xD6,
};
constexpr std::uint8_t kDispatchCallsiteSignature[4] = {
    0x00, 0x01, 0x3F, 0xD6,  // BLR X8
};

enum FrameFlags : std::uint32_t {
    kFrameSteeringValid = 1u << 0,
    kFrameLongitudinalValid = 1u << 1,
    kFrameValueCValid = 1u << 2,
};

enum class RunMode : std::uint32_t {
    kDisabled = 0,
    kObserve = 1,
    kReplay = 2,
};

#pragma pack(push, 1)
struct ReplayHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t frame_size;
    std::uint32_t frame_count;
    std::uint8_t build_id[20];
    std::uint8_t reserved[20];
};

struct ReplayFrame {
    float steering;
    float longitudinal;
    float value_c;
    std::uint32_t flags;
};
#pragma pack(pop)

static_assert(sizeof(ReplayHeader) == 64, "replay header ABI");
static_assert(sizeof(ReplayFrame) == 16, "replay frame ABI");

struct GameMapping {
    std::uintptr_t base{};
    char path[1024]{};
};

struct ProxySource {
    void** vtable{};
    void* original_source{};
    std::uint32_t value_a_bits{};
    std::uint32_t value_b_bits{};
    std::uint32_t value_c_bits{};
    std::uint32_t reserved{};
};

struct TickSwap {
    std::uintptr_t controller{};
    void* original_source{};
    bool observed{};
    bool swapped{};
};

std::atomic<std::uintptr_t> g_guest_base{0};
extern "C" __attribute__((visibility("hidden"))) std::uintptr_t
    g_tick_continue = 0;
std::atomic<std::uint32_t> g_next_frame{0};
std::atomic<std::uint32_t> g_disabled{0};
std::atomic<std::uint32_t> g_observed_ticks{0};
std::atomic<RunMode> g_mode{RunMode::kDisabled};
std::atomic<std::uintptr_t> g_last_controller{0};
std::atomic<std::uint32_t> g_identity_mismatch_logged{0};
std::vector<ReplayFrame> g_frames;
thread_local ProxySource g_proxy{};
thread_local TickSwap g_swap{};

extern "C" void ProxyDestroy(void*) {}

// The game's three source methods return through the AArch64 hidden aggregate
// result pointer in x8. Keeping these as tiny naked functions preserves that
// ABI exactly and avoids a compiler-generated return convention mismatch.
extern "C" __attribute__((naked)) void ProxyGetValueC() {
    __asm__ volatile("ldr w9, [x0, #24]\n"
                     "str w9, [x8]\n"
                     "ret\n");
}

extern "C" __attribute__((naked)) void ProxyGetValueA() {
    __asm__ volatile("ldr w9, [x0, #16]\n"
                     "str w9, [x8]\n"
                     "ret\n");
}

extern "C" __attribute__((naked)) void ProxyGetValueB() {
    __asm__ volatile("ldr w9, [x0, #20]\n"
                     "str w9, [x8]\n"
                     "ret\n");
}

void* g_proxy_vtable[] = {
    reinterpret_cast<void*>(&ProxyDestroy),
    reinterpret_cast<void*>(&ProxyDestroy),
    reinterpret_cast<void*>(&ProxyGetValueC),
    reinterpret_cast<void*>(&ProxyGetValueA),
    reinterpret_cast<void*>(&ProxyGetValueB),
};

bool IsFiniteAxis(float value) {
    return std::isfinite(value) && value >= -1.0001f && value <= 1.0001f;
}

RunMode ReadRunMode() {
    FILE* file = std::fopen(kConfigPath, "re");
    if (!file) return RunMode::kDisabled;
    char line[64]{};
    const bool read_ok = std::fgets(line, sizeof(line), file) != nullptr;
    std::fclose(file);
    if (!read_ok) return RunMode::kDisabled;
    if (std::strcmp(line, "mode=observe\n") == 0 ||
        std::strcmp(line, "mode=observe\r\n") == 0 ||
        std::strcmp(line, "mode=observe") == 0)
        return RunMode::kObserve;
    if (std::strcmp(line, "mode=replay\n") == 0 ||
        std::strcmp(line, "mode=replay\r\n") == 0 ||
        std::strcmp(line, "mode=replay") == 0)
        return RunMode::kReplay;
    return RunMode::kDisabled;
}

bool ReadProcMem(std::uintptr_t address, void* output, std::size_t size) {
    const int fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t read_count =
        pread(fd, output, size, static_cast<off_t>(address));
    close(fd);
    return read_count == static_cast<ssize_t>(size);
}

bool WriteProcMem(std::uintptr_t address, const void* input,
                  std::size_t size) {
    const int fd = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t write_count =
        pwrite(fd, input, size, static_cast<off_t>(address));
    close(fd);
    return write_count == static_cast<ssize_t>(size);
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
        if (fields != 5 || offset != 0 ||
            std::strstr(path, "libAsphalt9.so") == nullptr)
            continue;
        char* clean = path;
        while (*clean == ' ') ++clean;
        mapping->base = static_cast<std::uintptr_t>(start);
        std::snprintf(mapping->path, sizeof(mapping->path), "%s", clean);
        found = true;
        break;
    }
    std::fclose(maps);
    return found;
}

int FindGuestModule(dl_phdr_info* info, size_t, void*) {
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
    Elf64_Ehdr header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 ||
        std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_machine != EM_AARCH64) {
        std::fclose(file);
        return false;
    }
    bool found = false;
    for (std::uint16_t index = 0; index < header.e_phnum && !found; ++index) {
        Elf64_Phdr program{};
        const auto at = static_cast<long>(header.e_phoff) +
                        static_cast<long>(index) * sizeof(program);
        if (std::fseek(file, at, SEEK_SET) != 0 ||
            std::fread(&program, sizeof(program), 1, file) != 1)
            break;
        if (program.p_type != PT_NOTE || program.p_filesz > 1024 * 1024)
            continue;
        std::uint64_t cursor = program.p_offset;
        const std::uint64_t end = program.p_offset + program.p_filesz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr note{};
            if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) != 0 ||
                std::fread(&note, sizeof(note), 1, file) != 1)
                break;
            cursor += sizeof(note);
            const std::uint32_t name_size = (note.n_namesz + 3u) & ~3u;
            const std::uint32_t data_size = (note.n_descsz + 3u) & ~3u;
            if (cursor + name_size + data_size > end) break;
            char name[16]{};
            if (note.n_namesz < sizeof(name)) {
                std::fseek(file, static_cast<long>(cursor), SEEK_SET);
                std::fread(name, 1, note.n_namesz, file);
            }
            cursor += name_size;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_descsz == 20 &&
                std::strcmp(name, "GNU") == 0) {
                std::fseek(file, static_cast<long>(cursor), SEEK_SET);
                found = std::fread(output, 20, 1, file) == 1;
                break;
            }
            cursor += data_size;
        }
    }
    std::fclose(file);
    return found;
}

bool LoadReplay() {
    FILE* file = std::fopen(kReplayPath, "rb");
    if (!file) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "replay open failed errno=%d", errno);
        return false;
    }
    ReplayHeader header{};
    if (std::fread(&header, sizeof(header), 1, file) != 1 ||
        std::memcmp(header.magic, kReplayMagic, sizeof(kReplayMagic)) != 0 ||
        header.version != kReplayVersion ||
        header.header_size != sizeof(ReplayHeader) ||
        header.frame_size != sizeof(ReplayFrame) || header.frame_count == 0 ||
        header.frame_count > kMaxFrames ||
        std::memcmp(header.build_id, kExpectedBuildId,
                    sizeof(kExpectedBuildId)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "replay header validation failed");
        std::fclose(file);
        return false;
    }
    std::vector<ReplayFrame> frames(header.frame_count);
    if (std::fread(frames.data(), sizeof(ReplayFrame), frames.size(), file) !=
            frames.size() ||
        std::fgetc(file) != EOF) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "replay length validation failed");
        std::fclose(file);
        return false;
    }
    std::fclose(file);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto& frame = frames[index];
        if ((frame.flags & (kFrameSteeringValid | kFrameLongitudinalValid |
                            kFrameValueCValid)) !=
                (kFrameSteeringValid | kFrameLongitudinalValid |
                 kFrameValueCValid) ||
            !IsFiniteAxis(frame.steering) ||
            !IsFiniteAxis(frame.longitudinal) ||
            !IsFiniteAxis(frame.value_c)) {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "invalid replay frame index=%zu", index);
            return false;
        }
    }
    g_frames.swap(frames);
    return true;
}

extern "C" __attribute__((noinline)) void SourceProxyBeginTick(
    void* controller_pointer) {
    g_swap = {};
    const RunMode mode = g_mode.load(std::memory_order_acquire);
    if (g_disabled.load(std::memory_order_acquire) != 0 ||
        mode == RunMode::kDisabled || controller_pointer == nullptr)
        return;
    const auto controller =
        reinterpret_cast<std::uintptr_t>(controller_pointer);
    const auto base = g_guest_base.load(std::memory_order_acquire);
    auto* controller_vtable_slot =
        reinterpret_cast<std::uintptr_t*>(controller);
    const std::uintptr_t controller_vtable =
        __atomic_load_n(controller_vtable_slot, __ATOMIC_ACQUIRE);
    g_swap.controller = controller;
    g_swap.observed = true;
    if (controller_vtable != base + kControllerVtableRva) {
        if (g_identity_mismatch_logged.exchange(1, std::memory_order_acq_rel) ==
            0) {
            __android_log_print(
                mode == RunMode::kObserve ? ANDROID_LOG_WARN
                                          : ANDROID_LOG_ERROR,
                kTag,
                "controller identity mismatch controller=%p actual_vtable=%p "
                "expected_vtable=%p mode=%u",
                controller_pointer, reinterpret_cast<void*>(controller_vtable),
                reinterpret_cast<void*>(base + kControllerVtableRva),
                static_cast<unsigned>(mode));
        }
        // Observation is intentionally non-invasive: an alternate controller
        // implementation is still a valid tick source to count. Replay remains
        // fail-closed until that concrete runtime identity is understood.
        if (mode == RunMode::kObserve) return;
        g_disabled.store(1, std::memory_order_release);
        return;
    }
    if (mode == RunMode::kObserve) return;
    if (g_frames.empty()) {
        g_disabled.store(1, std::memory_order_release);
        return;
    }
    const auto previous_controller =
        g_last_controller.exchange(controller, std::memory_order_acq_rel);
    if (previous_controller != controller)
        g_next_frame.store(0, std::memory_order_release);

    const std::uint32_t index =
        g_next_frame.load(std::memory_order_acquire);
    if (index >= g_frames.size()) {
        g_disabled.store(1, std::memory_order_release);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "replay complete frames=%zu", g_frames.size());
        return;
    }

    void** source_slot = reinterpret_cast<void**>(
        controller + kControllerSourceOffset);
    void* original_source = __atomic_load_n(source_slot, __ATOMIC_ACQUIRE);
    if (original_source == nullptr) {
        g_disabled.store(1, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "source pointer unavailable; replay disabled");
        return;
    }
    auto* source_vtable_slot =
        reinterpret_cast<std::uintptr_t*>(original_source);
    const std::uintptr_t source_vtable =
        __atomic_load_n(source_vtable_slot, __ATOMIC_ACQUIRE);
    if (source_vtable != base + kKeyboardSourceVtableRva) {
        g_disabled.store(1, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "keyboard source identity mismatch; replay disabled");
        return;
    }

    const ReplayFrame& frame = g_frames[index];
    g_proxy.vtable = g_proxy_vtable;
    g_proxy.original_source = original_source;
    std::memcpy(&g_proxy.value_a_bits, &frame.steering,
                sizeof(frame.steering));
    std::memcpy(&g_proxy.value_b_bits, &frame.longitudinal,
                sizeof(frame.longitudinal));
    std::memcpy(&g_proxy.value_c_bits, &frame.value_c,
                sizeof(frame.value_c));

    // x0 and its source pointer are being used by the game on this exact call,
    // so direct atomic loads/stores are safe here and avoid any per-tick file
    // descriptor/syscall activity that would distort timing.
    __atomic_store_n(source_slot, static_cast<void*>(&g_proxy),
                     __ATOMIC_RELEASE);
    g_swap.original_source = original_source;
    g_swap.swapped = true;
}

extern "C" __attribute__((noinline)) void SourceProxyEndTick(
    void* controller_pointer) {
    if (!g_swap.observed) return;
    if (!g_swap.swapped) {
        const std::uint32_t observed =
            g_observed_ticks.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (observed % 300 == 0)
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "observe progress=%u", observed);
        g_swap = {};
        return;
    }
    const auto controller =
        reinterpret_cast<std::uintptr_t>(controller_pointer);
    if (controller != g_swap.controller) {
        g_disabled.store(1, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "tick controller changed before restore");
    }
    void** source_slot = reinterpret_cast<void**>(
        g_swap.controller + kControllerSourceOffset);
    void* current_source = __atomic_load_n(source_slot, __ATOMIC_ACQUIRE);
    if (current_source != &g_proxy) {
        g_disabled.store(1, std::memory_order_release);
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "source restore validation failed");
        g_swap = {};
        return;
    }
    __atomic_store_n(source_slot, g_swap.original_source, __ATOMIC_RELEASE);
    const std::uint32_t completed =
        g_next_frame.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (completed % 300 == 0)
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "replay progress=%u/%zu", completed,
                            g_frames.size());
    g_swap = {};
}

// Replaces the registry's indirect BLR X8 call. Calls aimed at UpdatePerTick
// receive begin/end lifecycle handling; every other vtable+0x40 target passes
// through untouched. Calling the intact function entry avoids unsupported
// Houdini transfers into the middle of a translated guest function.
extern "C" __attribute__((naked)) void SourceProxyTickWrapper() {
    __asm__ volatile(
        "sub sp, sp, #0xe0\n"
        "stp x0, x1, [sp, #0x00]\n"
        "stp x2, x3, [sp, #0x10]\n"
        "stp x4, x5, [sp, #0x20]\n"
        "stp x6, x7, [sp, #0x30]\n"
        "str x30, [sp, #0x40]\n"
        "str x8, [sp, #0x48]\n"
        "stp q0, q1, [sp, #0x50]\n"
        "stp q2, q3, [sp, #0x70]\n"
        "stp q4, q5, [sp, #0x90]\n"
        "stp q6, q7, [sp, #0xb0]\n"
        "adrp x17, :got:g_tick_continue\n"
        "ldr x17, [x17, #:got_lo12:g_tick_continue]\n"
        "ldr x17, [x17]\n"
        "cmp x8, x17\n"
        "b.ne 1f\n"
        "bl SourceProxyBeginTick\n"
        "ldp x0, x1, [sp, #0x00]\n"
        "ldp x2, x3, [sp, #0x10]\n"
        "ldp x4, x5, [sp, #0x20]\n"
        "ldp x6, x7, [sp, #0x30]\n"
        "ldp q0, q1, [sp, #0x50]\n"
        "ldp q2, q3, [sp, #0x70]\n"
        "ldp q4, q5, [sp, #0x90]\n"
        "ldp q6, q7, [sp, #0xb0]\n"
        "ldr x17, [sp, #0x48]\n"
        "blr x17\n"
        "str x0, [sp, #0x48]\n"
        "str q0, [sp, #0x50]\n"
        "ldr x0, [sp, #0x00]\n"
        "bl SourceProxyEndTick\n"
        "ldr x0, [sp, #0x48]\n"
        "ldr q0, [sp, #0x50]\n"
        "ldr x30, [sp, #0x40]\n"
        "add sp, sp, #0xe0\n"
        "ret\n"
        "1:\n"
        "ldp x0, x1, [sp, #0x00]\n"
        "ldp x2, x3, [sp, #0x10]\n"
        "ldp x4, x5, [sp, #0x20]\n"
        "ldp x6, x7, [sp, #0x30]\n"
        "ldp q0, q1, [sp, #0x50]\n"
        "ldp q2, q3, [sp, #0x70]\n"
        "ldp q4, q5, [sp, #0x90]\n"
        "ldp q6, q7, [sp, #0xb0]\n"
        "ldr x17, [sp, #0x48]\n"
        "blr x17\n"
        "ldr x30, [sp, #0x40]\n"
        "add sp, sp, #0xe0\n"
        "ret\n"
        ::: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x17",
            "x30", "memory");
}

void EncodeAbsoluteJump16(std::uint8_t output[16],
                          std::uintptr_t destination) {
    constexpr std::uint32_t ldr_x17_literal = 0x58000051;
    constexpr std::uint32_t branch_x17 = 0xD61F0220;
    std::memcpy(output, &ldr_x17_literal, sizeof(ldr_x17_literal));
    std::memcpy(output + 4, &branch_x17, sizeof(branch_x17));
    std::memcpy(output + 8, &destination, sizeof(destination));
}

bool EncodeBranchImmediate(std::uint8_t output[4], std::uintptr_t pc,
                           std::uintptr_t target, bool link) {
    if ((pc & 3u) != 0 || (target & 3u) != 0) return false;
    const std::int64_t byte_delta =
        static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
    if ((byte_delta & 3) != 0) return false;
    const std::int64_t word_delta = byte_delta / 4;
    if (word_delta < -(std::int64_t{1} << 25) ||
        word_delta >= (std::int64_t{1} << 25))
        return false;
    const std::uint32_t instruction =
        (link ? 0x94000000u : 0x14000000u) |
        (static_cast<std::uint32_t>(word_delta) & 0x03FFFFFFu);
    std::memcpy(output, &instruction, sizeof(instruction));
    return true;
}

bool Install(std::uintptr_t base) {
    const std::uintptr_t target = base + kUpdatePerTickRva;
    const std::uintptr_t callsite = base + kDispatchCallsiteRva;
    std::uint8_t actual[sizeof(kUpdatePerTickSignature)]{};
    if (!ReadProcMem(target, actual, sizeof(actual)) ||
        std::memcmp(actual, kUpdatePerTickSignature, sizeof(actual)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "UpdatePerTick signature mismatch");
        return false;
    }
    std::uint8_t actual_callsite[sizeof(kDispatchCallsiteSignature)]{};
    if (!ReadProcMem(callsite, actual_callsite, sizeof(actual_callsite)) ||
        std::memcmp(actual_callsite, kDispatchCallsiteSignature,
                    sizeof(actual_callsite)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "dispatch callsite signature mismatch");
        return false;
    }
    const std::uintptr_t stub = base + kTickStubRva;
    std::uint8_t original_stub[16]{};
    if (!ReadProcMem(stub, original_stub, sizeof(original_stub)) ||
        std::memcmp(original_stub, kTickStubPaddingSignature,
                    sizeof(original_stub)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "verified tick stub padding mismatch");
        return false;
    }
    std::uint8_t stub_code[16]{};
    EncodeAbsoluteJump16(
        stub_code,
        reinterpret_cast<std::uintptr_t>(&SourceProxyTickWrapper));
    if (!WriteProcMem(stub, stub_code, sizeof(stub_code))) return false;
    __builtin___clear_cache(reinterpret_cast<char*>(stub),
                            reinterpret_cast<char*>(stub + 16));

    std::uint8_t branch[4]{};
    // Publish the continuation before the entry can possibly branch into the
    // wrapper. The game thread must never observe an armed hook with a null
    // continuation, even for the few instructions between patch and verify.
    g_tick_continue = target + 4;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (!EncodeBranchImmediate(branch, callsite, stub, true) ||
        !WriteProcMem(callsite, branch, sizeof(branch))) {
        g_tick_continue = 0;
        WriteProcMem(stub, original_stub, sizeof(original_stub));
        return false;
    }
    std::uint8_t verify[4]{};
    if (!ReadProcMem(callsite, verify, sizeof(verify)) ||
        std::memcmp(verify, branch, sizeof(branch)) != 0) {
        WriteProcMem(callsite, kDispatchCallsiteSignature,
                     sizeof(kDispatchCallsiteSignature));
        WriteProcMem(stub, original_stub, sizeof(original_stub));
        g_tick_continue = 0;
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(callsite),
                            reinterpret_cast<char*>(callsite + 4));
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "armed mode=%u frames=%zu callsite=%p target=%p "
                        "stub_rva=0x%x",
                        static_cast<unsigned>(
                            g_mode.load(std::memory_order_acquire)),
                        g_frames.size(), reinterpret_cast<void*>(callsite),
                        reinterpret_cast<void*>(target),
                        static_cast<unsigned>(kTickStubRva));
    return true;
}

void* Worker(void*) {
    if (access(kEnableMarker, F_OK) != 0) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "enable marker absent; disabled");
        return nullptr;
    }
    const RunMode mode = ReadRunMode();
    if (mode == RunMode::kDisabled) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "config must be exactly mode=observe or mode=replay");
        return nullptr;
    }
    if (mode == RunMode::kReplay && !LoadReplay()) return nullptr;
    g_mode.store(mode, std::memory_order_release);
    for (int attempt = 0; attempt < 1200; ++attempt) {
        GameMapping mapping{};
        dl_iterate_phdr(FindGuestModule, nullptr);
        const auto base = g_guest_base.load(std::memory_order_acquire);
        std::uint8_t build_id[20]{};
        if (base != 0 && FindGameMapping(&mapping) &&
            ReadBuildId(mapping.path, build_id)) {
            if (std::memcmp(build_id, kExpectedBuildId,
                            sizeof(kExpectedBuildId)) != 0) {
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                                    "game Build ID mismatch; disabled");
                return nullptr;
            }
            if (!Install(base))
                __android_log_print(ANDROID_LOG_ERROR, kTag,
                                    "install failed; disabled");
            return nullptr;
        }
        usleep(100000);
    }
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "game module not found within 120 seconds");
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded prototype=source-proxy-v1 default=disabled");
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, Worker, nullptr) == 0)
        pthread_detach(worker);
}

}  // namespace

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_payload_protocol() {
    return 3;
}
