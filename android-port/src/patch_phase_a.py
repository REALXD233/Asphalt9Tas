# Patch payload_phase_a.cpp: add the frame-dispatch caller-histogram probe
src = open('payload_phase_a.cpp', encoding='utf-8').read()

# 1. Add second target global + frame dispatch constants after g_trampoline_target
src = src.replace(
"""// target+16 of the dispatch function; consumed by the probe asm via GOT.
extern "C" std::uintptr_t g_trampoline_target = 0;""",
"""// target+16 of the dispatch function; consumed by the probe asm via GOT.
extern "C" std::uintptr_t g_trampoline_target = 0;
// second probe target (frame dispatcher 0x38b78e4) - separate global.
extern "C" std::uintptr_t g_trampoline_target_frame = 0;
constexpr std::uintptr_t kFrameDispatchOffset = 0x38b78e4;
// first 16 bytes of 0x38b78e4: sub sp,sp,#0x30 / str x20,[sp,#0x10] /
// stp x19,x30,[sp,#0x20] / ldr x8,[x1]  (relocatable, no PC-relative)
constexpr std::uint8_t kFrameDispatchSignature[16] = {
    0xd1, 0x00, 0xc3, 0xff, 0xf9, 0x00, 0x0b, 0xf4,
    0xa9, 0x02, 0x7b, 0xf3, 0xf9, 0x40, 0x00, 0x28,
};
// return addresses of the 5 known bl callers (caller+4)
constexpr std::uintptr_t kFrameCallerLrs[5] = {
    0x3794c78, 0x3846518, 0x38469d0, 0x3a5ce2c, 0x3a5d7f4,
};
std::atomic<std::uint64_t> g_frame_caller_calls[6] = {};  // 5 known + other""")

# 2. Add the frame probe impl + probe before ProbeReporter
frame_probe = r'''
// Frame-dispatcher probe impl: histogram by caller return address.
extern "C" __attribute__((noinline)) void FrameDispatchProbeImpl(std::uintptr_t lr) {
    for (size_t i = 0; i < 5; ++i) {
        if (lr == kFrameCallerLrs[i]) {
            g_frame_caller_calls[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_frame_caller_calls[5].fetch_add(1, std::memory_order_relaxed);
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

'''
src = src.replace("void* ProbeReporter(void*) {", frame_probe + "void* ProbeReporter(void*) {")

# 3. Extend the reporter to log per-caller deltas
old_reporter = """void* ProbeReporter(void*) {
    std::uint64_t previous = 0;
    // 600 samples x 5s = 50 minutes of coverage for the three-state test;
    // the payload is gated by the enable marker, so this only runs when the
    // probe is actually installed.
    for (int sample = 1; sample <= 600; ++sample) {
        sleep(5);
        const std::uint64_t current =
            g_physics_dispatch_calls.load(std::memory_order_relaxed);
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "physics dispatch sample=%d total=%llu delta_5s=%llu",
                            sample, static_cast<unsigned long long>(current),
                            static_cast<unsigned long long>(current - previous));
        previous = current;
    }
    return nullptr;
}"""
new_reporter = """void* ProbeReporter(void*) {
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
        previous = current;
    }
    return nullptr;
}"""
assert old_reporter in src
src = src.replace(old_reporter, new_reporter)

# 4. Install both probes in the worker (after the marker gate)
old_install = """        if (InstallPhysicsDispatchProbe(guest_base)) {
            pthread_t reporter{};
            if (pthread_create(&reporter, nullptr, ProbeReporter, nullptr) == 0) {
                pthread_detach(reporter);
            }
        }"""
new_install = """        const bool phys_ok = InstallPhysicsDispatchProbe(guest_base);
        const bool frame_ok = InstallFrameDispatchProbe(guest_base);
        if (phys_ok || frame_ok) {
            pthread_t reporter{};
            if (pthread_create(&reporter, nullptr, ProbeReporter, nullptr) == 0) {
                pthread_detach(reporter);
            }
        }"""
assert old_install in src
src = src.replace(old_install, new_install)

open('payload_phase_a.cpp', 'w', encoding='utf-8').write(src)
print("payload_phase_a.cpp patched OK")
