// ARM64 GetPhysicsInterval wrapper core.
//
// Production builds are passive: there is no installer and arm() returns -100.
// The wrapper implements the original AluTasV2 order only:
//   call original -> optionally overwrite *X8 -> capture final *X8.

#include "physics_interval_getter_payload_v2.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

using namespace a9tas::physics_interval_payload_v2;

extern "C" std::uintptr_t g_physics_interval_getter_continue_v2;

namespace {

alignas(64) Control g_control{};
alignas(64) Evidence g_evidence{};
alignas(64) std::uint32_t g_interval_bits[kCapacity]{};
alignas(64) Event g_events[kCapacity]{};
// Published storage for a host-built shadow vtable. The active vptr is
// &g_shadow_vtable[4], leaving four exact negative entries available.
alignas(64) std::uintptr_t g_shadow_vtable[16]{};

bool ValidBits(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value) && value >= 0.001f && value <= 0.1f;
}

std::uint64_t MonotonicNs() {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

void Initialize() {
    std::memset(&g_control, 0, sizeof(g_control));
    std::memset(&g_evidence, 0, sizeof(g_evidence));
    std::memset(g_interval_bits, 0, sizeof(g_interval_bits));
    std::memset(g_events, 0, sizeof(g_events));
    std::memset(g_shadow_vtable, 0, sizeof(g_shadow_vtable));
    std::memcpy(g_control.magic, kControlMagic, sizeof(kControlMagic));
    g_control.version = kVersion;
    g_control.size = sizeof(g_control);
    std::memcpy(g_evidence.magic, kEvidenceMagic, sizeof(kEvidenceMagic));
    g_evidence.version = kVersion;
    g_evidence.size = sizeof(g_evidence);
    g_evidence.status = kStatusPassive;
}

bool ControlValid() {
    return std::memcmp(g_control.magic, kControlMagic,
                       sizeof(kControlMagic)) == 0 &&
           g_control.version == kVersion && g_control.size == sizeof(Control) &&
           g_control.limit > 0 && g_control.limit <= kCapacity &&
           (g_control.mode == static_cast<std::uint32_t>(Mode::kRecord) ||
            g_control.mode == static_cast<std::uint32_t>(Mode::kReplay)) &&
           g_control.original_getter != 0 &&
           g_control.original_getter ==
               __atomic_load_n(&g_physics_interval_getter_continue_v2,
                               __ATOMIC_ACQUIRE);
}

}  // namespace

extern "C" std::uintptr_t g_physics_interval_getter_continue_v2 = 0;

