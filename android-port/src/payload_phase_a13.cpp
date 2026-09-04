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
#include <sys/mman.h>
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
std::atomic<std::uintptr_t> g_frame_container{0};  // frame dispatcher x0 (first call)
std::uintptr_t g_frame_container_dumped = 0;
// ---- Phase B: HID input probe (handler 0x6613d8c) ----
constexpr std::uintptr_t kHidHandlerOffset = 0x6613d8c;
// first 16 bytes: sub sp,#0x20 / str x30,[sp,#0x10] / mov x8,x0 / ldr x0,[x0,#0x110]
constexpr std::uint8_t kHidHandlerSignature[16] = {
    0xff, 0x83, 0x00, 0xd1, 0xfe, 0x0b, 0x00, 0xf9,
    0xe8, 0x03, 0x00, 0xaa, 0x00, 0x88, 0x40, 0xf9,
};
std::atomic<std::uint64_t> g_input_event_counts[22] = {};  // event codes 0-21
std::atomic<std::uint64_t> g_input_total{0};
extern "C" std::uintptr_t g_input_save[3] = {};  // device, event, value(double)
extern "C" std::uintptr_t g_input_original = 0;  // original handler address
extern "C" std::uintptr_t g_input_pending[2] = {};  // event, value bits (pending injection)
extern "C" std::uintptr_t g_input_pending_device = 0;
extern "C" std::uintptr_t g_input_trampoline = 0;  // clean re-entry stub
constexpr const char* kInputInjectPath = "/data/local/tmp/a9tas-inject-input";

// ---- Phase B2: touch dispatch probe (common dispatch point 0x660d1e4) ----
// All touch entries (NativeOnTouch family 0x660be48..0x660bf1c) tail-jump
// here with (x0=platform object from global systemPlatform @0xa5d4980,
// w1=action, w2=flag, x3=payload ptr). platform+0x98 holds the std::function.
constexpr std::uintptr_t kTouchDispatchOffset = 0x660d1e4;
// first 16 bytes: sub sp,#0x30 / stp x3,x30,[sp,#0x18] / stp w2,w1,[sp,#0x28] /
//                  add x8,x0,#0x98
constexpr std::uint8_t kTouchDispatchSignature[16] = {
    0xff, 0xc3, 0x00, 0xd1, 0xe3, 0xfb, 0x01, 0xa9,
    0xe2, 0x07, 0x05, 0x29, 0x08, 0x60, 0x02, 0x91,
};
std::atomic<std::uint64_t> g_touch_total{0};
std::atomic<std::uint64_t> g_touch_w1_counts[64] = {};   // action histogram (w1 & 0x3f)
std::atomic<std::uint64_t> g_touch_w2_counts[8] = {};    // flag histogram (w2 & 7)
std::atomic<std::uint64_t> g_touch_x3_nonzero{0};        // x3 payload pointer present
std::atomic<std::uint64_t> g_touch_platform_ok{0};       // platform object non-null
extern "C" std::uintptr_t g_touch_save[4] = {};  // x0, w1, w2, x3
extern "C" std::uintptr_t g_touch_original = 0;  // original dispatch address
extern "C" std::uintptr_t g_touch_pending[3] = {};  // w1, w2, x3 (pending injection)
extern "C" std::uintptr_t g_touch_trampoline = 0;   // prologue+abs-jump stub
constexpr const char* kTouchInjectPath = "/data/local/tmp/a9tas-inject-touch";

// ---- Phase B3: keyboard probe (NativeKeyboard handler 0x6613f68) ----
// Same calling convention as the HID handler: (x0=device, w1=keycode,
// d0=value double, value != 1.0 means pressed).
constexpr std::uintptr_t kKeyboardHandlerOffset = 0x6613f68;
// first 16 bytes: sub sp,#0x80 / str d8,[sp,#0x40] / stp x23,x22,[sp,#0x50] /
//                  stp x21,x20,[sp,#0x60]
constexpr std::uint8_t kKeyboardHandlerSignature[16] = {
    0xff, 0x03, 0x02, 0xd1, 0xe8, 0x23, 0x00, 0xfd,
    0xf7, 0x5b, 0x05, 0xa9, 0xf5, 0x53, 0x06, 0xa9,
};
std::atomic<std::uint64_t> g_key_counts[256] = {};  // keycode histogram
std::atomic<std::uint64_t> g_key_total{0};
extern "C" std::uintptr_t g_key_save[4] = {};  // device, keycode, value(double), lr
extern "C" std::uintptr_t g_key_original = 0;  // original handler address
extern "C" std::uintptr_t g_key_pending[2] = {};  // keycode, value bits (pending injection)
extern "C" std::uintptr_t g_key_pending_device = 0;
extern "C" std::uintptr_t g_key_trampoline = 0;  // clean re-entry stub
constexpr const char* kKeyInjectPath = "/data/local/tmp/a9tas-inject-key";
// ---- Phase B4: frame-driven key sequence injection ----
// The physics dispatch probe ticks at 60 Hz; it drains this queue so a TAS
// input script runs without any real input events (no player interaction).
struct KeySeqStep {
    std::uint32_t keycode;
    double value;
    std::int32_t wait_frames;  // frames to wait before this step
};
KeySeqStep g_key_seq[256] = {};
std::atomic<std::int32_t> g_key_seq_len{0};
std::atomic<std::int32_t> g_key_seq_pos{0};
std::atomic<std::int32_t> g_key_seq_wait{0};
std::atomic<std::int32_t> g_key_seq_active{0};
constexpr const char* kKeySeqPath = "/data/local/tmp/a9tas-inject-seq";
// ---- Phase B5: input recording / replay ----
// Records real key events (keycode, value, physics frame) and dumps them as
// a replay sequence (keycode,value,frames-delta) for a9tas-inject-seq.
struct RecEvent {
    std::uint32_t keycode;
    double value;
    std::uint32_t frame;
};
RecEvent g_rec[4096] = {};
std::atomic<std::int32_t> g_rec_count{0};
std::atomic<std::int32_t> g_rec_active{0};
std::atomic<std::int32_t> g_rec_armed{0};  // "start" seen; begin on next RACE START
constexpr const char* kRecCtlPath = "/data/local/tmp/a9tas-record-ctl";
// Output goes to the game's own files dir: the game process (untrusted_app
// SELinux domain) cannot create files under /data/local/tmp (read-only OK).
constexpr const char* kRecOutPath =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/a9tas-record-out";
// ---- Phase B6: vehicle update callback probe (race-state signal) ----
// The vehicle update callback (ctx+0x180 subscriber sub[2], 0x36c7110) is
// invoked while a vehicle simulation exists. Counting it tells us whether a
// race is running (menu vs race vs pause) — the basis for frame-accurate
// record/replay alignment.
constexpr std::uintptr_t kVehicleUpdateOffset = 0x36c7110;
// first 16 bytes: sub sp,#0xb0 / stp d11,d10 / stp d9,d8 / stp x21,x20
constexpr std::uint8_t kVehicleUpdateSignature[16] = {
    0xff, 0xc3, 0x02, 0xd1, 0xeb, 0x2b, 0x07, 0x6d,
    0xe9, 0x23, 0x08, 0x6d, 0xf5, 0x53, 0x09, 0xa9,
};
std::atomic<std::uint64_t> g_vehicle_calls{0};
std::atomic<std::uint64_t> g_vehicle_prev{0};
extern "C" std::uintptr_t g_vehicle_trampoline = 0;  // clean re-entry stub
extern "C" std::uintptr_t g_vehicle_original = 0;
// race-state tracking: the vehicle pointer changes when a race starts
// (lobby showcase car -> drivable race car). race_frame counts from 0 at the
// change; record/replay align to it for frame-accurate TAS sync.
std::atomic<std::uintptr_t> g_last_vehicle{0};
std::atomic<std::uint32_t> g_race_frame{0};
std::atomic<std::int32_t> g_race_active{0};
// vehicle-field sampling: dump the vehicle object bytes to the game files
// dir on request, for locating steer/brake/accel/nitro fields by comparing
// states (idle vs steer vs brake vs nitro).
std::atomic<std::int32_t> g_sample_request{0};
char g_sample_label[32] = {};
// device-object sampling on key events: dumps the keyboard device object on
// each real key press while enabled (for locating driving-value fields)
std::atomic<std::int32_t> g_dev_sample_on{0};
std::atomic<std::uint32_t> g_dev_sample_seq{0};
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
extern "C" void ProcessKeySequence();
extern "C" __attribute__((noinline)) void PhysicsDispatchProbeImpl(void*,
                                                                  const void*) {
    g_physics_dispatch_calls.fetch_add(1, std::memory_order_relaxed);
    ProcessKeySequence();
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

// Build a clean re-entry stub: a copy of the original 16-byte prologue
// followed by an absolute jump back to target+16. The original entry keeps
// the probe patch forever; probes call this stub to run the real handler
// without re-entering the patch. mmap + clear-cache + RX (no mprotect+W on
// translated pages; the stub is fresh so PROT_WRITE is safe before exec).
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
extern "C" __attribute__((noinline)) void FrameDispatchProbeImpl(std::uintptr_t lr,
                                                                    std::uintptr_t container) {
    if (g_frame_container.load(std::memory_order_relaxed) == 0) {
        g_frame_container.store(container, std::memory_order_relaxed);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "frame container=%p", reinterpret_cast<void*>(container));
    }
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


// Interval override control: file /data/local/tmp/a9tas-interval-override
// contains "0x<hexfloat>" (e.g. 0x3d088889 = 1/30). The reporter applies it
// to PhysicsContext+0x178 every 5s; deleting the file restores 1/60
// (0x3c888889). Direct write (the object is in writable heap memory).
constexpr const char* kIntervalOverridePath = "/data/local/tmp/a9tas-interval-override";
constexpr std::uint32_t kIntervalDefaultBits = 0x3c888889;  // 1/60

// ---- Determinism: RNG control ----
// the GOT slots with fixed implementations makes every rand()-driven
// behaviour (AI, effects, etc.) deterministic — the same inputs then yield
// the same race outcomes (mirrors the original UcrtBaseRand hook).
constexpr std::uintptr_t kRandGotOffset = 0xa506598;
constexpr std::uintptr_t kSrandGotOffset = 0xa5068c8;
std::atomic<std::uint32_t> g_rng_state{0x9e3779b9u};  // fixed seed

// Keyboard device auto-resolution: the JNI dispatch reads
// device = *(vec_begin + 0x20) where vec_begin = *(guest_base + 0xa5bc9c0)
// (NativeKeyboard.s_keyboardEventCallbacks). This removes the need for a
// manual key press to "ready" the device before replay.
std::uintptr_t g_guest_base_saved = 0;

bool SafeRead(void* out, std::uintptr_t addr, size_t len);  // defined later

std::uintptr_t ResolveKeyboardDevice() {
    if (g_guest_base_saved == 0) return 0;
    std::uintptr_t vec_begin = 0;
    if (!SafeRead(&vec_begin, g_guest_base_saved + 0xa5bc9c0, sizeof(vec_begin)) ||
        vec_begin == 0) {
        return 0;
    }
    // The JNI dispatch (0x5d4b7e4) does: ldr x0,[elem,#0x20]; ldr x8,[x0];
    // ldr x3,[x8,#0x30]; br x3 — so the device passed to the keyboard
    // handler (vtable[6] = 0x6613f68) is the value at elem+0x20.
    std::uintptr_t device = 0;
    if (!SafeRead(&device, vec_begin + 0x20, sizeof(device)) || device == 0) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "DEVICE RESOLVE elem+0x20 empty vec=0x%llx",
                            static_cast<unsigned long long>(vec_begin));
        return 0;
    }
    return device;
}

