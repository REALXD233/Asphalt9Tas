// BUILD-ONLY/OFFLINE steering-pair transport core.
//
// This core accepts only the 0xfe steering-only projection. It composes the
// selected packet's raw steering bits into the high 32 bits of the live C98 or
// C9C pair and preserves that boundary's low 32-bit brake value. It performs
// no process access, thread control, game write or action call.

#include "authoritative_steering_transport_v1.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace a9tas::authoritative_steering_v1 {

namespace {

bool RawFloatIsPositiveZero(float value) noexcept {
    std::uint32_t bits = 1;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits == 0;
}

bool ByteArrayIsZero(const std::uint8_t* values, std::size_t size) noexcept {
    if (values == nullptr) return false;
    for (std::size_t index = 0; index < size; ++index) {
        if (values[index] != 0) return false;
    }
    return true;
}

}  // namespace

bool FrameIsSteeringOnly(
    const unified_tick_v1::RecordingFrameV1& frame) noexcept {
    if (frame.skip_override_flags != kSteeringOnlySkipMask ||
        frame.flags != unified_tick_v1::kRequiredFrameFlags ||
        frame.reserved != 0 || frame.nitro_activation_count != 0 ||
        frame.respawn_button_press != 0 ||
        !ByteArrayIsZero(frame.padding, sizeof(frame.padding)) ||
        !std::isfinite(frame.steering) || std::fabs(frame.steering) > 1.0f ||
        !RawFloatIsPositiveZero(frame.brake) ||
        !RawFloatIsPositiveZero(frame.accelerator)) {
        return false;
    }
    for (float value : frame.barrel_angular_velocity) {
        if (!RawFloatIsPositiveZero(value)) return false;
    }
    for (float value : frame.barrel_rbx) {
        if (!RawFloatIsPositiveZero(value)) return false;
    }
    return true;
}

PairPlanV1 PlanPair(
    const unified_tick_v1::RecordingFrameV1& selected_packet,
    std::uint64_t live_pair_before) noexcept {
    PairPlanV1 plan{};
    if (!FrameIsSteeringOnly(selected_packet)) return plan;

    plan.outcome = PlanOutcomeV1::kReady;
    plan.tick = selected_packet.tick;
    std::memcpy(&plan.steering_bits, &selected_packet.steering,
                sizeof(plan.steering_bits));
    plan.preserved_brake_bits =
        static_cast<std::uint32_t>(live_pair_before & 0xffffffffULL);
    plan.pair_before = live_pair_before;
    plan.pair_intended =
        static_cast<std::uint64_t>(plan.preserved_brake_bits) |
        (static_cast<std::uint64_t>(plan.steering_bits) << 32);
    return plan;
}

bool VerifyAppliedPair(const PairPlanV1& plan,
                       std::uint64_t live_pair_after) noexcept {
    if (plan.outcome != PlanOutcomeV1::kReady ||
        live_pair_after != plan.pair_intended) {
        return false;
    }
    return static_cast<std::uint32_t>(live_pair_after & 0xffffffffULL) ==
               plan.preserved_brake_bits &&
           static_cast<std::uint32_t>(live_pair_after >> 32) ==
               plan.steering_bits;
}

bool VerifyDualBoundary(
    const unified_tick_v1::RecordingFrameV1& selected_packet,
    std::uint64_t c98_pair_before, std::uint64_t c98_pair_after,
    std::uint64_t c9c_pair_before, std::uint64_t c9c_pair_after,
    DualBoundaryAuditV1* audit) noexcept {
    if (audit == nullptr) return false;
    *audit = DualBoundaryAuditV1{};
    const PairPlanV1 c98 = PlanPair(selected_packet, c98_pair_before);
    const PairPlanV1 c9c = PlanPair(selected_packet, c9c_pair_before);
    if (c98.outcome != PlanOutcomeV1::kReady ||
        c9c.outcome != PlanOutcomeV1::kReady || c98.tick != c9c.tick ||
        c98.steering_bits != c9c.steering_bits ||
        !VerifyAppliedPair(c98, c98_pair_after) ||
        !VerifyAppliedPair(c9c, c9c_pair_after)) {
        return false;
    }
    audit->tick = selected_packet.tick;
    audit->steering_bits = c98.steering_bits;
    audit->c98 = c98;
    audit->c9c = c9c;
    audit->c98_pair_after = c98_pair_after;
    audit->c9c_pair_after = c9c_pair_after;
    return true;
}

}  // namespace a9tas::authoritative_steering_v1