extern "C" __attribute__((noinline, visibility("hidden"))) void
PhysicsIntervalGetterAfterOriginalV2(void* object, std::uint32_t* output) {
    if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0 ||
        output == nullptr)
        return;
    __atomic_fetch_add(&g_control.active_calls, 1u, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&g_control.enabled, __ATOMIC_ACQUIRE) == 0 ||
        !ControlValid()) {
        __atomic_fetch_add(&g_evidence.semantic_errors, 1ULL,
                           __ATOMIC_RELAXED);
        __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_evidence.status, kStatusFault, __ATOMIC_RELEASE);
        __atomic_fetch_sub(&g_control.active_calls, 1u, __ATOMIC_RELEASE);
        return;
    }

    const std::uint32_t sequence = __atomic_fetch_add(
        &g_control.cursor, 1u, __ATOMIC_ACQ_REL);
    if (sequence >= g_control.limit || sequence >= kCapacity) {
        __atomic_fetch_add(&g_evidence.semantic_errors, 1ULL,
                           __ATOMIC_RELAXED);
        __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_control.completed, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_evidence.status, kStatusFault, __ATOMIC_RELEASE);
        __atomic_fetch_sub(&g_control.active_calls, 1u, __ATOMIC_RELEASE);
        return;
    }

    const std::uint32_t original_bits = __atomic_load_n(output, __ATOMIC_RELAXED);
    std::uint32_t final_bits = original_bits;
    std::uint32_t requested_bits = 0;
    std::uint32_t flags = kOriginalCalled;
    const bool object_matches =
        g_control.expected_object == 0 ||
        g_control.expected_object == reinterpret_cast<std::uintptr_t>(object);
    const bool vptr_matches =
        object != nullptr &&
        (g_control.expected_vptr == 0 ||
         g_control.expected_vptr ==
             __atomic_load_n(reinterpret_cast<std::uintptr_t*>(object),
                             __ATOMIC_RELAXED));
    if (object_matches && vptr_matches) {
        flags |= kObjectMatched;
    } else {
        __atomic_fetch_add(&g_evidence.object_mismatches, 1ULL,
                           __ATOMIC_RELAXED);
    }
    if (ValidBits(original_bits)) flags |= kOriginalValid;

    const Mode mode = static_cast<Mode>(g_control.mode);
    if (mode == Mode::kReplay) {
        flags |= kOverrideRequested;
        requested_bits = __atomic_load_n(&g_interval_bits[sequence],
                                         __ATOMIC_ACQUIRE);
        if ((flags & (kOriginalValid | kObjectMatched)) ==
                (kOriginalValid | kObjectMatched) &&
            ValidBits(requested_bits)) {
            __atomic_store_n(output, requested_bits, __ATOMIC_RELEASE);
            final_bits = requested_bits;
            flags |= kOverrideApplied;
            __atomic_fetch_add(&g_evidence.overrides, 1ULL,
                               __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_add(&g_evidence.semantic_errors, 1ULL,
                               __ATOMIC_RELAXED);
            __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
            __atomic_store_n(&g_evidence.status, kStatusFault,
                             __ATOMIC_RELEASE);
        }
        __atomic_fetch_add(&g_evidence.replay_calls, 1ULL, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&g_interval_bits[sequence], original_bits,
                         __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_evidence.record_calls, 1ULL, __ATOMIC_RELAXED);
    }
    final_bits = __atomic_load_n(output, __ATOMIC_ACQUIRE);
    if (ValidBits(final_bits)) flags |= kFinalCaptured;
    else {
        __atomic_fetch_add(&g_evidence.semantic_errors, 1ULL,
                           __ATOMIC_RELAXED);
        __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_evidence.status, kStatusFault, __ATOMIC_RELEASE);
    }

    const std::uint32_t tid = static_cast<std::uint32_t>(syscall(SYS_gettid));
    const std::uint64_t previous_tid = __atomic_load_n(
        &g_evidence.last_tid, __ATOMIC_RELAXED);
    if (sequence == 0)
        __atomic_store_n(&g_evidence.first_tid, tid, __ATOMIC_RELAXED);
    else if (previous_tid != 0 && previous_tid != tid)
        __atomic_fetch_add(&g_evidence.tid_changes, 1ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&g_evidence.last_tid, tid, __ATOMIC_RELAXED);

    Event* event = &g_events[sequence];
    event->sequence = sequence;
    event->object = reinterpret_cast<std::uintptr_t>(object);
    event->output = reinterpret_cast<std::uintptr_t>(output);
    event->monotonic_ns = MonotonicNs();
    event->tid = tid;
    event->original_bits = original_bits;
    event->requested_bits = requested_bits;
    event->final_bits = final_bits;
    event->flags = flags;
    __atomic_store_n(&event->commit_sequence, sequence + 1,
                     __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_evidence.calls, 1ULL, __ATOMIC_RELAXED);

    if (sequence + 1 >= g_control.limit) {
        __atomic_store_n(&g_control.enabled, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_control.completed, 1u, __ATOMIC_RELEASE);
        if (__atomic_load_n(&g_evidence.status, __ATOMIC_ACQUIRE) !=
            kStatusFault)
            __atomic_store_n(&g_evidence.status, kStatusComplete,
                             __ATOMIC_RELEASE);
    }
    __atomic_fetch_sub(&g_control.active_calls, 1u, __ATOMIC_RELEASE);
}