extern "C" __attribute__((noinline)) int FixedRand() {
    // deterministic LCG; returns [0, 2^31-1] like bionic RAND_MAX
    std::uint32_t s = g_rng_state.fetch_add(1664525u, std::memory_order_relaxed);
    s = s * 1103515245u + 12345u;
    return static_cast<int>(s & 0x7fffffffu);
}

extern "C" __attribute__((noinline)) void FixedSrand(unsigned int) {
    // ignore the seed: RNG stays deterministic (fixed state)
}

bool WriteProcMem8(std::uintptr_t address, const std::uint8_t* bytes) {
    const int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) return false;
    const ssize_t written = pwrite(fd, bytes, 8, static_cast<off_t>(address));
    close(fd);
    return written == 8;
}

bool InstallRngHook(std::uintptr_t guest_base) {
    const std::uintptr_t rand_got = guest_base + kRandGotOffset;
    const std::uintptr_t srand_got = guest_base + kSrandGotOffset;
    std::uintptr_t orig_rand = 0, orig_srand = 0;
    std::memcpy(&orig_rand, reinterpret_cast<const void*>(rand_got), 8);
    std::memcpy(&orig_srand, reinterpret_cast<const void*>(srand_got), 8);
    if (orig_rand == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "rng got rand slot unresolved");
        return false;
    }
    std::uint8_t patch[8]{};
    const std::uintptr_t rand_fn = reinterpret_cast<std::uintptr_t>(&FixedRand);
    std::memcpy(patch, &rand_fn, 8);
    if (!WriteProcMem8(rand_got, patch)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "rng rand slot write failed");
        return false;
    }
    std::uint8_t back[8]{};
    std::memcpy(back, reinterpret_cast<const void*>(rand_got), 8);
    if (std::memcmp(back, patch, 8) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "rng rand slot read-back mismatch; probe disabled");
        return false;
    }
    if (orig_srand != 0) {
        const std::uintptr_t srand_fn =
            reinterpret_cast<std::uintptr_t>(&FixedSrand);
        std::memcpy(patch, &srand_fn, 8);
        if (WriteProcMem8(srand_got, patch)) {
            std::memcpy(back, reinterpret_cast<const void*>(srand_got), 8);
            if (std::memcmp(back, patch, 8) == 0) {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "rng hook installed rand=%p->%p srand=%p->%p "
                                    "readback_ok",
                                    reinterpret_cast<void*>(orig_rand),
                                    reinterpret_cast<void*>(&FixedRand),
                                    reinterpret_cast<void*>(orig_srand),
                                    reinterpret_cast<void*>(&FixedSrand));
                return true;
            }
        }
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "rng hook installed rand=%p->%p readback_ok",
                        reinterpret_cast<void*>(orig_rand),
                        reinterpret_cast<void*>(&FixedRand));
    return true;
}


void ApplyIntervalOverride() {
    if (g_physics_ctx[0] == 0) return;
    FILE* file = std::fopen(kIntervalOverridePath, "re");
    std::uint32_t target = kIntervalDefaultBits;
    bool has_target = false;
    if (file != nullptr) {
        char buf[32]{};
        if (std::fgets(buf, sizeof(buf), file) != nullptr) {
            std::uint32_t parsed = 0;
            if (std::sscanf(buf, "0x%x", &parsed) == 1 ||
                std::sscanf(buf, "%x", &parsed) == 1) {
                target = parsed;
                has_target = true;
            }
        }
        std::fclose(file);
    }
    std::uint32_t current = 0;
    std::memcpy(&current, reinterpret_cast<const void*>(g_physics_ctx[0] + 0x178),
                sizeof(current));
    if (current != target) {
        std::memcpy(reinterpret_cast<void*>(g_physics_ctx[0] + 0x178), &target,
                    sizeof(target));
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "INTERVAL OVERRIDE -> 0x%08x (was 0x%08x)%s",
                            target, current, has_target ? "" : " [restore]");
    }
}

std::uintptr_t ReporterGuestBase() {
    return g_guest_game_base.load(std::memory_order_acquire);
}


// Safe memory access: verify the range is mapped before dereferencing.
// The payload runs translated by Houdini; an unmapped deref SEGVs the whole
// game process (observed as "libhoudini" tombstone frames). All diagnostics
// must go through these.
bool RangeMapped(std::uintptr_t addr, size_t len) {
    if (addr == 0) return false;
    FILE* maps = std::fopen("/proc/self/maps", "re");
    if (maps == nullptr) return false;
    char line[2048]{};
    bool found = false;
    const std::uintptr_t end = addr + len;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long start = 0, e = 0;
        char perms[5]{};
        if (std::sscanf(line, "%llx-%llx %4s", &start, &e, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        if (addr >= start && end <= e) { found = true; break; }
    }
    std::fclose(maps);
    return found;
}

bool SafeRead(void* out, std::uintptr_t addr, size_t len) {
    if (!RangeMapped(addr, len)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(addr), len);
    return true;
}

void DumpDispatchList(std::uintptr_t guest_base, const char* name,
                      std::uintptr_t list_addr);
void DumpCallbackTable(std::uintptr_t guest_base, const char* name,
                       std::uintptr_t table_off);


// Input probe impl: histogram + pending synthetic-event injection.
// The real handler runs through the clean trampoline stub.
extern "C" __attribute__((noinline)) void InputProbeImpl(void* device,
                                                         std::uint32_t event,
                                                         float value) {
    g_input_total.fetch_add(1, std::memory_order_relaxed);
    if (event < 22) {
        g_input_event_counts[event].fetch_add(1, std::memory_order_relaxed);
    }
    using Handler = void (*)(void*, std::uint32_t, double);
    const std::uintptr_t pend_event = g_input_pending[0];
    if (pend_event != 0) {
        g_input_pending[0] = 0;
        std::uintptr_t val_bits = g_input_pending[1];
        double dval = 0.0;
        std::memcpy(&dval, &val_bits, sizeof(dval));
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "INPUT INJECT device=%p event=%llu value=%.3f",
                            reinterpret_cast<void*>(device),
                            static_cast<unsigned long long>(pend_event),
                            dval);
        if (g_input_trampoline != 0 && device != 0) {
            Handler tramp = reinterpret_cast<Handler>(g_input_trampoline);
            tramp(device, static_cast<std::uint32_t>(pend_event), dval);
        }
    }
    if (g_input_trampoline != 0) {
        Handler tramp = reinterpret_cast<Handler>(g_input_trampoline);
        tramp(device, event, value);
    }
}

// Touch probe impl: histogram + pending synthetic-touch injection.
// The real dispatch runs through the clean trampoline stub.
extern "C" __attribute__((noinline)) void TouchProbeImpl(void* platform,
                                                         std::uint32_t w1,
                                                         std::uint32_t w2,
                                                         std::uintptr_t x3) {
    g_touch_total.fetch_add(1, std::memory_order_relaxed);
    g_touch_w1_counts[w1 & 0x3f].fetch_add(1, std::memory_order_relaxed);
    g_touch_w2_counts[w2 & 7].fetch_add(1, std::memory_order_relaxed);
    if (x3 != 0) g_touch_x3_nonzero.fetch_add(1, std::memory_order_relaxed);
    if (platform != nullptr) g_touch_platform_ok.fetch_add(1, std::memory_order_relaxed);
    using TouchFn = void (*)(void*, std::uint32_t, std::uint32_t, std::uintptr_t);
    const std::uintptr_t pend_w1 = g_touch_pending[0];
    if (pend_w1 != 0) {
        g_touch_pending[0] = 0;
        const std::uintptr_t pend_w2 = g_touch_pending[1];
        const std::uintptr_t pend_x3 = g_touch_pending[2];
        const std::uintptr_t plat = reinterpret_cast<std::uintptr_t>(platform);
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "TOUCH INJECT platform=%p w1=%llu w2=%llu x3=0x%llx",
                            reinterpret_cast<void*>(plat),
                            static_cast<unsigned long long>(pend_w1),
                            static_cast<unsigned long long>(pend_w2),
                            static_cast<unsigned long long>(pend_x3));
        if (g_touch_trampoline != 0 && plat != 0) {
            TouchFn tramp = reinterpret_cast<TouchFn>(g_touch_trampoline);
            tramp(reinterpret_cast<void*>(plat), static_cast<std::uint32_t>(pend_w1),
                  static_cast<std::uint32_t>(pend_w2), pend_x3);
        }
    }
    if (g_touch_trampoline != 0) {
        TouchFn tramp = reinterpret_cast<TouchFn>(g_touch_trampoline);
        tramp(platform, w1, w2, x3);
    }
}

// Touch probe: a plain function; the real dispatch runs via the clean
// trampoline stub, so no registers are clobbered across the impl call.
extern "C" __attribute__((noinline)) void TouchProbe(void* platform,
                                                     std::uint32_t w1,
                                                     std::uint32_t w2,
                                                     std::uintptr_t x3) {
    TouchProbeImpl(platform, w1, w2, x3);
}

