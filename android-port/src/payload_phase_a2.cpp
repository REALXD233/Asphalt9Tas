// Staging variant of payload.cpp for the PhysicsDispatchProbe first-round
// safe test (PROGRESS.md round 1). Changes vs payload.cpp (baseline 773423849a...):
//   1. Default-off device-side enable marker: probe installs only when
//      /data/local/tmp/a9tas-enable-physics-probe exists.
//   2. Patch write via /proc/self/mem pwrite — no mprotect anywhere. Verified
//      in the isolated probe_selftest: mprotect(+W) on a page with existing
//      Houdini translations crashes the process, while /proc/self/mem writes
//      to read-only pages work and Houdini re-reads patched bytes (lateG).
//   3. Compiled leaf probe (Design G, validated by probe_selftest probeG):
//      entry patch jumps (br x17) into ProbePhysicsDispatch, a compiled
//      function whose asm replays the game's copied 16-byte prologue stores
//      (saving the ORIGINAL x19-x22/x30), counts, re-runs the 4th prologue
//      instruction (ldp x8,x9,[x0]) and absolute-jumps to target+16. The
//      game body's own epilogue then returns directly to the original caller
//      with the stack balanced. No naked entry blocks anywhere in the chain
//      (Houdini mangles integer registers at naked entries with early memory
//      access — probe_selftest dummyA/B/C).
//   4. Misleading "hooks_enabled=0" log removed; probe decision logged.
// Keep the original payload.cpp and build/liba9tas_payload.so untouched.
#include <android/log.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr const char* kTag = "A9TAS_PAYLOAD";
constexpr const char* kEnableMarkerPath = "/data/local/tmp/a9tas-enable-physics-probe";
constexpr std::uint8_t kExpectedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

std::atomic<bool> g_loaded{false};
std::atomic<bool> g_build_verified{false};
std::atomic<std::uintptr_t> g_game_base{0};
std::atomic<std::uintptr_t> g_guest_game_base{0};
std::atomic<std::uint64_t> g_physics_dispatch_calls{0};
// target+16 of the dispatch function; consumed by the probe asm via GOT.
extern "C" std::uintptr_t g_trampoline_target = 0;
// second probe target (frame dispatcher 0x38b78e4) - separate global.
extern "C" std::uintptr_t g_trampoline_target_frame = 0;
constexpr std::uintptr_t kFrameDispatchOffset = 0x38b78e4;
// first 16 bytes of 0x38b78e4: sub sp,sp,#0x30 / str x20,[sp,#0x10] /
// stp x19,x30,[sp,#0x20] / ldr x8,[x1]  (relocatable, no PC-relative)
// NOTE: little-endian memory byte order (word d100c3ff -> ff c3 00 d1).
constexpr std::uint8_t kFrameDispatchSignature[16] = {
    0xff, 0xc3, 0x00, 0xd1, 0xf4, 0x0b, 0x00, 0xf9,
    0xf3, 0x7b, 0x02, 0xa9, 0x28, 0x00, 0x40, 0xf9,
};
// return addresses of the 5 known bl callers (caller+4)
constexpr std::uintptr_t kFrameCallerLrs[5] = {
    0x3794c78, 0x3846518, 0x38469d0, 0x3a5ce2c, 0x3a5d7f4,
};
std::atomic<std::uint64_t> g_frame_caller_calls[6] = {};  // 5 known + other
// distinct LR capture: up to 16 distinct return addresses with counts
std::atomic<std::uint64_t> g_lr_slots[32] = {};  // pairs: (lr, count)
// PhysicsContext instance discovery (read-only): scan rw segments for the
// vtable pointer guest_base+0x8103830, then read the float at +0x178.
constexpr std::uintptr_t kPhysicsVtableOffsetA = 0x8103830;
std::uintptr_t g_physics_ctx[16] = {};  // found instance addresses
float g_physics_interval[16] = {};

struct CandidateSignature {
    const char* name;
    std::uintptr_t offset;
    const std::uint8_t* bytes;
    size_t size;
};