// Calls the original getter with the untouched entry X0/X8 ABI.  After it
// returns, all volatile integer/vector registers, NZCV, FPCR and FPSR produced
// by the original are saved across the C handler and restored before returning
// to the original caller.  Only the four bytes at the original X8 output
// pointer may be changed by the handler.
extern "C" __attribute__((naked, visibility("hidden"))) void
PhysicsIntervalGetterEntryV2() {
    __asm__ volatile(
        "sub sp, sp, #0x160\n"
        "stp x19, x20, [sp, #0x00]\n"
        "str x30, [sp, #0x10]\n"
        "mov x19, x0\n"
        "mov x20, x8\n"
        "adrp x17, :got:g_physics_interval_getter_continue_v2\n"
        "ldr x17, [x17, #:got_lo12:g_physics_interval_getter_continue_v2]\n"
        "ldr x17, [x17]\n"
        "blr x17\n"
        "stp x0, x1, [sp, #0x20]\n"
        "stp x2, x3, [sp, #0x30]\n"
        "stp x4, x5, [sp, #0x40]\n"
        "stp x6, x7, [sp, #0x50]\n"
        "stp x8, x9, [sp, #0x60]\n"
        "stp x10, x11, [sp, #0x70]\n"
        "stp x12, x13, [sp, #0x80]\n"
        "stp x14, x15, [sp, #0x90]\n"
        "stp x16, x17, [sp, #0xA0]\n"
        "str x18, [sp, #0xB0]\n"
        "stp q0, q1, [sp, #0xC0]\n"
        "stp q2, q3, [sp, #0xE0]\n"
        "stp q4, q5, [sp, #0x100]\n"
        "stp q6, q7, [sp, #0x120]\n"
        "mrs x9, nzcv\n"
        "mrs x10, fpcr\n"
        "stp x9, x10, [sp, #0x140]\n"
        "mrs x9, fpsr\n"
        "str x9, [sp, #0x150]\n"
        "mov x0, x19\n"
        "mov x1, x20\n"
        "bl PhysicsIntervalGetterAfterOriginalV2\n"
        "ldr x9, [sp, #0x150]\n"
        "msr fpsr, x9\n"
        "ldp x9, x10, [sp, #0x140]\n"
        "msr nzcv, x9\n"
        "msr fpcr, x10\n"
        "ldp q6, q7, [sp, #0x120]\n"
        "ldp q4, q5, [sp, #0x100]\n"
        "ldp q2, q3, [sp, #0xE0]\n"
        "ldp q0, q1, [sp, #0xC0]\n"
        "ldr x18, [sp, #0xB0]\n"
        "ldp x16, x17, [sp, #0xA0]\n"
        "ldp x14, x15, [sp, #0x90]\n"
        "ldp x12, x13, [sp, #0x80]\n"
        "ldp x10, x11, [sp, #0x70]\n"
        "ldp x8, x9, [sp, #0x60]\n"
        "ldp x6, x7, [sp, #0x50]\n"
        "ldp x4, x5, [sp, #0x40]\n"
        "ldp x2, x3, [sp, #0x30]\n"
        "ldp x0, x1, [sp, #0x20]\n"
        "ldr x30, [sp, #0x10]\n"
        "ldp x19, x20, [sp, #0x00]\n"
        "add sp, sp, #0x160\n"
        "ret\n");
}

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_physics_interval_getter_protocol_v2() {
    return kVersion;
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_physics_interval_getter_arm_v2(void*, void*) {
    return -100;
}

extern "C" __attribute__((visibility("default"))) Control*
a9tas_physics_interval_getter_control_v2() {
    return &g_control;
}

extern "C" __attribute__((visibility("default"))) Evidence*
a9tas_physics_interval_getter_evidence_v2() {
    return &g_evidence;
}

extern "C" __attribute__((visibility("default"))) std::uint32_t*
a9tas_physics_interval_getter_intervals_v2() {
    return g_interval_bits;
}

extern "C" __attribute__((visibility("default"))) Event*
a9tas_physics_interval_getter_events_v2() {
    return g_events;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t*
a9tas_physics_interval_getter_shadow_vtable_v2() {
    return g_shadow_vtable;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t*
a9tas_physics_interval_getter_continue_storage_v2() {
    return &g_physics_interval_getter_continue_v2;
}

// Fixed-width locator exports for a read-only ELF resolver. Each symbol is
// payload-owned data containing an address; resolving them never calls guest
// code.
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_physics_interval_getter_wrapper_data_v2 =
        reinterpret_cast<std::uintptr_t>(&PhysicsIntervalGetterEntryV2);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_physics_interval_getter_control_data_v2 =
        reinterpret_cast<std::uintptr_t>(&g_control);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_physics_interval_getter_evidence_data_v2 =
        reinterpret_cast<std::uintptr_t>(&g_evidence);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_physics_interval_getter_intervals_data_v2 =
        reinterpret_cast<std::uintptr_t>(g_interval_bits);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_physics_interval_getter_events_data_v2 =
        reinterpret_cast<std::uintptr_t>(g_events);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_physics_interval_getter_shadow_data_v2 =
        reinterpret_cast<std::uintptr_t>(g_shadow_vtable);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_physics_interval_getter_continue_data_v2 =
        reinterpret_cast<std::uintptr_t>(
            &g_physics_interval_getter_continue_v2);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_physics_interval_getter_control_size_v2 = sizeof(Control);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_physics_interval_getter_evidence_size_v2 = sizeof(Evidence);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_physics_interval_getter_event_size_v2 = sizeof(Event);

#if defined(A9TAS_PHYSICS_INTERVAL_GETTER_PAYLOAD_SELFTEST)
#include <cstdio>

extern "C" __attribute__((naked)) void FakePhysicsIntervalGetterV2() {
    __asm__ volatile(
        "mov w9, #0x8889\n"
        "movk w9, #0x3c08, lsl #16\n"
        "str w9, [x8]\n"
        "mov x0, x8\n"
        "ret\n");
}

extern "C" __attribute__((naked)) void InvokePhysicsIntervalGetterV2(
    void*, std::uint32_t*) {
    __asm__ volatile(
        "stp x29, x30, [sp, #-16]!\n"
        "mov x29, sp\n"
        "mov x8, x1\n"
        "bl PhysicsIntervalGetterEntryV2\n"
        "ldp x29, x30, [sp], #16\n"
        "ret\n");
}

namespace {
bool ArmSelfTest(Mode mode, std::uint32_t target_bits,
                 std::uintptr_t object, std::uintptr_t vptr) {
    Initialize();
    g_physics_interval_getter_continue_v2 =
        reinterpret_cast<std::uintptr_t>(&FakePhysicsIntervalGetterV2);
    g_control.mode = static_cast<std::uint32_t>(mode);
    g_control.limit = 1;
    g_control.expected_object = object;
    g_control.expected_vptr = vptr;
    g_control.original_getter = g_physics_interval_getter_continue_v2;
    g_interval_bits[0] = target_bits;
    g_evidence.status = kStatusArmed;
    g_control.enabled = 1;
    return true;
}

bool SelfTest() {
    alignas(8) std::uintptr_t object[2] = {0x12345000u, 0};
    std::uint32_t output = 0;
    const std::uint32_t original = 0x3c088889u;
    const std::uint32_t desired = 0x3c888889u;

    ArmSelfTest(Mode::kRecord, 0, reinterpret_cast<std::uintptr_t>(object),
                object[0]);
    InvokePhysicsIntervalGetterV2(object, &output);
    if (output != original || g_interval_bits[0] != original ||
        g_evidence.record_calls != 1 || g_evidence.overrides != 0 ||
        g_events[0].original_bits != original ||
        g_events[0].final_bits != original ||
        (g_events[0].flags & (kOriginalCalled | kOriginalValid |
                              kFinalCaptured | kObjectMatched)) !=
            (kOriginalCalled | kOriginalValid | kFinalCaptured |
             kObjectMatched))
        return false;

    output = 0;
    ArmSelfTest(Mode::kReplay, desired,
                reinterpret_cast<std::uintptr_t>(object), object[0]);
    InvokePhysicsIntervalGetterV2(object, &output);
    if (output != desired || g_evidence.replay_calls != 1 ||
        g_evidence.overrides != 1 ||
        g_events[0].original_bits != original ||
        g_events[0].requested_bits != desired ||
        g_events[0].final_bits != desired ||
        (g_events[0].flags & kOverrideApplied) == 0)
        return false;

    output = 0;
    ArmSelfTest(Mode::kReplay, 0x7fc00000u,
                reinterpret_cast<std::uintptr_t>(object), object[0]);
    InvokePhysicsIntervalGetterV2(object, &output);
    return output == original && g_evidence.overrides == 0 &&
           g_evidence.semantic_errors == 1 &&
           g_evidence.status == kStatusFault;
}
}  // namespace

int main() {
    const bool passed = SelfTest();
    std::printf("PHYSICS_INTERVAL_GETTER_PAYLOAD_V2_SELFTEST passed=%d "
                "installer=absent game_access=0 game_writes=0\n",
                passed ? 1 : 0);
    return passed ? 0 : 1;
}
#else
__attribute__((constructor)) void OnLoad() {
    Initialize();
}
#endif