// Vehicle update probe: counts invocations (race-state signal) and replays
// through the clean trampoline stub.
extern "C" __attribute__((noinline)) void VehicleProbeImpl(void* vehicle,
                                                           const void* frame) {
    const std::uint64_t n = g_vehicle_calls.fetch_add(1, std::memory_order_relaxed);
    // race-state: a vehicle pointer change marks the start of a new race
    // (lobby showcase car -> drivable race car). Only the first change while
    // not in a race triggers the timeline; mid-race rebuilds (wreck respawn)
    // are ignored so race_frame keeps counting.
    const std::uintptr_t vptr = reinterpret_cast<std::uintptr_t>(vehicle);
    const std::uintptr_t last = g_last_vehicle.load(std::memory_order_relaxed);
    if (last != 0 && vptr != last) {
        g_last_vehicle.store(vptr, std::memory_order_relaxed);
        // Only start the race timeline when the keyboard device is ready
        // (player has pressed a key once) — the lobby showcase car creation
        // sequence at game start must not trigger it.
        if (g_race_active.load(std::memory_order_acquire) == 0 &&
            g_key_save[0] != 0) {
            g_race_frame.store(0, std::memory_order_relaxed);
            g_race_active.store(1, std::memory_order_release);
            // arm recording on the race start if "start" was requested
            if (g_rec_armed.exchange(0, std::memory_order_acq_rel) != 0) {
                g_rec_count.store(0, std::memory_order_relaxed);
                g_rec_active.store(1, std::memory_order_release);
                __android_log_print(ANDROID_LOG_WARN, kTag,
                                    "REC AUTO-START race_frame=0");
            }
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "RACE START vehicle=%p call=%llu", vehicle,
                                static_cast<unsigned long long>(n));
        } else if (g_race_active.load(std::memory_order_acquire) == 0) {
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "VEHICLE CHANGE device-not-ready (waiting) "
                                "vehicle=%p",
                                vehicle);
        } else {
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "VEHICLE REBUILD (ignored) vehicle=%p call=%llu",
                                vehicle, static_cast<unsigned long long>(n));
        }
    } else if (last == 0) {
        g_last_vehicle.store(vptr, std::memory_order_relaxed);
    }
    // note: no auto resume — pausing naturally freezes the physics tick, so
    // sequence wait counting stops by itself; race state is reset manually
    // via the "race-end" command after leaving a race.
    g_race_frame.fetch_add(1, std::memory_order_relaxed);
    // one-shot vehicle-object sampling (triggered by reporter ctl file)
    if (g_sample_request.exchange(0, std::memory_order_acq_rel) != 0) {
        char path[160]{};
        std::snprintf(path, sizeof(path),
                      "/data/user/0/com.aligames.kuang.kybc.aligames/"
                      "files/vehicle-%s.bin",
                      g_sample_label[0] ? g_sample_label : "sample");
        FILE* f = std::fopen(path, "wb");
        if (f != nullptr) {
            std::fwrite(vehicle, 1, 0x2000, f);
            std::fclose(f);
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "VEHICLE SAMPLE label=%s vehicle=%p bytes=0x400",
                                g_sample_label, vehicle);
        } else {
            __android_log_print(ANDROID_LOG_ERROR, kTag,
                                "VEHICLE SAMPLE fopen failed label=%s", g_sample_label);
        }
    }
    if (g_vehicle_trampoline != 0) {
        using VehicleFn = void (*)(void*, const void*);
        VehicleFn tramp = reinterpret_cast<VehicleFn>(g_vehicle_trampoline);
        tramp(vehicle, frame);
    }
}

extern "C" __attribute__((noinline)) void VehicleProbe(void* vehicle,
                                                       const void* frame) {
    VehicleProbeImpl(vehicle, frame);
}

bool InstallVehicleProbe(std::uintptr_t guest_base) {
    auto* target = reinterpret_cast<std::uint8_t*>(guest_base + kVehicleUpdateOffset);
    if (std::memcmp(target, kVehicleUpdateSignature, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime vehicle update bytes mismatch; probe disabled");
        return false;
    }
    g_vehicle_trampoline = BuildTrampoline(reinterpret_cast<std::uintptr_t>(target));
    if (g_vehicle_trampoline == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "vehicle update trampoline build failed; probe disabled");
        return false;
    }
    g_vehicle_original = reinterpret_cast<std::uintptr_t>(target);
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&VehicleProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "vehicle update patch write failed; probe disabled");
        g_vehicle_trampoline = 0;
        g_vehicle_original = 0;
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    std::uint8_t back[16]{};
    std::memcpy(back, target, 16);
    if (std::memcmp(back, patch, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "vehicle update patch read-back mismatch; probe disabled");
        g_vehicle_trampoline = 0;
        g_vehicle_original = 0;
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "vehicle probe installed target=%p trampoline=%p readback_ok",
                        target, reinterpret_cast<void*>(g_vehicle_trampoline));
    return true;
}

// ---- Race-state via frame subscribers ----
// The frame dispatcher (0x38b78e4) drives the logic-tick subscriber chain
// at container+0x8 (dispatcher 0x367f0cc, vtable[8] calls). The race logic
// registers additional subscribers when a race starts — comparing the
// subscriber list between lobby and race identifies the race-start signal
// and the race-logic functions (driving-value chain).
constexpr std::uintptr_t kSubscriberDispatchOffset = 0x367f0cc;
// first 16 bytes: sub sp,#0x70 / str x24,[sp,#0x30] / stp x23,x22 / stp x21,x20
constexpr std::uint8_t kSubscriberDispatchSignature[16] = {
    0xff, 0xc3, 0x01, 0xd1, 0xf8, 0x1b, 0x00, 0xf9,
    0xf7, 0x5b, 0x04, 0xa9, 0xf5, 0x53, 0x05, 0xa9,
};
extern "C" std::uintptr_t g_subscriber_trampoline = 0;
std::atomic<std::int32_t> g_sub_count{0};
[[maybe_unused]] std::atomic<std::int32_t> g_sub_dump{0};

extern "C" __attribute__((noinline)) void SubscriberProbeImpl(void* container,
                                                              const void* frame) {
    std::int32_t count = 0;
    if (SafeRead(&count, reinterpret_cast<std::uintptr_t>(container) + 0x10,
                 sizeof(count)) &&
        count != g_sub_count.load(std::memory_order_relaxed)) {
        g_sub_count.store(count, std::memory_order_relaxed);
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "SUBSCRIBERS count=%d container=%p", count, container);
        // dump the subscriber vtable list (intrusive list: node at obj-0x10,
        // vtable at obj, next at obj-0x10)
        std::uintptr_t cur = 0;
        SafeRead(&cur, reinterpret_cast<std::uintptr_t>(container), sizeof(cur));
        for (std::int32_t i = 0; i < 16 && cur != 0; ++i) {
            const std::uintptr_t obj = cur - 0x10;
            std::uintptr_t vtable = 0;
            if (!SafeRead(&vtable, obj, sizeof(vtable))) break;
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "  SUB[%d] vtable=0x%llx obj=%p",
                                i, static_cast<unsigned long long>(vtable),
                                reinterpret_cast<void*>(obj));
            SafeRead(&cur, obj, sizeof(cur));
        }
    }
    if (g_subscriber_trampoline != 0) {
        using SubFn = void (*)(void*, const void*);
        SubFn tramp = reinterpret_cast<SubFn>(g_subscriber_trampoline);
        tramp(container, frame);
    }
}

extern "C" __attribute__((noinline)) void SubscriberProbe(void* container,
                                                          const void* frame) {
    SubscriberProbeImpl(container, frame);
}

[[maybe_unused]]
bool InstallSubscriberProbe(std::uintptr_t guest_base) {
    auto* target =
        reinterpret_cast<std::uint8_t*>(guest_base + kSubscriberDispatchOffset);
    if (std::memcmp(target, kSubscriberDispatchSignature, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime subscriber dispatch bytes mismatch; probe "
                            "disabled");
        return false;
    }
    g_subscriber_trampoline =
        BuildTrampoline(reinterpret_cast<std::uintptr_t>(target));
    if (g_subscriber_trampoline == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "subscriber dispatch trampoline build failed");
        return false;
    }
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&SubscriberProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "subscriber dispatch patch write failed");
        g_subscriber_trampoline = 0;
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    std::uint8_t back[16]{};
    std::memcpy(back, target, 16);
    if (std::memcmp(back, patch, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "subscriber dispatch read-back mismatch");
        g_subscriber_trampoline = 0;
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "subscriber probe installed target=%p trampoline=%p "
                        "readback_ok",
                        target, reinterpret_cast<void*>(g_subscriber_trampoline));
    return true;
}

// ---- Driving value chain probe ----
// Chain: input event loop 0x36c63ec -> clamp 0x366f59c -> consume 0x366d4b0
// -> vehicle control 0x4bff63c. 0x4bff63c's prologue contains a bl (pc-
// relative) so it cannot be trampolined; 0x366d4b0's prologue is bl-free
// (mov) and is the safe hook point. It fires on player input events — the
// value-level injection point (original SteeringValue/BrakeValue/
// AcceleratorValue equivalent).
constexpr std::uintptr_t kDriveConsumeOffset = 0x366d4b0;
// first 16 bytes: sub sp,#0x30 / stp x21,x20 / stp x19,x30 / mov x21,x0
constexpr std::uint8_t kDriveConsumeSignature[16] = {
    0xff, 0xc3, 0x00, 0xd1, 0xf5, 0x53, 0x01, 0xa9,
    0xf3, 0x7b, 0x02, 0xa9, 0xf5, 0x03, 0x00, 0xaa,
};
extern "C" std::uintptr_t g_drive_trampoline = 0;
std::atomic<std::uint64_t> g_drive_calls{0};
std::atomic<std::int32_t> g_drive_observe{1};
std::atomic<std::int32_t> g_drive_override{0};
float g_drive_values[3] = {0.0f, 0.0f, 0.0f};