constexpr std::uint8_t kPhysicsCtor[] = {
    0xf4,0x0f,0x1e,0xf8,0xf3,0x7b,0x01,0xa9,0xf4,0x03,0x01,0xaa,0xf3,0x03,0x00,0xaa,
    0x38,0x00,0x00,0x94,0x68,0x42,0x02,0xb0,0x08,0xc1,0x20,0x91,0x60,0xa2,0x02,0x91,
};
constexpr std::uint8_t kPhysicsIntervalGet[] = {
    0x09,0x78,0x41,0xb9,0x09,0x01,0x00,0xb9,0xc0,0x03,0x5f,0xd6,
};
constexpr std::uint8_t kPhysicsDispatch[] = {
    0xf6,0x0f,0x1d,0xf8,0xf5,0x53,0x01,0xa9,0xf3,0x7b,0x02,0xa9,0x08,0x24,0x40,0xa9,
    0xf4,0x03,0x01,0xaa,0xf3,0x03,0x00,0xaa,0x2a,0x00,0x80,0x52,0x28,0x01,0x08,0xcb,
};
constexpr std::uint8_t kHidJni[] = {
    0xe0,0x03,0x02,0x2a,0x01,0x00,0x00,0x14,0xe8,0x0f,0x1d,0xfc,0xf5,0x53,0x01,0xa9,
    0xf3,0x7b,0x02,0xa9,0x88,0x43,0x02,0xd0,0x14,0xb5,0x44,0xf9,0x08,0x1c,0xa0,0x4e,
};
constexpr std::uint8_t kKeyboardJni[] = {
    0xe0,0x03,0x02,0x2a,0x01,0x00,0x00,0x14,0xe8,0x0f,0x1d,0xfc,0xf5,0x53,0x01,0xa9,
    0xf3,0x7b,0x02,0xa9,0x88,0x43,0x02,0xb0,0x14,0xe1,0x44,0xf9,0x08,0x1c,0xa0,0x4e,
};
constexpr std::uint8_t kReplayReader[] = {
    0x13,0x00,0x80,0x12,0xe8,0x0f,0x00,0xf9,0xe8,0x57,0x41,0xb9,0x3f,0x03,0x08,0x6b,
    0x02,0x19,0x00,0x54,0xe0,0xb3,0x40,0xf9,0x00,0xe4,0x00,0x6f,0x7f,0x23,0x00,0xb9,
};

constexpr CandidateSignature kCandidates[] = {
    {"physics_ctor", 0x38b6e60, kPhysicsCtor, sizeof(kPhysicsCtor)},
    {"physics_interval_get", 0x38b77c0, kPhysicsIntervalGet, sizeof(kPhysicsIntervalGet)},
    {"physics_dispatch", 0x38b7930, kPhysicsDispatch, sizeof(kPhysicsDispatch)},
    {"hid_jni", 0x5d4a69c, kHidJni, sizeof(kHidJni)},
    {"keyboard_jni", 0x5d4b368, kKeyboardJni, sizeof(kKeyboardJni)},
    {"replay_reader", 0x61b2a80, kReplayReader, sizeof(kReplayReader)},
};

constexpr std::uintptr_t kPhysicsVtableOffset = 0x8103830;
constexpr std::uintptr_t kPhysicsDispatchOffset = 0x38b7930;
constexpr std::uintptr_t kPhysicsVtableMethods[] = {
    0x38b6f94, 0x38b7088, 0x38b70a0, 0x38b7258, 0x38b76c8, 0x38b76e0,
    0x38b76f4, 0x38b77ac, 0x38b77b4, 0x38b77c0, 0x38b77cc, 0x38b7840,
    0x38b79d4, 0x38b77a4, 0x38b79fc, 0x38b7b54, 0x38b7b5c, 0x38b7a00,
    0x38b7a1c, 0x38b7a3c, 0x38b7a54,
};

// Counting entry called by the probe asm. Ignores its arguments.
extern "C" __attribute__((noinline)) void PhysicsDispatchProbeImpl(void*,
                                                                  const void*) {
    g_physics_dispatch_calls.fetch_add(1, std::memory_order_relaxed);
}

