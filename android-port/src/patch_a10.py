# Patch payload_phase_a10.cpp: Phase B HID input probe
src = open('payload_phase_a10.cpp', encoding='utf-8').read()

# 1. Add input probe globals + constants
src = src.replace(
"""std::uintptr_t g_frame_container_dumped = 0;""",
"""std::uintptr_t g_frame_container_dumped = 0;
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
constexpr const char* kInputInjectPath = "/data/local/tmp/a9tas-inject-input";""")

# 2. Input probe impl + probe + installer
probe_code = r'''
// Input probe impl: histogram + pending synthetic-event injection.
extern "C" __attribute__((noinline)) void InputProbeImpl(void* device,
                                                         std::uint32_t event,
                                                         float value) {
    g_input_total.fetch_add(1, std::memory_order_relaxed);
    if (event < 22) {
        g_input_event_counts[event].fetch_add(1, std::memory_order_relaxed);
    }
    // apply a pending synthetic injection on this (input) thread
    const std::uintptr_t pend_event = g_input_pending[0];
    if (pend_event != 0) {
        g_input_pending[0] = 0;
        std::uintptr_t val_bits = g_input_pending[1];
        std::uintptr_t dev = g_input_pending_device;
        using Handler = void (*)(void*, std::uint32_t, double);
        Handler orig = reinterpret_cast<Handler>(g_input_original);
        double dval = 0.0;
        std::memcpy(&dval, &val_bits, sizeof(dval));
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "INPUT INJECT device=%p event=%llu value=%.3f",
                            reinterpret_cast<void*>(dev),
                            static_cast<unsigned long long>(pend_event),
                            dval);
        if (orig != nullptr && dev != 0) {
            orig(reinterpret_cast<void*>(dev), static_cast<std::uint32_t>(pend_event), dval);
        }
    }
}

// Design G probe for the HID handler 0x6613d8c. The copied prologue bytes
// 0-11 are sub sp / str x30 / mov x8,x0; the 3rd and 4th instructions are
// re-run after the count (x8=device and x0=sub-object). device/event/value
// (x0/w1/d0) are parked in g_input_save across the count call.
extern "C" __attribute__((noinline)) void InputProbe(void* device,
                                                     std::uint32_t event,
                                                     double value) {
    register void* dev __asm__("x0") = device;
    register std::uint32_t ev __asm__("w1") = event;
    register double val __asm__("d0") = value;
    __asm__ volatile(
        // copied prologue bytes 0-11
        "sub sp, sp, #0x20\n"
        "str x30, [sp, #0x10]\n"
        "mov x8, x0\n"
        // park device/event/value
        "adrp x16, :got:g_input_save\n"
        "ldr x16, [x16, #:got_lo12:g_input_save]\n"
        "ldr x16, [x16]\n"
        "str x0, [x16]\n"
        "str w1, [x16, #8]\n"
        "str d0, [x16, #16]\n"
        "bl InputProbeImpl\n"
        "ldr x0, [x16]\n"
        "ldr w1, [x16, #8]\n"
        "ldr d0, [x16, #16]\n"
        // re-run 3rd and 4th prologue instructions
        "mov x8, x0\n"
        "ldr x0, [x0, #0x110]\n"
        // absolute jump to target+16
        "adrp x17, :got:g_trampoline_target_input\n"
        "ldr x17, [x17, #:got_lo12:g_trampoline_target_input]\n"
        "ldr x17, [x17]\n"
        "br x17\n"
        :: "r"(dev), "r"(ev), "r"(val)
        : "x8", "x16", "x17", "cc", "memory");
}

extern "C" std::uintptr_t g_trampoline_target_input = 0;

bool InstallInputProbe(std::uintptr_t guest_base) {
    auto* target = reinterpret_cast<std::uint8_t*>(guest_base + kHidHandlerOffset);
    if (std::memcmp(target, kHidHandlerSignature, 16) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "runtime hid handler bytes mismatch; probe disabled");
        return false;
    }
    g_trampoline_target_input = reinterpret_cast<std::uintptr_t>(target + 16);
    g_input_original = reinterpret_cast<std::uintptr_t>(target);
    std::uint8_t patch[16]{};
    WriteAbsoluteJump(patch, reinterpret_cast<std::uintptr_t>(&InputProbe));
    if (!WriteProcMem(reinterpret_cast<std::uintptr_t>(target), patch)) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "hid handler patch write failed; probe disabled");
        g_trampoline_target_input = 0;
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
        g_trampoline_target_input = 0;
        g_input_original = 0;
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "input probe installed target=%p target16=%p readback_ok",
                        target, reinterpret_cast<void*>(g_trampoline_target_input));
    return true;
}

'''
src = src.replace("void* ProbeReporter(void*) {", probe_code + "void* ProbeReporter(void*) {")

# 3. Reporter: input histogram + injection control file
old_rpt = """                DumpDispatchList(ReporterGuestBase(), "framecontainer+0x180", fc + 0x180);
            }
        }
        ApplyIntervalOverride();"""
new_rpt = """                DumpDispatchList(ReporterGuestBase(), "framecontainer+0x180", fc + 0x180);
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
        FILE* inj = std::fopen(kInputInjectPath, "re");
        if (inj != nullptr) {
            char buf[48]{};
            if (std::fgets(buf, sizeof(buf), inj) != nullptr) {
                unsigned long long ev = 0;
                double v = 0.0;
                if (std::sscanf(buf, "%llu,%lf", &ev, &v) == 2 && ev < 22) {
                    std::uintptr_t dev = g_input_save[0];
                    g_input_pending[0] = static_cast<std::uintptr_t>(ev);
                    std::uintptr_t bits = 0;
                    std::memcpy(&bits, &v, sizeof(bits));
                    g_input_pending[1] = bits;
                    g_input_pending_device = dev;
                    __android_log_print(ANDROID_LOG_WARN, kTag,
                                        "INPUT INJECT REQUESTED event=%llu value=%.3f "
                                        "device=%p (applied on next input event)",
                                        ev, v, reinterpret_cast<void*>(dev));
                }
            }
            std::fclose(inj);
            // one-shot: remove the file so repeated ticks do not re-inject
            remove(kInputInjectPath);
        }
        ApplyIntervalOverride();"""
assert old_rpt in src
src = src.replace(old_rpt, new_rpt)

# 4. Install the input probe in the worker
src = src.replace(
"""        const bool phys_ok = InstallPhysicsDispatchProbe(guest_base);
        const bool frame_ok = InstallFrameDispatchProbe(guest_base);
        if (phys_ok || frame_ok) {""",
"""        const bool phys_ok = InstallPhysicsDispatchProbe(guest_base);
        const bool frame_ok = InstallFrameDispatchProbe(guest_base);
        const bool input_ok = InstallInputProbe(guest_base);
        if (phys_ok || frame_ok || input_ok) {""")

open('payload_phase_a10.cpp', 'w', encoding='utf-8').write(src)
print("a10 ready")