extern "C" __attribute__((noinline)) void DriveProbeImpl(void* a, void* b,
                                                         void* values_v,
                                                         void* d) {
    g_drive_calls.fetch_add(1, std::memory_order_relaxed);
    // x2 is the 3-float driving value pointer on 0x366d4b0's path; guard
    // with RangeMapped (not every call path passes a pointer).
    float* values = nullptr;
    const std::uintptr_t vaddr = reinterpret_cast<std::uintptr_t>(values_v);
    if (vaddr != 0 && RangeMapped(vaddr, 12)) {
        values = reinterpret_cast<float*>(values_v);
    }
    if (values != nullptr && g_drive_observe.load(std::memory_order_relaxed)) {
        static std::uint32_t n = 0;
        if ((n++ % 120) == 0) {
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "DRIVE VALUES %.3f %.3f %.3f (calls=%llu)",
                                static_cast<double>(values[0]),
                                static_cast<double>(values[1]),
                                static_cast<double>(values[2]),
                                static_cast<unsigned long long>(
                                    g_drive_calls.load(std::memory_order_relaxed)));
        }
    }
    if (values != nullptr && g_drive_override.load(std::memory_order_relaxed)) {
        values[0] = g_drive_values[0];
        values[1] = g_drive_values[1];
        values[2] = g_drive_values[2];
    }
    if (g_drive_trampoline != 0) {
        using Fn = void (*)(void*, void*, void*, void*);
        Fn tramp = reinterpret_cast<Fn>(g_drive_trampoline);
        tramp(a, b, values_v, d);
    }
}

extern "C" __attribute__((noinline)) void DriveProbe(void* a, void* b,
                                                     void* values, void* d) {
    DriveProbeImpl(a, b, values, d);
}

[[maybe_unused]]
bool InstallDriveProbe(std::uintptr_t guest_base) {
    auto* target = reinterpret_cast<std::uint8_t*>(guest_base + kDriveConsumeOffset);
    if (std::memcmp(target, kDriveConsumeSignature, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime drive consume bytes mismatch; probe disabled");
        return false;
    }
    g_drive_trampoline = BuildTrampoline(reinterpret_cast<std::uintptr_t>(target));
    if (g_drive_trampoline == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "drive consume trampoline build failed");
        return false;
    }
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&DriveProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "drive consume patch write failed");
        g_drive_trampoline = 0;
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    std::uint8_t back[16]{};
    std::memcpy(back, target, 16);
    if (std::memcmp(back, patch, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "drive consume read-back mismatch");
        g_drive_trampoline = 0;
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "drive probe installed target=%p trampoline=%p readback_ok",
                        target, reinterpret_cast<void*>(g_drive_trampoline));
    return true;
}

// ---- Keyboard action dispatcher probe ----
// 0x35f79cc dispatches keyboard actions (from state reader 0x385a0e8) to a
// subscriber chain (vtable[8] calls, same shape as 0x367f0cc). Dumping the
// container reveals the subscriber vtables -> the function that writes the
// driving float values (the final value-level injection point).
constexpr std::uintptr_t kActionDispatchOffset = 0x35f79cc;
// first 16 bytes: sub sp,#0x60 / stp x23,x22 / stp x21,x20 / stp x19,x30
constexpr std::uint8_t kActionDispatchSignature[16] = {
    0xff, 0x83, 0x01, 0xd1, 0xf7, 0x5b, 0x03, 0xa9,
    0xf5, 0x53, 0x04, 0xa9, 0xf3, 0x7b, 0x05, 0xa9,
};
extern "C" std::uintptr_t g_action_trampoline = 0;
std::atomic<std::uint64_t> g_action_calls{0};

extern "C" __attribute__((noinline)) void ActionProbeImpl(void* container) {
    const std::uint64_t n =
        g_action_calls.fetch_add(1, std::memory_order_relaxed);
    if ((n % 300) == 0) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "ACTION DISPATCH calls=%llu container=%p",
                            static_cast<unsigned long long>(n), container);
    }
    // periodic dump of the container (subscriber list): container bytes +
    // one-level deref of every pointer-like slot. Refreshes every 3000 calls
    // so the last dump reflects the in-race state.
    if (n % 3000 == 0) {
        char path[160]{};
        std::snprintf(path, sizeof(path),
                      "/data/user/0/com.aligames.kuang.kybc.aligames/"
                      "files/action-sub.bin");
        FILE* f = std::fopen(path, "wb");
        if (f != nullptr) {
            // header: guest_base + container so vtable static offsets are
            // computable offline
            std::fwrite(&g_guest_base_saved, 1, 8, f);
            const std::uintptr_t cont_addr =
                reinterpret_cast<std::uintptr_t>(container);
            std::fwrite(&cont_addr, 1, 8, f);
            std::fwrite(container, 1, 0x200, f);
            const std::uintptr_t base =
                reinterpret_cast<std::uintptr_t>(container);
            for (std::uintptr_t i = 0; i < 0x200; i += 8) {
                std::uintptr_t v = 0;
                if (!RangeMapped(base + i, 8)) continue;
                std::memcpy(&v, reinterpret_cast<const void*>(base + i), 8);
                if (v < 0x7fff00000000ull || v > 0x7fff80000000ull) continue;
                if (!RangeMapped(v, 0x40)) continue;
                std::uint8_t obj[0x40]{};
                std::memcpy(obj, reinterpret_cast<const void*>(v), 0x40);
                std::fwrite(&v, 1, 8, f);
                std::fwrite(obj, 1, 0x40, f);
                // third level: if obj[0] looks like a vtable (code segment
                // of the game), dump 0x80 bytes of it
                std::uintptr_t vt = 0;
                std::memcpy(&vt, obj, 8);
                if (vt > g_guest_base_saved &&
                    vt < g_guest_base_saved + 0x4000000ull &&
                    RangeMapped(vt, 0x80)) {
                    std::uint8_t vtbl[0x80]{};
                    std::memcpy(vtbl, reinterpret_cast<const void*>(vt), 0x80);
                    std::fwrite(&vt, 1, 8, f);
                    std::fwrite(vtbl, 1, 0x80, f);
                }
            }
            std::fclose(f);
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "ACTION SUB DUMPED container=%p calls=%llu "
                                "base=%p",
                                container,
                                static_cast<unsigned long long>(n),
                                reinterpret_cast<void*>(g_guest_base_saved));
        }
    }
    if (g_action_trampoline != 0) {
        using Fn = void (*)(void*);
        Fn tramp = reinterpret_cast<Fn>(g_action_trampoline);
        tramp(container);
    }
}

extern "C" __attribute__((noinline)) void ActionProbe(void* container) {
    ActionProbeImpl(container);
}

bool InstallActionProbe(std::uintptr_t guest_base) {
    auto* target =
        reinterpret_cast<std::uint8_t*>(guest_base + kActionDispatchOffset);
    if (std::memcmp(target, kActionDispatchSignature, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime action dispatch bytes mismatch; probe "
                            "disabled");
        return false;
    }
    g_action_trampoline = BuildTrampoline(reinterpret_cast<std::uintptr_t>(target));
    if (g_action_trampoline == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "action dispatch trampoline build failed");
        return false;
    }
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&ActionProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "action dispatch patch write failed");
        g_action_trampoline = 0;
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    std::uint8_t back[16]{};
    std::memcpy(back, target, 16);
    if (std::memcmp(back, patch, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "action dispatch read-back mismatch");
        g_action_trampoline = 0;
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "action probe installed target=%p trampoline=%p "
                        "readback_ok",
                        target, reinterpret_cast<void*>(g_action_trampoline));
    return true;
}

// Keyboard probe impl: keycode histogram + pending synthetic-key injection.
// Runs on the input thread; injection re-enters the patched handler with a
// synthetic (keycode,value); the re-entry sees pending==0 and only replays.
extern "C" __attribute__((noinline)) void KeyboardProbeImpl(void* device,
                                                            std::uint32_t keycode,
                                                            double value) {
    g_key_total.fetch_add(1, std::memory_order_relaxed);
    g_key_counts[keycode & 0xff].fetch_add(1, std::memory_order_relaxed);
    // remember the last real device pointer for frame-driven sequence
    // injection (the sequence runs on the physics tick, not on input events)
    g_key_save[0] = reinterpret_cast<std::uintptr_t>(device);
    // calibration: dump the vec element +0x20 values vs the real device to
    // find which element holds the keyboard device (one-time diagnostic)
    if (g_guest_base_saved != 0) {
        std::uintptr_t vec_begin = 0, vec_end = 0;
        if (SafeRead(&vec_begin, g_guest_base_saved + 0xa5bc9c0, 8) &&
            SafeRead(&vec_end, g_guest_base_saved + 0xa5bc9c8, 8) &&
            vec_begin != 0 && vec_end > vec_begin) {
            const std::size_t count = (vec_end - vec_begin) / 0x50;
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "DEV CALIB real_device=%p vec_begin=%p count=%zu",
                                device, reinterpret_cast<void*>(vec_begin), count);
            for (std::size_t i = 0; i < count && i < 8; ++i) {
                std::uintptr_t slot = 0;
                if (SafeRead(&slot, vec_begin + i * 0x50 + 0x20, 8)) {
                    __android_log_print(ANDROID_LOG_WARN, kTag,
                                        "  DEV CALIB elem[%zu]+0x20=%p %s", i,
                                        reinterpret_cast<void*>(slot),
                                        slot == reinterpret_cast<std::uintptr_t>(device)
                                            ? " <== MATCH"
                                            : "");
                }
            }
        }
    }
    // record real key events for replay (keycode, value, race frame)
    if (g_rec_active.load(std::memory_order_acquire) != 0) {
        const std::int32_t count = g_rec_count.load(std::memory_order_relaxed);
        if (count < 4096) {
            const std::uint32_t frame = g_race_frame.load(std::memory_order_relaxed);
            g_rec[count].keycode = keycode;
            g_rec[count].value = value;
            g_rec[count].frame = frame;
            g_rec_count.store(count + 1, std::memory_order_relaxed);
        }
    }
    // device-object sampling on key events (driving-value field hunt)
    if (g_dev_sample_on.load(std::memory_order_acquire) != 0) {
        const std::uint32_t seq =
            g_dev_sample_seq.fetch_add(1, std::memory_order_relaxed);
        char path[160]{};
        std::snprintf(path, sizeof(path),
                      "/data/user/0/com.aligames.kuang.kybc.aligames/"
                      "files/device-%u-%u.bin",
                      keycode, seq);
        FILE* f = std::fopen(path, "wb");
        if (f != nullptr) {
            std::fwrite(device, 1, 0x1000, f);
            std::fclose(f);
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "DEV SAMPLE keycode=%u seq=%u device=%p", keycode,
                                seq, device);
        }
    }
    // diagnostic: log the first real events with their value bits to learn
    // the real DOWN/UP encoding
    const std::uint64_t n = g_key_total.load(std::memory_order_relaxed);
    if (n <= 40) {
        std::uint64_t vb = 0;
        std::memcpy(&vb, &value, sizeof(vb));
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "KEY EVENT n=%llu keycode=%u value_bits=0x%llx",
                            static_cast<unsigned long long>(n), keycode,
                            static_cast<unsigned long long>(vb));
    }
    using Handler = void (*)(void*, std::uint32_t, double);
    // apply a pending synthetic injection first, then replay the real event
    const std::uintptr_t pend_key = g_key_pending[0];
    if (pend_key != 0) {
        g_key_pending[0] = 0;
        std::uintptr_t val_bits = g_key_pending[1];
        double dval = 0.0;
        std::memcpy(&dval, &val_bits, sizeof(dval));
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "KEY INJECT device=%p keycode=%llu value=%.3f",
                            reinterpret_cast<void*>(device),
                            static_cast<unsigned long long>(pend_key),
                            dval);
        if (g_key_trampoline != 0 && device != 0) {
            Handler tramp = reinterpret_cast<Handler>(g_key_trampoline);
            tramp(device, static_cast<std::uint32_t>(pend_key), dval);
        }
        // diagnostic: dump the keyboard state table after injection
        std::uintptr_t tbl = 0;
        if (SafeRead(&tbl, reinterpret_cast<std::uintptr_t>(device) + 0x130,
                     sizeof(tbl)) &&
            tbl != 0) {
            for (int i = 0; i < 6; ++i) {
                std::uint32_t type = 0, pressed = 0;
                SafeRead(&type, tbl + static_cast<std::uintptr_t>(i) * 0x18 + 0x10,
                         sizeof(type));
                SafeRead(&pressed, tbl + static_cast<std::uintptr_t>(i) * 0x18 + 0x14,
                         sizeof(pressed));
                __android_log_print(ANDROID_LOG_WARN, kTag,
                                    "KEY TBL[%d] type=%u pressed=%u", i, type, pressed);
            }
        } else {
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "KEY TBL unreadable device=%p", device);
        }
    }
    if (g_key_trampoline != 0) {
        Handler tramp = reinterpret_cast<Handler>(g_key_trampoline);
        tramp(device, keycode, value);
    }
}