// Design G probe — validated by probe_selftest (probeG mode). Compiled leaf
// function; the asm replays the game's copied prologue stores, parks
// context/frame in x19/x20 (the game body overwrites both at +16 before the
// epilogue restores them from the frame), counts, restores x0/x1, re-runs the
// 4th prologue instruction (ldp x8,x9,[x0]) and absolute-jumps to target+16
// via g_trampoline_target. The game epilogue then rets to the original caller
// with the stack balanced. The clobber list intentionally excludes x19/x20/
// x30 so the compiler allocates no frame (leaf) — sp stays untouched.
extern "C" __attribute__((noinline)) void PhysicsDispatchProbe(void* context,
                                                               const void* frame) {
    register void* ctx __asm__("x0") = context;
    register const void* frm __asm__("x1") = frame;
    __asm__ volatile(
        // copied game prologue bytes 0-11 (x19-x22/x30 still original here)
        "str x22, [sp, #-0x30]!\n"
        "stp x21, x20, [sp, #0x10]\n"
        "stp x19, x30, [sp, #0x20]\n"
        "mov x19, x0\n"
        "mov x20, x1\n"
        "bl PhysicsDispatchProbeImpl\n"
        "mov x0, x19\n"
        "mov x1, x20\n"
        "ldp x8, x9, [x0]\n"
        "adrp x17, :got:g_trampoline_target\n"
        "ldr x17, [x17, #:got_lo12:g_trampoline_target]\n"
        "ldr x17, [x17]\n"
        "br x17\n"
        :: "r"(ctx), "r"(frm)
        : "x8", "x9", "x17", "cc", "memory");
}

// 16-byte entry patch: ldr x17, #8; br x17; .quad target
void WriteAbsoluteJump(std::uint8_t* destination, std::uintptr_t target) {
    constexpr std::uint32_t load_x17_literal = 0x58000051;
    constexpr std::uint32_t branch_x17 = 0xD61F0220;
    std::memcpy(destination, &load_x17_literal, sizeof(load_x17_literal));
    std::memcpy(destination + 4, &branch_x17, sizeof(branch_x17));
    std::memcpy(destination + 8, &target, sizeof(target));
}

// Write 16 bytes to a read-only page via /proc/self/mem (validated in
// probe_selftest memfile mode). No mprotect: mprotect(+W) on a page with
// existing Houdini translations kills the process (probe_selftest probeC).
bool WriteProcMem(std::uintptr_t address, const std::uint8_t* bytes) {
    const int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "open /proc/self/mem failed errno=%d", errno);
        return false;
    }
    const ssize_t written = pwrite(fd, bytes, 16, static_cast<off_t>(address));
    close(fd);
    if (written != 16) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "proc mem pwrite=%zd errno=%d", written, errno);
        return false;
    }
    return true;
}

bool InstallPhysicsDispatchProbe(std::uintptr_t guest_base) {
    auto* target = reinterpret_cast<std::uint8_t*>(guest_base + kPhysicsDispatchOffset);
    if (std::memcmp(target, kPhysicsDispatch, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime dispatch bytes mismatch; probe disabled");
        return false;
    }

    g_trampoline_target = reinterpret_cast<std::uintptr_t>(target + 16);
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&PhysicsDispatchProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "dispatch patch write failed; probe disabled");
        g_trampoline_target = 0;
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));

    // read-back verification (fail closed if the patch did not land)
    std::uint8_t back[16]{};
    std::memcpy(back, target, 16);
    if (std::memcmp(back, patch, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "dispatch patch read-back mismatch; probe disabled");
        g_trampoline_target = 0;
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "physics dispatch count probe installed target=%p "
                        "target16=%p readback_ok",
                        target, reinterpret_cast<void*>(g_trampoline_target));
    return true;
}