#if defined(A9TAS_AUTHORITATIVE_STEERING_TRANSPORT_SELFTEST)

extern "C" int a9tas_authoritative_steering_transport_selftest_v1() {
    using namespace a9tas::authoritative_steering_v1;
    using a9tas::unified_tick_v1::RecordingFrameV1;

    RecordingFrameV1 frame{};
    frame.tick = 250;
    frame.steering = -0.75f;
    frame.skip_override_flags = kSteeringOnlySkipMask;
    frame.flags = a9tas::unified_tick_v1::kRequiredFrameFlags;
    if (!FrameIsSteeringOnly(frame)) return 1;

    std::uint32_t steering_bits = 0;
    std::memcpy(&steering_bits, &frame.steering, sizeof(steering_bits));
    const std::uint64_t c98_before = 0x111111213f800000ULL;
    const std::uint64_t c9c_before = 0x22222222bf800000ULL;
    const std::uint64_t c98_after =
        (static_cast<std::uint64_t>(steering_bits) << 32) | 0x3f800000ULL;
    const std::uint64_t c9c_after =
        (static_cast<std::uint64_t>(steering_bits) << 32) | 0xbf800000ULL;
    DualBoundaryAuditV1 audit{};
    if (!VerifyDualBoundary(frame, c98_before, c98_after, c9c_before,
                            c9c_after, &audit) ||
        audit.tick != 250 || audit.steering_bits != steering_bits ||
        audit.c98.preserved_brake_bits != 0x3f800000U ||
        audit.c9c.preserved_brake_bits != 0xbf800000U) {
        return 2;
    }

    if (VerifyDualBoundary(frame, c98_before, c98_after ^ 1ULL, c9c_before,
                           c9c_after, &audit)) {
        return 3;
    }
    if (VerifyDualBoundary(frame, c98_before, c98_after, c9c_before,
                           c9c_after ^ (1ULL << 32), &audit)) {
        return 4;
    }

    frame.skip_override_flags |= a9tas::unified_tick_v1::kSkipSteer;
    if (FrameIsSteeringOnly(frame)) return 5;
    frame.skip_override_flags = kSteeringOnlySkipMask;
    frame.brake = -1.0f;
    if (FrameIsSteeringOnly(frame)) return 6;
    frame.brake = 0.0f;
    frame.nitro_activation_count = 1;
    if (FrameIsSteeringOnly(frame)) return 7;
    frame.nitro_activation_count = 0;
    frame.steering = 1.25f;
    if (FrameIsSteeringOnly(frame)) return 8;

    frame.steering = 0.0f;
    if (!FrameIsSteeringOnly(frame)) return 9;
    const auto zero = PlanPair(frame, 0xdeadbeef12345678ULL);
    if (zero.outcome != PlanOutcomeV1::kReady ||
        zero.pair_intended != 0x0000000012345678ULL ||
        !VerifyAppliedPair(zero, zero.pair_intended)) {
        return 10;
    }
    return 0;
}

#endif

#if !defined(A9TAS_AUTHORITATIVE_STEERING_TRANSPORT_NO_MAIN)
int main() {
#if defined(A9TAS_AUTHORITATIVE_STEERING_TRANSPORT_SELFTEST)
    const int result = a9tas_authoritative_steering_transport_selftest_v1();
    std::printf(
        "AUTHORITATIVE_STEERING_TRANSPORT_SELFTEST passed=%d return=%d "
        "device_access=0 game_writes=0\n",
        result == 0 ? 1 : 0, result);
    return result;
#else
    std::puts(
        "AUTHORITATIVE_STEERING_TRANSPORT_V1_BUILD_ONLY runtime=disabled "
        "return=-100 device_access=0 game_writes=0");
    return -100;
#endif
}
#endif