// Keyboard probe: a plain function (the compiler emits and manages the full
// prologue/epilogue, so no registers are clobbered across the impl call).
// The real handler runs through the clean trampoline stub.
extern "C" __attribute__((noinline)) void KeyboardProbe(void* device,
                                                        std::uint32_t keycode,
                                                        double value) {
    KeyboardProbeImpl(device, keycode, value);
}

// Sequence file parser: reads /data/local/tmp/a9tas-inject-seq and loads
// steps into a local buffer. Returns true if the content fingerprint changed.
// Caller must hold g_seq_mutex to publish results.
static bool ParseKeySequenceFile(KeySeqStep* out_steps, std::size_t* out_count,
                                  char* out_fingerprint, std::size_t fp_size) {
    FILE* seq = std::fopen(kKeySeqPath, "re");
    if (seq == nullptr) return false;
    char line[64]{};
    std::size_t flen = 0;
    std::size_t n = 0;
    while (n < 256 && std::fgets(line, sizeof(line), seq) != nullptr) {
        unsigned long long k = 0;
        double v = 0.0;
        long long f = 0;
        if (std::sscanf(line, "%llu,%lf,%lld", &k, &v, &f) == 3) {
            out_steps[n].keycode = static_cast<std::uint32_t>(k);
            out_steps[n].value = v;
            out_steps[n].wait_frames = static_cast<std::int32_t>(f);
            ++n;
        }
        // Clamp snprintf return to avoid overflow on truncation
        if (flen < fp_size) {
            const int consumed = std::snprintf(out_fingerprint + flen,
                                               fp_size - flen, "%s", line);
            if (consumed > 0) {
                flen += static_cast<std::size_t>(consumed);
                if (flen > fp_size - 1) flen = fp_size - 1;
            }
        }
    }
    std::fclose(seq);
    remove(kKeySeqPath);
    *out_count = n;
    return n > 0;
}

// Mutex protecting sequence loading and publication.
static pthread_mutex_t g_seq_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_seq_fingerprint[1024] = {};

static bool TryLoadKeySequence() {
    KeySeqStep local_steps[256];
    std::size_t local_count = 0;
    char local_fp[1024] = {};
    if (!ParseKeySequenceFile(local_steps, &local_count,
                               local_fp, sizeof(local_fp))) {
        return false;
    }
    pthread_mutex_lock(&g_seq_mutex);
    if (std::strcmp(local_fp, g_seq_fingerprint) == 0) {
        pthread_mutex_unlock(&g_seq_mutex);
        return false;
    }
    std::strncpy(g_seq_fingerprint, local_fp, sizeof(g_seq_fingerprint) - 1);
    g_seq_fingerprint[sizeof(g_seq_fingerprint) - 1] = '\0';
    if (local_count > 0) {
        std::memcpy(g_key_seq, local_steps, local_count * sizeof(KeySeqStep));
        g_key_seq_len.store(static_cast<std::int32_t>(local_count),
                            std::memory_order_release);
        g_key_seq_pos.store(0, std::memory_order_relaxed);
        g_key_seq_wait.store(g_key_seq[0].wait_frames,
                             std::memory_order_relaxed);
        g_key_seq_active.store(1, std::memory_order_release);
    }
    pthread_mutex_unlock(&g_seq_mutex);
    __android_log_print(ANDROID_LOG_WARN, kTag,
                        "KEY SEQ LOADED steps=%zu", local_count);
    return true;
}

// Frame-driven control polling (replaces the reporter thread, which was
// observed to stall on this guest): race-start/race-end commands and
// sequence loading, checked once per second from the physics tick.
extern "C" void CheckControlFiles() {
    static char race_last[16] = {};
    FILE* rc = std::fopen("/data/local/tmp/a9tas-record-ctl", "re");
    if (rc != nullptr) {
        char buf[16]{};
        if (std::fgets(buf, sizeof(buf), rc) != nullptr &&
            std::strcmp(buf, race_last) != 0) {
            std::strncpy(race_last, buf, sizeof(race_last) - 1);
            if (std::strncmp(buf, "race-start", 10) == 0) {
                if (g_race_active.load(std::memory_order_acquire) == 0) {
                    g_race_frame.store(0, std::memory_order_relaxed);
                    g_race_active.store(1, std::memory_order_release);
                    __android_log_print(ANDROID_LOG_WARN, kTag,
                                        "RACE START (manual)");
                }
            } else if (std::strncmp(buf, "race-end", 8) == 0) {
                g_race_active.store(0, std::memory_order_release);
                g_race_frame.store(0, std::memory_order_relaxed);
                __android_log_print(ANDROID_LOG_WARN, kTag, "RACE END (manual)");
            }
        }
        std::fclose(rc);
        remove("/data/local/tmp/a9tas-record-ctl");
    }
    TryLoadKeySequence();
}

// Frame-driven sequence execution: called once per physics dispatch tick
// (60 Hz). Drains g_key_seq without any real input events — this is the TAS
// input axis. Runs on the game's physics thread (same thread as the input
// handler), so calling the keyboard trampoline here is thread-safe.
extern "C" __attribute__((noinline)) void ProcessKeySequence() {
    // poll control files once per second (frame-driven; the reporter thread
    // was observed to stall on this guest)
    static std::uint32_t ctl_tick = 0;
    if ((ctl_tick++ % 60) == 0) {
        CheckControlFiles();
    }
    if (g_key_seq_active.load(std::memory_order_acquire) == 0) return;
    const std::int32_t len = g_key_seq_len.load(std::memory_order_acquire);
    std::int32_t pos = g_key_seq_pos.load(std::memory_order_relaxed);
    if (pos >= len) {
        g_key_seq_active.store(0, std::memory_order_release);
        // determinism check: sample the vehicle right when the sequence ends
        // (race frame aligned across runs) for cross-run comparison
        const std::uintptr_t vptr = g_last_vehicle.load(std::memory_order_relaxed);
        static std::uint32_t done_seq = 0;
        const std::uint32_t seq_id = ++done_seq;
        char path[160]{};
        std::snprintf(path, sizeof(path),
                      "/data/user/0/com.aligames.kuang.kybc.aligames/"
                      "files/seqdone-%u.bin",
                      seq_id);
        FILE* f = std::fopen(path, "wb");
        if (f != nullptr && vptr != 0 && RangeMapped(vptr, 0x2000)) {
            std::fwrite(reinterpret_cast<const void*>(vptr), 1, 0x2000, f);
            std::fclose(f);
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "KEY SEQ DONE steps=%d sampled=%u vehicle=%p",
                                len, seq_id, reinterpret_cast<void*>(vptr));
        } else {
            if (f != nullptr) std::fclose(f);
            __android_log_print(ANDROID_LOG_WARN, kTag,
                                "KEY SEQ DONE steps=%d (sample failed)", len);
        }
        return;
    }
    // only the device captured from a real key event is reliable — the
    // auto-resolved value crashed the game when used for injection
    std::uintptr_t dev = g_key_save[0];
    static std::uint32_t seq_diag = 0;
    if ((seq_diag++ % 120) == 0) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "SEQ DIAG dev=%p tramp=%p race=%d wait=%d pos=%d/%d",
                            reinterpret_cast<void*>(dev),
                            reinterpret_cast<void*>(g_key_trampoline),
                            g_race_active.load(std::memory_order_relaxed),
                            g_key_seq_wait.load(std::memory_order_relaxed),
                            pos, len);
    }
    if (g_key_trampoline == 0 || dev == 0) {
        // No device yet (no real key event seen since load): hold the
        // sequence without consuming steps, so it starts once the player
        // presses any key once.
        return;
    }
    // frame-accurate sync: only advance the timeline while a race is running
    // (race frame counts from 0 at the vehicle pointer change)
    if (g_race_active.load(std::memory_order_acquire) == 0) {
        return;
    }
    std::int32_t wait = g_key_seq_wait.load(std::memory_order_relaxed);
    if (wait > 0) {
        g_key_seq_wait.store(wait - 1, std::memory_order_relaxed);
        return;
    }
    const KeySeqStep& step = g_key_seq[pos];
    if (g_key_trampoline != 0 && dev != 0) {
        using Handler = void (*)(void*, std::uint32_t, double);
        Handler tramp = reinterpret_cast<Handler>(g_key_trampoline);
        tramp(reinterpret_cast<void*>(dev), step.keycode, step.value);
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "KEY SEQ step=%d/%d keycode=%u value=%.2f wait=%d "
                            "race_frame=%u",
                            pos, len, step.keycode, step.value, step.wait_frames,
                            g_race_frame.load(std::memory_order_relaxed));
    } else {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "KEY SEQ no device=%p trampoline=%p", 
                            reinterpret_cast<void*>(dev),
                            reinterpret_cast<void*>(g_key_trampoline));
    }
    ++pos;
    g_key_seq_pos.store(pos, std::memory_order_relaxed);
    if (pos < len) {
        g_key_seq_wait.store(g_key_seq[pos].wait_frames, std::memory_order_relaxed);
    }
}