// Frame-dispatcher probe impl: histogram by caller return address, plus
// distinct-LR capture (16 slots) to identify unknown callers.
extern "C" __attribute__((noinline)) void FrameDispatchProbeImpl(std::uintptr_t lr) {
    // kFrameCallerLrs are STATIC offsets; compare against runtime addresses.
    const std::uintptr_t base = g_guest_game_base.load(std::memory_order_acquire);
    for (size_t i = 0; i < 5; ++i) {
        if (base != 0 && lr == base + kFrameCallerLrs[i]) {
            g_frame_caller_calls[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_frame_caller_calls[5].fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < 16; ++i) {
        const std::uint64_t slot_lr = g_lr_slots[i * 2].load(std::memory_order_relaxed);
        if (slot_lr == lr) {
            g_lr_slots[i * 2 + 1].fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (slot_lr == 0) {
            std::uint64_t expected = 0;
            if (g_lr_slots[i * 2].compare_exchange_strong(expected, lr,
                                                          std::memory_order_relaxed)) {
                g_lr_slots[i * 2 + 1].store(1, std::memory_order_relaxed);
                return;
            }
        }
    }
}

// Design G probe for 0x38b78e4 (frame dispatcher). The copied prologue stores
// only x20/x19/x30; the body at +16 immediately overwrites x19/x20, so the
// parked context/frame live there safely. LR (x30) is passed to the count
// function; the original x30 was already stored into the game frame by the
// copied stp x19, x30.
extern "C" __attribute__((noinline)) void FrameDispatchProbe(void* context,
                                                             const void* frame) {
    register void* ctx __asm__("x0") = context;
    register const void* frm __asm__("x1") = frame;
    __asm__ volatile(
        // copied prologue bytes 0-11
        "sub sp, sp, #0x30\n"
        "str x20, [sp, #0x10]\n"
        "stp x19, x30, [sp, #0x20]\n"
        "mov x19, x0\n"
        "mov x20, x1\n"
        "mov x0, x30\n"
        "bl FrameDispatchProbeImpl\n"
        "mov x0, x19\n"
        "mov x1, x20\n"
        "ldr x8, [x1]\n"
        "adrp x17, :got:g_trampoline_target_frame\n"
        "ldr x17, [x17, #:got_lo12:g_trampoline_target_frame]\n"
        "ldr x17, [x17]\n"
        "br x17\n"
        :: "r"(ctx), "r"(frm)
        : "x8", "x17", "cc", "memory");
}

// Install the frame-dispatcher probe via /proc/self/mem (same scheme as the
// physics probe).
bool InstallFrameDispatchProbe(std::uintptr_t guest_base) {
    auto* target = reinterpret_cast<std::uint8_t*>(guest_base + kFrameDispatchOffset);
    if (std::memcmp(target, kFrameDispatchSignature, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime frame dispatch bytes mismatch; probe disabled");
        return false;
    }
    g_trampoline_target_frame = reinterpret_cast<std::uintptr_t>(target + 16);
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&FrameDispatchProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "frame dispatch patch write failed; probe disabled");
        g_trampoline_target_frame = 0;
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    std::uint8_t back[16]{};
    std::memcpy(back, target, 16);
    if (std::memcmp(back, patch, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "frame dispatch patch read-back mismatch; probe disabled");
        g_trampoline_target_frame = 0;
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "frame dispatch caller probe installed target=%p target16=%p "
                        "readback_ok",
                        target, reinterpret_cast<void*>(g_trampoline_target_frame));
    return true;
}

void* ProbeReporter(void*) {
    std::uint64_t previous = 0;
    std::uint64_t frame_previous[6] = {};
    // 600 samples x 5s = 50 minutes of coverage for the three-state test;
    // the payload is gated by the enable marker, so this only runs when the
    // probe is actually installed.
    for (int sample = 1; sample <= 600; ++sample) {
        sleep(5);
        const std::uint64_t current =
            g_physics_dispatch_calls.load(std::memory_order_relaxed);
        std::uint64_t frame_total = 0;
        std::uint64_t frame_deltas[6] = {};
        for (size_t i = 0; i < 6; ++i) {
            const std::uint64_t c =
                g_frame_caller_calls[i].load(std::memory_order_relaxed);
            frame_deltas[i] = c - frame_previous[i];
            frame_previous[i] = c;
            frame_total += c;
        }
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "phys sample=%d delta=%llu | frame total=%llu "
                            "c0=%llu c1=%llu c2=%llu c3=%llu c4=%llu other=%llu",
                            sample, static_cast<unsigned long long>(current - previous),
                            static_cast<unsigned long long>(frame_total),
                            static_cast<unsigned long long>(frame_deltas[0]),
                            static_cast<unsigned long long>(frame_deltas[1]),
                            static_cast<unsigned long long>(frame_deltas[2]),
                            static_cast<unsigned long long>(frame_deltas[3]),
                            static_cast<unsigned long long>(frame_deltas[4]),
                            static_cast<unsigned long long>(frame_deltas[5]));
        // distinct LR dump (first 5 nonzero slots)
        std::uint64_t dumped = 0;
        for (size_t i = 0; i < 16 && dumped < 5; ++i) {
            const std::uint64_t slot_lr = g_lr_slots[i * 2].load(std::memory_order_relaxed);
            if (slot_lr == 0) continue;
            const std::uint64_t slot_cnt =
                g_lr_slots[i * 2 + 1].load(std::memory_order_relaxed);
            __android_log_print(ANDROID_LOG_INFO, kTag, "  lr[%zu]=0x%llx cnt=%llu",
                                i, static_cast<unsigned long long>(slot_lr),
                                static_cast<unsigned long long>(slot_cnt));
            ++dumped;
        }
        previous = current;
        if (g_physics_ctx[0] != 0) {
            float interval = 0.0f;
            std::memcpy(&interval,
                        reinterpret_cast<const void*>(g_physics_ctx[0] + 0x178),
                        sizeof(interval));
            if (interval != g_physics_interval[0] || sample % 12 == 0) {
                g_physics_interval[0] = interval;
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "  ctx0 interval=%.8f (0x%08x)",
                                    static_cast<double>(interval),
                                    static_cast<unsigned>(*reinterpret_cast<std::uint32_t*>(&interval)));
            }
        }
    }
    return nullptr;
}

struct GameMapping {
    std::uintptr_t base{};
    char path[1024]{};
};

bool FindGameMapping(GameMapping* mapping) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) return false;
    char line[2048]{};
    bool found = false;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[5]{};
        char path[1024]{};
        const int fields = std::sscanf(line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
                                       &start, &end, perms, &offset, path);
        if (fields == 5 && offset == 0 && std::strstr(path, "libAsphalt9.so") != nullptr) {
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
    if (info != nullptr && info->dlpi_name != nullptr &&
        std::strstr(info->dlpi_name, "libAsphalt9.so") != nullptr) {
        g_guest_game_base.store(static_cast<std::uintptr_t>(info->dlpi_addr),
                                std::memory_order_release);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "guest game base=%p phnum=%u path=%s",
                            reinterpret_cast<void*>(info->dlpi_addr),
                            static_cast<unsigned>(info->dlpi_phnum), info->dlpi_name);
        return 1;
    }
    return 0;
}

bool ReadBuildId(const char* path, std::uint8_t out[20]) {
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) return false;

    Elf64_Ehdr header{};
    const bool header_ok = std::fread(&header, sizeof(header), 1, file) == 1 &&
                           std::memcmp(header.e_ident, ELFMAG, SELFMAG) == 0 &&
                           header.e_ident[EI_CLASS] == ELFCLASS64 &&
                           header.e_machine == EM_AARCH64 &&
                           header.e_phentsize == sizeof(Elf64_Phdr);
    if (!header_ok) {
        std::fclose(file);
        return false;
    }

    bool found = false;
    for (std::uint16_t index = 0; index < header.e_phnum && !found; ++index) {
        Elf64_Phdr program{};
        if (std::fseek(file, static_cast<long>(header.e_phoff) +
                            static_cast<long>(index) * sizeof(program), SEEK_SET) != 0 ||
            std::fread(&program, sizeof(program), 1, file) != 1) break;
        if (program.p_type != PT_NOTE || program.p_filesz > 1024 * 1024) continue;

        std::uint64_t cursor = program.p_offset;
        const std::uint64_t end = program.p_offset + program.p_filesz;
        while (cursor + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr note{};
            if (std::fseek(file, static_cast<long>(cursor), SEEK_SET) != 0 ||
                std::fread(&note, sizeof(note), 1, file) != 1) break;
            cursor += sizeof(note);
            const std::uint64_t name_size = (note.n_namesz + 3u) & ~3u;
            const std::uint64_t desc_size = (note.n_descsz + 3u) & ~3u;
            if (cursor + name_size + desc_size > end) break;

            char name[16]{};
            if (note.n_namesz < sizeof(name)) {
                std::fseek(file, static_cast<long>(cursor), SEEK_SET);
                std::fread(name, 1, note.n_namesz, file);
            }
            cursor += name_size;
            if (note.n_type == NT_GNU_BUILD_ID && note.n_descsz == 20 &&
                std::strcmp(name, "GNU") == 0) {
                std::fseek(file, static_cast<long>(cursor), SEEK_SET);
                found = std::fread(out, 20, 1, file) == 1;
                break;
            }
            cursor += desc_size;
        }
    }
    std::fclose(file);
    return found;
}