// Input probe: a plain function; the real handler runs via the clean
// trampoline stub, so no registers are clobbered across the impl call.
extern "C" __attribute__((noinline)) void InputProbe(void* device,
                                                     std::uint32_t event,
                                                     double value) {
    InputProbeImpl(device, event, value);
}

bool InstallInputProbe(std::uintptr_t guest_base) {
    auto* target = reinterpret_cast<std::uint8_t*>(guest_base + kHidHandlerOffset);
    if (std::memcmp(target, kHidHandlerSignature, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime hid handler bytes mismatch; probe disabled");
        return false;
    }
    g_input_trampoline = BuildTrampoline(reinterpret_cast<std::uintptr_t>(target));
    if (g_input_trampoline == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "hid handler trampoline build failed; probe disabled");
        return false;
    }
    g_input_original = reinterpret_cast<std::uintptr_t>(target);
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&InputProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "hid handler patch write failed; probe disabled");
        g_input_trampoline = 0;
        g_input_original = 0;
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    std::uint8_t back[16]{};
    std::memcpy(back, target, 16);
    if (std::memcmp(back, patch, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "hid handler patch read-back mismatch; probe disabled");
        g_input_trampoline = 0;
        g_input_original = 0;
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "input probe installed target=%p trampoline=%p readback_ok",
                        target, reinterpret_cast<void*>(g_input_trampoline));
    return true;
}

bool InstallTouchProbe(std::uintptr_t guest_base) {
    auto* target = reinterpret_cast<std::uint8_t*>(guest_base + kTouchDispatchOffset);
    if (std::memcmp(target, kTouchDispatchSignature, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime touch dispatch bytes mismatch; probe disabled");
        return false;
    }
    g_touch_trampoline = BuildTrampoline(reinterpret_cast<std::uintptr_t>(target));
    if (g_touch_trampoline == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "touch dispatch trampoline build failed; probe disabled");
        return false;
    }
    g_touch_original = reinterpret_cast<std::uintptr_t>(target);
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&TouchProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "touch dispatch patch write failed; probe disabled");
        g_touch_trampoline = 0;
        g_touch_original = 0;
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    std::uint8_t back[16]{};
    std::memcpy(back, target, 16);
    if (std::memcmp(back, patch, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "touch dispatch patch read-back mismatch; probe disabled");
        g_touch_trampoline = 0;
        g_touch_original = 0;
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "touch probe installed target=%p trampoline=%p readback_ok",
                        target, reinterpret_cast<void*>(g_touch_trampoline));
    return true;
}

bool InstallKeyboardProbe(std::uintptr_t guest_base) {
    auto* target = reinterpret_cast<std::uint8_t*>(guest_base + kKeyboardHandlerOffset);
    if (std::memcmp(target, kKeyboardHandlerSignature, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime keyboard handler bytes mismatch; probe disabled");
        return false;
    }
    g_key_trampoline = BuildTrampoline(reinterpret_cast<std::uintptr_t>(target));
    if (g_key_trampoline == 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "keyboard handler trampoline build failed; probe disabled");
        return false;
    }
    g_key_original = reinterpret_cast<std::uintptr_t>(target);
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&KeyboardProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "keyboard handler patch write failed; probe disabled");
        g_key_trampoline = 0;
        g_key_original = 0;
        return false;
    }
    __builtin___clear_cache(reinterpret_cast<char*>(target),
                            reinterpret_cast<char*>(target + 16));
    std::uint8_t back[16]{};
    std::memcpy(back, target, 16);
    if (std::memcmp(back, patch, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "keyboard handler patch read-back mismatch; probe disabled");
        g_key_trampoline = 0;
        g_key_original = 0;
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "keyboard probe installed target=%p trampoline=%p readback_ok",
                        target, reinterpret_cast<void*>(g_key_trampoline));
    return true;
}

void* ProbeReporter(void*) {
    std::uint64_t previous = 0;
    std::uint64_t frame_previous[6] = {};
    // 600 samples x 5s = 50 minutes of coverage for the three-state test;
    // the payload is gated by the enable marker, so this only runs when the
    // probe is actually installed.
    for (int sample = 1; sample <= 600; ++sample) {
        usleep(5000000);  // 5s (usleep instead of sleep: guest sleep() was
                          // observed to stall the reporter thread)
        const std::uint64_t current =
            g_physics_dispatch_calls.load(std::memory_order_relaxed);
        // heartbeat (every cycle) — if this stops, the reporter is stuck
        // between cycles; if the HB line continues but later handling stops,
        // the reporter is stuck later in the loop.
        __android_log_print(ANDROID_LOG_INFO, kTag, "HB sample=%d", sample);
        const std::uint64_t delta5s = current - previous;
        previous = current;
        static std::uint64_t phys_sum30 = 0;
        phys_sum30 += delta5s;
        if (sample % 6 == 0) {
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
                                "phys sample=%d delta30s=%llu | frame total=%llu "
                                "c0=%llu c1=%llu c2=%llu c3=%llu c4=%llu other=%llu",
                                sample,
                                static_cast<unsigned long long>(phys_sum30),
                                static_cast<unsigned long long>(frame_total),
                                static_cast<unsigned long long>(frame_deltas[0]),
                                static_cast<unsigned long long>(frame_deltas[1]),
                                static_cast<unsigned long long>(frame_deltas[2]),
                                static_cast<unsigned long long>(frame_deltas[3]),
                                static_cast<unsigned long long>(frame_deltas[4]),
                                static_cast<unsigned long long>(frame_deltas[5]));
            phys_sum30 = 0;
        }
        // loading-resume race-start detection: physics delta drops (<200)
        // during a scene load then recovers (>=250); the recovery edge marks
        // the new scene. The vehicle object is a persistent singleton in this
        // build, so pointer changes cannot be used as the race-start signal.
        static std::int32_t loading = 0;
        if (delta5s > 0 && delta5s < 200) {
            loading = 1;
        } else if (delta5s >= 250 && loading != 0) {
            loading = 0;
            if (g_race_active.load(std::memory_order_acquire) == 0 &&
                g_key_save[0] != 0) {
                g_race_frame.store(0, std::memory_order_relaxed);
                g_race_active.store(1, std::memory_order_release);
                if (g_rec_armed.exchange(0, std::memory_order_acq_rel) != 0) {
                    g_rec_count.store(0, std::memory_order_relaxed);
                    g_rec_active.store(1, std::memory_order_release);
                    __android_log_print(ANDROID_LOG_WARN, kTag,
                                        "REC AUTO-START race_frame=0");
                }
                __android_log_print(ANDROID_LOG_WARN, kTag,
                                    "RACE START (loading-resume) frame=%u",
                                    static_cast<unsigned>(current));
            }
        }
        // vehicle update probe report (race-state signal)
        const std::uint64_t vcur = g_vehicle_calls.load(std::memory_order_relaxed);
        const std::uint64_t vprev = g_vehicle_prev.load(std::memory_order_relaxed);
        g_vehicle_prev.store(vcur, std::memory_order_relaxed);
        __android_log_print(ANDROID_LOG_INFO, kTag, "vehicle total=%llu delta5s=%llu",
                            static_cast<unsigned long long>(vcur),
                            static_cast<unsigned long long>(vcur - vprev));
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
        if (g_frame_container_dumped == 0) {
            const std::uintptr_t fc = g_frame_container.load(std::memory_order_relaxed);
            if (fc != 0) {
                g_frame_container_dumped = 1;
                DumpDispatchList(ReporterGuestBase(), "framecontainer+0x180", fc + 0x180);
            }
        }
        // input probe report (every 5s): total + per-event deltas
        static std::uint64_t input_previous[22] = {};
        std::uint64_t input_total = 0;
        for (size_t i = 0; i < 22; ++i) {
            const std::uint64_t c =
                g_input_event_counts[i].load(std::memory_order_relaxed);
            input_total += c;
            if (c != input_previous[i]) {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "  input event[%zu] total=%llu delta5s=%llu",
                                    i, static_cast<unsigned long long>(c),
                                    static_cast<unsigned long long>(c - input_previous[i]));
                input_previous[i] = c;
            }
        }
        __android_log_print(ANDROID_LOG_INFO, kTag, "input total=%llu",
                            static_cast<unsigned long long>(input_total));
        // injection control: /data/local/tmp/a9tas-inject-input "event,value"
        static char input_last_cmd[64] = {};
        FILE* inj = std::fopen(kInputInjectPath, "re");
        if (inj != nullptr) {
            char buf[48]{};
            if (std::fgets(buf, sizeof(buf), inj) != nullptr) {
                unsigned long long ev = 0;
                double v = 0.0;
                if (std::sscanf(buf, "%llu,%lf", &ev, &v) == 2 && ev < 22 &&
                    std::strcmp(buf, input_last_cmd) != 0) {
                    std::strncpy(input_last_cmd, buf, sizeof(input_last_cmd) - 1);
                    g_input_pending[0] = static_cast<std::uintptr_t>(ev);
                    std::uintptr_t bits = 0;
                    std::memcpy(&bits, &v, sizeof(bits));
                    g_input_pending[1] = bits;
                    __android_log_print(ANDROID_LOG_WARN, kTag,
                                        "INPUT INJECT REQUESTED event=%llu value=%.3f "
                                        "(applied on next input event)",
                                        ev, v);
                }
            }
            std::fclose(inj);
            // one-shot: remove the file so repeated ticks do not re-inject
            remove(kInputInjectPath);
        }
        // touch probe report + injection control:
        // /data/local/tmp/a9tas-inject-touch "w1,w2,x3"
        static std::uint64_t touch_previous[64] = {};
        std::uint64_t touch_total = 0;
        for (size_t i = 0; i < 64; ++i) {
            const std::uint64_t c = g_touch_w1_counts[i].load(std::memory_order_relaxed);
            touch_total += c;
            if (c != touch_previous[i]) {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "  touch w1[%zu] total=%llu", i,
                                    static_cast<unsigned long long>(c));
                touch_previous[i] = c;
            }
        }
        if (touch_total != 0) {
            std::uint64_t w2_hist[8] = {};
            for (size_t i = 0; i < 8; ++i) {
                w2_hist[i] = g_touch_w2_counts[i].load(std::memory_order_relaxed);
            }
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "touch total=%llu x3_nonzero=%llu platform_ok=%llu "
                                "w2=[%llu %llu %llu %llu %llu %llu %llu %llu]",
                                static_cast<unsigned long long>(touch_total),
                                static_cast<unsigned long long>(
                                    g_touch_x3_nonzero.load(std::memory_order_relaxed)),
                                static_cast<unsigned long long>(
                                    g_touch_platform_ok.load(std::memory_order_relaxed)),
                                static_cast<unsigned long long>(w2_hist[0]),
                                static_cast<unsigned long long>(w2_hist[1]),
                                static_cast<unsigned long long>(w2_hist[2]),
                                static_cast<unsigned long long>(w2_hist[3]),
                                static_cast<unsigned long long>(w2_hist[4]),
                                static_cast<unsigned long long>(w2_hist[5]),
                                static_cast<unsigned long long>(w2_hist[6]),
                                static_cast<unsigned long long>(w2_hist[7]));
        }
        FILE* tinj = std::fopen(kTouchInjectPath, "re");
        if (tinj != nullptr) {
            char buf[64]{};
            if (std::fgets(buf, sizeof(buf), tinj) != nullptr) {
                unsigned long long w1 = 0, w2 = 0, x3 = 0;
                if (std::sscanf(buf, "%llu,%llu,%llx", &w1, &w2, &x3) >= 2) {
                    g_touch_pending[0] = static_cast<std::uintptr_t>(w1);
                    g_touch_pending[1] = static_cast<std::uintptr_t>(w2);
                    g_touch_pending[2] = static_cast<std::uintptr_t>(x3);
                    __android_log_print(ANDROID_LOG_WARN, kTag,
                                        "TOUCH INJECT REQUESTED w1=%llu w2=%llu "
                                        "x3=0x%llx (applied on next touch event)",
                                        w1, w2, x3);
                }
            }
            std::fclose(tinj);
            remove(kTouchInjectPath);
        }
        // keyboard probe report + injection control:
        // /data/local/tmp/a9tas-inject-key "keycode,value"
        static std::uint64_t key_previous[256] = {};
        static char key_last_cmd[64] = {};
        std::uint64_t key_total = 0;
        for (size_t i = 0; i < 256; ++i) {
            const std::uint64_t c = g_key_counts[i].load(std::memory_order_relaxed);
            key_total += c;
            if (c != key_previous[i]) {
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "  key[0x%02zx] total=%llu", i,
                                    static_cast<unsigned long long>(c));
                key_previous[i] = c;
            }
        }
        __android_log_print(ANDROID_LOG_INFO, kTag, "key total=%llu",
                            static_cast<unsigned long long>(key_total));
        // device/race/seq status feedback (every 5s) — write to the game
        // files dir so the operator can cat it at any time
        std::uintptr_t dev_now = g_key_save[0];
        if (dev_now == 0) dev_now = ResolveKeyboardDevice();
        const std::int32_t race_now = g_race_active.load(std::memory_order_acquire);
        const std::uint32_t frame_now = g_race_frame.load(std::memory_order_relaxed);
        const std::int32_t seq_pos = g_key_seq_pos.load(std::memory_order_relaxed);
        const std::int32_t seq_len = g_key_seq_len.load(std::memory_order_acquire);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "STATUS device=%p race=%d frame=%u seq=%d/%d",
                            reinterpret_cast<void*>(dev_now), race_now, frame_now,
                            seq_pos, seq_len);
        FILE* st = std::fopen(
            "/data/user/0/com.aligames.kuang.kybc.aligames/files/a9tas-status",
            "w");
        if (st != nullptr) {
            std::fprintf(st, "device=%p race=%d frame=%u seq=%d/%d\n",
                         reinterpret_cast<void*>(dev_now), race_now, frame_now,
                         seq_pos, seq_len);
            std::fclose(st);
        }
        FILE* kinj = std::fopen(kKeyInjectPath, "re");
        if (kinj != nullptr) {
            char buf[48]{};
            if (std::fgets(buf, sizeof(buf), kinj) != nullptr) {
                unsigned long long key = 0;
                double v = 0.0;
                if (std::sscanf(buf, "%llu,%lf", &key, &v) == 2 &&
                    std::strcmp(buf, key_last_cmd) != 0) {
                    std::strncpy(key_last_cmd, buf, sizeof(key_last_cmd) - 1);
                    g_key_pending[0] = static_cast<std::uintptr_t>(key);
                    std::uintptr_t bits = 0;
                    std::memcpy(&bits, &v, sizeof(bits));
                    g_key_pending[1] = bits;
                    __android_log_print(ANDROID_LOG_WARN, kTag,
                                        "KEY INJECT REQUESTED keycode=%llu value=%.3f "
                                        "(applied on next key event)",
                                        key, v);
                }
            }
            std::fclose(kinj);
            remove(kKeyInjectPath);
        }
        // Sequence loading is handled exclusively by the physics-frame
        // thread (CheckControlFiles) to avoid concurrent modification.
        ApplyIntervalOverride();
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
        // recording control: /data/local/tmp/a9tas-record-ctl
        //   "start" -> begin recording (clears buffer)
        //   "stop"  -> stop recording
        //   "dump"  -> write /data/local/tmp/a9tas-record-out as a replay
        //              sequence (keycode,value,frames-delta)
        static char rec_last_cmd[16] = {};
        FILE* rc = std::fopen(kRecCtlPath, "re");
        if (rc != nullptr) {
            char buf[16]{};
            if (std::fgets(buf, sizeof(buf), rc) != nullptr) {
                // all commands dedup on content; since SELinux blocks the
                // game's remove(), repeat writes with varying content (e.g.
                // race-end-1, race-end-2) to re-trigger. The stale file with
                // the same content is then ignored.
                if (std::strcmp(buf, rec_last_cmd) != 0) {
                    std::strncpy(rec_last_cmd, buf, sizeof(rec_last_cmd) - 1);
                    if (std::strncmp(buf, "race-start", 10) == 0) {
                        // manual race start (this build loads at full physics
                        // rate, so automatic load detection is unreliable)
                        std::uintptr_t dev = g_key_save[0];
                        if (dev == 0) dev = ResolveKeyboardDevice();
                        if (g_race_active.load(std::memory_order_acquire) == 0 &&
                            dev != 0) {
                            g_race_frame.store(0, std::memory_order_relaxed);
                            g_race_active.store(1, std::memory_order_release);
                            if (g_rec_armed.exchange(0,
                                                     std::memory_order_acq_rel) != 0) {
                                g_rec_count.store(0, std::memory_order_relaxed);
                                g_rec_active.store(1, std::memory_order_release);
                                __android_log_print(ANDROID_LOG_WARN, kTag,
                                                    "REC AUTO-START race_frame=0");
                            }
                            __android_log_print(ANDROID_LOG_WARN, kTag,
                                                "RACE START (manual)");
                        }
                    } else if (std::strncmp(buf, "race-end", 8) == 0) {
                        g_race_active.store(0, std::memory_order_release);
                        g_race_frame.store(0, std::memory_order_relaxed);
                        __android_log_print(ANDROID_LOG_WARN, kTag,
                                            "RACE END (manual) %s", buf);
                    } else if (std::strncmp(buf, "start", 5) == 0) {
                    // arm: recording begins on the next RACE START (vehicle
                    // pointer change) so the timeline starts at race frame 0
                    g_rec_count.store(0, std::memory_order_relaxed);
                    g_rec_active.store(0, std::memory_order_release);
                    g_rec_armed.store(1, std::memory_order_release);
                    g_race_active.store(0, std::memory_order_release);
                    g_race_frame.store(0, std::memory_order_relaxed);
                    __android_log_print(ANDROID_LOG_WARN, kTag,
                                        "REC ARMED (starts on next RACE START)");
                } else if (std::strncmp(buf, "stop", 4) == 0) {
                    g_rec_active.store(0, std::memory_order_release);
                    g_rec_armed.store(0, std::memory_order_release);
                    __android_log_print(ANDROID_LOG_WARN, kTag, "REC STOP events=%d",
                                        g_rec_count.load(std::memory_order_relaxed));
                } else if (std::strncmp(buf, "dump", 4) == 0) {
                    const std::int32_t n = g_rec_count.load(std::memory_order_relaxed);
                    FILE* out = std::fopen(kRecOutPath, "w");
                    if (out != nullptr) {
                        for (std::int32_t i = 0; i < n; ++i) {
                            const std::uint32_t delta =
                                i == 0 ? g_rec[i].frame
                                       : g_rec[i].frame - g_rec[i - 1].frame;
                            std::fprintf(out, "%u,%.1f,%u\n", g_rec[i].keycode,
                                         g_rec[i].value, delta);
                        }
                        std::fclose(out);
                        __android_log_print(ANDROID_LOG_WARN, kTag,
                                            "REC DUMP events=%d -> %s", n,
                                            kRecOutPath);
                    } else {
                        __android_log_print(ANDROID_LOG_ERROR, kTag,
                                            "REC DUMP fopen failed");
                    }
                }
                }
            }
            std::fclose(rc);
            remove(kRecCtlPath);
        }
        // vehicle-field sampling control: /data/local/tmp/a9tas-sample-ctl
        // content = label; dumps vehicle object bytes on next vehicle tick
        static char sample_last_cmd[32] = {};
        FILE* sc = std::fopen("/data/local/tmp/a9tas-sample-ctl", "re");
        if (sc != nullptr) {
            char buf[32]{};
            if (std::fgets(buf, sizeof(buf), sc) != nullptr &&
                std::strcmp(buf, sample_last_cmd) != 0) {
                std::strncpy(sample_last_cmd, buf, sizeof(sample_last_cmd) - 1);
                char* nl = std::strchr(buf, '\n');
                if (nl != nullptr) *nl = '\0';
                std::strncpy(g_sample_label, buf, sizeof(g_sample_label) - 1);
                g_sample_request.store(1, std::memory_order_release);
                __android_log_print(ANDROID_LOG_WARN, kTag,
                                    "SAMPLE REQUEST label=%s", buf);
            }
            std::fclose(sc);
            remove("/data/local/tmp/a9tas-sample-ctl");
        }
        // device sampling control: /data/local/tmp/a9tas-dev-sample-ctl
        // "on"/"off" — dumps keyboard device object on each key event
        static char dev_last_cmd[8] = {};
        FILE* dc = std::fopen("/data/local/tmp/a9tas-dev-sample-ctl", "re");
        if (dc != nullptr) {
            char buf[8]{};
            if (std::fgets(buf, sizeof(buf), dc) != nullptr &&
                std::strcmp(buf, dev_last_cmd) != 0) {
                std::strncpy(dev_last_cmd, buf, sizeof(dev_last_cmd) - 1);
                if (std::strncmp(buf, "on", 2) == 0) {
                    g_dev_sample_on.store(1, std::memory_order_release);
                    __android_log_print(ANDROID_LOG_WARN, kTag,
                                        "DEV SAMPLE ON");
                } else {
                    g_dev_sample_on.store(0, std::memory_order_release);
                    __android_log_print(ANDROID_LOG_WARN, kTag,
                                        "DEV SAMPLE OFF");
                }
            }
            std::fclose(dc);
            remove("/data/local/tmp/a9tas-dev-sample-ctl");
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


// Structure diagnostics: frame container identity, PhysicsContext sub-object
// vtables, and the dispatch subscriber callback list.



void DumpDispatchList(std::uintptr_t guest_base, const char* name,
                      std::uintptr_t list_addr) {
    std::uintptr_t begin = 0, end = 0;
    if (!SafeRead(&begin, list_addr, sizeof(begin))) return;
    if (!SafeRead(&end, list_addr + 8, sizeof(end))) return;
    if (begin == 0 || end <= begin || (end - begin) % 16 != 0 ||
        (end - begin) / 16 > 64) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "  %s list odd begin=%p end=%p", name,
                            reinterpret_cast<void*>(begin), reinterpret_cast<void*>(end));
        return;
    }
    const size_t count = (end - begin) / 16;
    __android_log_print(ANDROID_LOG_INFO, kTag, "  %s subscribers count=%zu",
                        name, count);
    for (size_t i = 0; i < count; ++i) {
        std::uintptr_t raw = 0, vtable = 0, cb = 0;
        if (!SafeRead(&raw, begin + 8 + i * 16, sizeof(raw))) continue;
        if (!SafeRead(&vtable, raw, sizeof(vtable))) continue;
        if (!SafeRead(&cb, vtable + 0x10, sizeof(cb))) continue;
        // deep pointers used by the vehicle-update callback (0x36c7110):
        // obj28=[elem+0x28], step=[obj28+0xb0]; obj250=[elem+0x250], v2=[[obj250]]+0x60
        std::uintptr_t obj28 = 0, step = 0, obj250 = 0, v2 = 0;
        if (!SafeRead(&obj28, raw + 0x28, sizeof(obj28))) continue;
        if (!SafeRead(&step, obj28 + 0xb0, sizeof(step))) continue;
        if (!SafeRead(&obj250, raw + 0x250, sizeof(obj250))) continue;
        if (!SafeRead(&v2, obj250 + 0x60, sizeof(v2))) continue;
        // step points to heap (std::function-like): its first dword is the
        // actual target code pointer; log it if it lands in the game lib.
        std::uintptr_t step_target = 0, step_target2 = 0;
        if (!SafeRead(&step_target, step, sizeof(step_target))) step_target = 0;
        if (step_target != 0 && step_target > guest_base &&
            step_target - guest_base < 0x8000000ull) {
            // maybe a thunk: read the next level only if the first is outside
            // the game lib
            step_target2 = 0;
        } else if (step_target != 0) {
            if (!SafeRead(&step_target2, step_target, sizeof(step_target2)))
                step_target2 = 0;
        }
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "    %s sub[%zu] cb=0x%llx(off 0x%llx) obj28=0x%llx "
                            "step=0x%llx step_tgt=0x%llx(off 0x%llx) tgt2=0x%llx "
                            "obj250=0x%llx v2=0x%llx",
                            name, i, static_cast<unsigned long long>(cb),
                            static_cast<unsigned long long>(cb - guest_base),
                            static_cast<unsigned long long>(obj28),
                            static_cast<unsigned long long>(step),
                            static_cast<unsigned long long>(step_target),
                            static_cast<unsigned long long>(step_target - guest_base),
                            static_cast<unsigned long long>(step_target2),
                            static_cast<unsigned long long>(obj250),
                            static_cast<unsigned long long>(v2));
    }
}