void FormatBuildId(const std::uint8_t id[20], char text[41]) {
    static constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 20; ++i) {
        text[i * 2] = hex[id[i] >> 4];
        text[i * 2 + 1] = hex[id[i] & 0xf];
    }
    text[40] = '\0';
}


struct MappedSegment {
    std::uintptr_t start;
    std::uintptr_t end;
};

// Read-only scan: find objects whose vtable pointer == guest_base+vtable_off.
// Reads are direct (we are inside the game process).
size_t ScanForVtable(std::uintptr_t guest_base, std::uintptr_t vtable_off,
                     std::uintptr_t* out, size_t max_out) {
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) return 0;
    char line[2048]{};
    size_t found = 0;
    const std::uintptr_t vtable_addr = guest_base + vtable_off;
    while (std::fgets(line, sizeof(line), maps) != nullptr && found < max_out) {
        unsigned long long start = 0, end = 0;
        char perms[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &start, &end, perms) != 3) continue;
        if (perms[1] != 'w') continue;  // writable segments only
        // skip stack/guard regions quickly by size cap
        const std::uintptr_t seg_start = static_cast<std::uintptr_t>(start);
        const std::uintptr_t seg_end = static_cast<std::uintptr_t>(end);
        if (seg_end - seg_start > (256ull << 20)) continue;
        const auto* p = reinterpret_cast<const std::uintptr_t*>(seg_start);
        const size_t count = (seg_end - seg_start) / sizeof(std::uintptr_t);
        for (size_t i = 0; i < count && found < max_out; ++i) {
            if (p[i] == vtable_addr) {
                out[found++] = seg_start + i * sizeof(std::uintptr_t);
            }
        }
    }
    std::fclose(maps);
    return found;
}

void LogPhysicsContexts(std::uintptr_t guest_base) {
    const size_t n = ScanForVtable(guest_base, kPhysicsVtableOffsetA,
                                   g_physics_ctx, 16);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "physics ctx scan: found=%zu vtable=%p",
                        n, reinterpret_cast<void*>(guest_base + kPhysicsVtableOffsetA));
    for (size_t i = 0; i < n; ++i) {
        float interval = 0.0f;
        std::memcpy(&interval,
                    reinterpret_cast<const void*>(g_physics_ctx[i] + 0x178),
                    sizeof(interval));
        g_physics_interval[i] = interval;
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "  ctx[%zu]=%p interval=%.8f (0x%08x)",
                            i, reinterpret_cast<void*>(g_physics_ctx[i]),
                            static_cast<double>(interval),
                            static_cast<unsigned>(*reinterpret_cast<std::uint32_t*>(&interval)));
    }
}