void LogPhysicsStructure(std::uintptr_t guest_base) {
    const std::uintptr_t ctx = g_physics_ctx[0];
    if (ctx == 0) return;
    auto log_ptr = [guest_base](const char* what, std::uintptr_t addr) {
        std::uintptr_t v = 0;
        if (!SafeRead(&v, addr, sizeof(v))) return;
        __android_log_print(ANDROID_LOG_INFO, kTag, "  %s@%p = 0x%llx (off 0x%llx)",
                            what, reinterpret_cast<void*>(addr),
                            static_cast<unsigned long long>(v),
                            static_cast<unsigned long long>(v - guest_base));
    };
    log_ptr("subobj+0x58", ctx + 0x58);
    log_ptr("subobj+0xa8", ctx + 0xa8);
    log_ptr("subobj+0xd0", ctx + 0xd0);
    log_ptr("subobj+0x128", ctx + 0x128);
    log_ptr("dispatch+0x28", ctx + 0x180 + 0x28);
    log_ptr("dispatch+0x30", ctx + 0x180 + 0x30);
    DumpDispatchList(guest_base, "ctx+0x180", ctx + 0x180);
    const std::uintptr_t fc = g_frame_container.load(std::memory_order_relaxed);
    if (fc != 0) {
        DumpDispatchList(guest_base, "framecontainer+0x180", fc + 0x180);
    }
    DumpCallbackTable(guest_base, "HidController.s_pHidEventCallbacks", 0xa5bc968);
    DumpCallbackTable(guest_base, "NativeKeyboard.s_keyboardEventCallbacks", 0xa5bc9c0);
    // touch dispatch global: fn ptr at 0xa5d4980 (0xa5d4000 + 0x980)
    std::uintptr_t touch_fn = 0;
    if (SafeRead(&touch_fn, guest_base + 0xa5d4980, sizeof(touch_fn))) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "  touch dispatch fn = 0x%llx (off 0x%llx)",
                            static_cast<unsigned long long>(touch_fn),
                            static_cast<unsigned long long>(touch_fn - guest_base));
        // std::function-style: the first dword is the target code pointer
        std::uintptr_t touch_target = 0;
        if (SafeRead(&touch_target, touch_fn, sizeof(touch_target))) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "  touch fn target = 0x%llx (off 0x%llx)",
                                static_cast<unsigned long long>(touch_target),
                                static_cast<unsigned long long>(touch_target - guest_base));
        }
    } else {
        __android_log_print(ANDROID_LOG_WARN, kTag, "  touch dispatch fn unreadable");
    }
    // HID callback vector data objects: possible device states (check +0x140)
    std::uintptr_t hid_begin = 0;
    if (SafeRead(&hid_begin, guest_base + 0xa5bc968, sizeof(hid_begin)) &&
        hid_begin != 0 && hid_begin > guest_base &&
        hid_begin - guest_base > 0x1000000ull) {
        // hid_begin is a heap vector base; elements at +0x18 (3rd slot) etc.
        for (size_t i = 0; i < 3; ++i) {
            std::uintptr_t elem = 0;
            if (!SafeRead(&elem, hid_begin + 8 + i * 16, sizeof(elem))) continue;
            std::uint32_t slot140 = 0;
            if (SafeRead(&slot140, elem + 0x140, sizeof(slot140))) {
                float f = 0.0f;
                std::memcpy(&f, &slot140, sizeof(f));
                __android_log_print(ANDROID_LOG_INFO, kTag,
                                    "  hidvec elem[%zu]=%p +0x140=%.4f (0x%08x)",
                                    i, reinterpret_cast<void*>(elem),
                                    static_cast<double>(f), slot140);
            }
        }
    }
}

// std::vector<fn*> layout: [0]=begin, [8]=end, [16]=capacity
void DumpCallbackTable(std::uintptr_t guest_base, const char* name,
                       std::uintptr_t table_off) {
    const std::uintptr_t table = guest_base + table_off;
    std::uintptr_t begin = 0, end = 0;
    if (!SafeRead(&begin, table, sizeof(begin))) return;
    if (!SafeRead(&end, table + 8, sizeof(end))) return;
    __android_log_print(ANDROID_LOG_INFO, kTag, "  %s vec={%p,%p}",
                        name, reinterpret_cast<void*>(begin),
                        reinterpret_cast<void*>(end));
    if (begin == 0 || end < begin || (end - begin) % 8 != 0 ||
        (end - begin) / 8 > 16) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "    odd range; skipping");
        return;
    }
    const size_t count = (end - begin) / 8;
    for (size_t i = 0; i < count; ++i) {
        std::uintptr_t fn = 0;
        if (!SafeRead(&fn, begin + i * 8, sizeof(fn))) continue;
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "    %s[%zu] = 0x%llx (off 0x%llx)",
                            name, i, static_cast<unsigned long long>(fn),
                            static_cast<unsigned long long>(fn - guest_base));
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
        g_guest_base_saved = guest_base;
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
        LogPhysicsStructure(guest_base);
        const bool phys_ok = InstallPhysicsDispatchProbe(guest_base);
        const bool frame_ok = InstallFrameDispatchProbe(guest_base);
        const bool input_ok = InstallInputProbe(guest_base);
        const bool touch_ok = InstallTouchProbe(guest_base);
        const bool key_ok = InstallKeyboardProbe(guest_base);
        const bool vehicle_ok = InstallVehicleProbe(guest_base);
        const bool rng_ok = InstallRngHook(guest_base);
        // DriveProbe (0x366d4b0) is disabled: CONTROL_VALUES_RESEARCH.md
        // proved it has 0 calls during keyboard driving. Keeping the install
        // call would patch a function that is never invoked on the keyboard
        // path, adding risk for no benefit.
        // const bool drive_ok = InstallDriveProbe(guest_base);
        const bool action_ok = InstallActionProbe(guest_base);
        // subscriber probe disabled: its per-change logging flooded logcat
        // and tanked the game framerate (5Hz); the conclusion (subscribers
        // are resident, not race-registered) is already recorded.
        if (phys_ok || frame_ok || input_ok || touch_ok || key_ok || vehicle_ok ||
            rng_ok || action_ok) {
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