void* VerificationWorker(void*) {
    // Stability fix (PROGRESS.md round1): the worker's original 1ms polling
    // of /proc/self/maps during game startup correlated with Houdini
    // translation-region crashes (fault 0x100000035 family, 5/5 injected
    // rounds). The idle variant (no worker) was stable. Delay the worker past
    // the startup/crash window and poll 50x slower.
    sleep(120);
    GameMapping mapping{};
    for (int attempt = 0; attempt < 120 && !FindGameMapping(&mapping); ++attempt) {
        usleep(50000);
    }
    if (mapping.base == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "game mapping wait timed out; hooks disabled");
        return nullptr;
    }
    g_game_base.store(mapping.base, std::memory_order_release);
    dl_iterate_phdr(FindGuestGameModule, nullptr);

    std::uint8_t build_id[20]{};
    char build_id_text[41]{};
    const bool read_ok = ReadBuildId(mapping.path, build_id);
    if (read_ok) FormatBuildId(build_id, build_id_text);
    const bool matches = read_ok && std::memcmp(build_id, kExpectedBuildId, 20) == 0;
    g_build_verified.store(matches, std::memory_order_release);
    __android_log_print(matches ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                        "game base=%p build_id=%s verified=%d",
                        reinterpret_cast<void*>(mapping.base),
                        read_ok ? build_id_text : "unreadable", matches);
    if (!matches) return nullptr;

    FILE* game_file = std::fopen(mapping.path, "rb");
    for (const auto& candidate : kCandidates) {
        std::uint8_t actual[64]{};
        const bool read_signature = game_file != nullptr && candidate.size <= sizeof(actual) &&
            std::fseek(game_file, static_cast<long>(candidate.offset), SEEK_SET) == 0 &&
            std::fread(actual, candidate.size, 1, game_file) == 1;
        const bool signature_matches = read_signature &&
            std::memcmp(actual, candidate.bytes, candidate.size) == 0;
        __android_log_print(signature_matches ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                            kTag, "candidate=%s offset=0x%zx signature=%s",
                            candidate.name, static_cast<size_t>(candidate.offset),
                            signature_matches ? "match" : "mismatch");
    }
    if (game_file != nullptr) std::fclose(game_file);

    const std::uintptr_t guest_base = g_guest_game_base.load(std::memory_order_acquire);
    if (guest_base != 0) {
        auto* vtable = reinterpret_cast<const std::uintptr_t*>(guest_base + kPhysicsVtableOffset);
        size_t vtable_matches = 0;
        for (size_t i = 0; i < sizeof(kPhysicsVtableMethods) / sizeof(kPhysicsVtableMethods[0]); ++i) {
            if (vtable[i] == guest_base + kPhysicsVtableMethods[i]) ++vtable_matches;
        }
        __android_log_print(vtable_matches == 21 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                            kTag, "physics_vtable=%p entries_match=%zu/21 interval_slot=%p",
                            vtable, vtable_matches, reinterpret_cast<void*>(vtable[9]));
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "payload_probe=%p physics_dispatch=%p branch_distance=%lld",
                            reinterpret_cast<void*>(&VerificationWorker),
                            reinterpret_cast<void*>(guest_base + 0x38b7930),
                            static_cast<long long>(reinterpret_cast<std::uintptr_t>(&VerificationWorker)) -
                                static_cast<long long>(guest_base + 0x38b7930));

        const bool marker_present = access(kEnableMarkerPath, F_OK) == 0;
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "physics_probe_marker=%d path=%s",
                            marker_present ? 1 : 0, kEnableMarkerPath);
        if (!marker_present) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "physics probe disabled by marker policy");
            return nullptr;
        }

        LogPhysicsContexts(guest_base);
        const bool phys_ok = InstallPhysicsDispatchProbe(guest_base);
        const bool frame_ok = InstallFrameDispatchProbe(guest_base);
        if (phys_ok || frame_ok) {
            pthread_t reporter{};
            if (pthread_create(&reporter, nullptr, ProbeReporter, nullptr) == 0) {
                pthread_detach(reporter);
            }
        }
    }
    return nullptr;
}

__attribute__((constructor)) void OnLoad() {
    g_loaded.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "loaded pid=%d abi=arm64 passive=1 protocol=1", getpid());
    pthread_t worker{};
    if (pthread_create(&worker, nullptr, VerificationWorker, nullptr) == 0) {
        pthread_detach(worker);
    } else {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "verification worker creation failed; hooks disabled");
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) std::uint32_t a9tas_payload_protocol() {
    return 1;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t a9tas_game_base() {
    return g_game_base.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t a9tas_guest_game_base() {
    return g_guest_game_base.load(std::memory_order_acquire);
}

extern "C" __attribute__((visibility("default"))) int a9tas_build_verified() {
    return g_build_verified.load(std::memory_order_acquire) ? 1 : 0;
}
