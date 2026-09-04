// Pure steering+brake pair transport for authoritative start-line replay.
// It replaces both halves of each C98/C9C pair from the selected packet's raw
// float bits. It performs no process access, thread control, write or action.

#include "authoritative_controls_transport_v1.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace a9tas::authoritative_controls_v1 {
namespace {

bool RawFloatIsPositiveZero(float value) noexcept {
    std::uint32_t bits = 1;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits == 0;
}

bool ByteArrayIsZero(const std::uint8_t* values, std::size_t size) noexcept {
    if (values == nullptr) return false;
    for (std::size_t index = 0; index < size; ++index)
        if (values[index] != 0) return false;
    return true;
}

}  // namespace

bool FrameIsSteeringBrake(
    const unified_tick_v1::RecordingFrameV1& frame) noexcept {
    if (frame.skip_override_flags != kSteeringBrakeSkipMask ||
        frame.flags != unified_tick_v1::kRequiredFrameFlags ||
        frame.reserved != 0 || frame.nitro_activation_count != 0 ||
        frame.respawn_button_press != 0 ||
        !ByteArrayIsZero(frame.padding, sizeof(frame.padding)) ||
        !std::isfinite(frame.steering) || std::fabs(frame.steering) > 1.0f ||
        !std::isfinite(frame.brake) || std::fabs(frame.brake) > 1.05f ||
        !RawFloatIsPositiveZero(frame.accelerator)) {
        return false;
    }
    for (float value : frame.barrel_angular_velocity)
        if (!RawFloatIsPositiveZero(value)) return false;
    for (float value : frame.barrel_rbx)
        if (!RawFloatIsPositiveZero(value)) return false;
    return true;
}

PairPlanV1 PlanPair(
    const unified_tick_v1::RecordingFrameV1& selected_packet,
    std::uint64_t live_pair_before) noexcept {
    PairPlanV1 plan{};
    if (!FrameIsSteeringBrake(selected_packet)) return plan;
    plan.outcome = PlanOutcomeV1::kReady;
    plan.tick = selected_packet.tick;
    std::memcpy(&plan.steering_bits, &selected_packet.steering,
                sizeof(plan.steering_bits));
    std::memcpy(&plan.brake_bits, &selected_packet.brake,
                sizeof(plan.brake_bits));
    plan.pair_before = live_pair_before;
    plan.pair_intended = static_cast<std::uint64_t>(plan.brake_bits) |
                         (static_cast<std::uint64_t>(plan.steering_bits) << 32);
    return plan;
}

bool VerifyAppliedPair(const PairPlanV1& plan,
                       std::uint64_t live_pair_after) noexcept {
    return plan.outcome == PlanOutcomeV1::kReady &&
           live_pair_after == plan.pair_intended &&
           static_cast<std::uint32_t>(live_pair_after) == plan.brake_bits &&
           static_cast<std::uint32_t>(live_pair_after >> 32) ==
               plan.steering_bits;
}

bool VerifyDualBoundary(
    const unified_tick_v1::RecordingFrameV1& selected_packet,
    std::uint64_t c98_pair_before, std::uint64_t c98_pair_after,
    std::uint64_t c9c_pair_before, std::uint64_t c9c_pair_after,
    DualBoundaryAuditV1* audit) noexcept {
    if (audit == nullptr) return false;
    *audit = {};
    const PairPlanV1 c98 = PlanPair(selected_packet, c98_pair_before);
    const PairPlanV1 c9c = PlanPair(selected_packet, c9c_pair_before);
    if (c98.outcome != PlanOutcomeV1::kReady ||
        c9c.outcome != PlanOutcomeV1::kReady || c98.tick != c9c.tick ||
        c98.steering_bits != c9c.steering_bits ||
        c98.brake_bits != c9c.brake_bits ||
        !VerifyAppliedPair(c98, c98_pair_after) ||
        !VerifyAppliedPair(c9c, c9c_pair_after))
        return false;
    audit->tick = selected_packet.tick;
    audit->steering_bits = c98.steering_bits;
    audit->brake_bits = c98.brake_bits;
    audit->c98 = c98;
    audit->c9c = c9c;
    audit->c98_pair_after = c98_pair_after;
    audit->c9c_pair_after = c9c_pair_after;
    return true;
}

}  // namespace a9tas::authoritative_controls_v1

#if defined(A9TAS_AUTHORITATIVE_CONTROLS_TRANSPORT_SELFTEST)
extern "C" int a9tas_authoritative_controls_transport_selftest_v1() {
    using namespace a9tas::authoritative_controls_v1;
    a9tas::unified_tick_v1::RecordingFrameV1 frame{};
    frame.tick = 12;
    frame.steering = -0.75f;
    frame.brake = -1.0f;
    frame.skip_override_flags = kSteeringBrakeSkipMask;
    frame.flags = a9tas::unified_tick_v1::kRequiredFrameFlags;
    if (!FrameIsSteeringBrake(frame)) return 1;
    const auto c98 = PlanPair(frame, 0x111111113f000000ULL);
    const auto c9c = PlanPair(frame, 0x222222223f800000ULL);
    if (c98.outcome != PlanOutcomeV1::kReady ||
        c98.pair_intended != c9c.pair_intended ||
        static_cast<std::uint32_t>(c98.pair_intended) != 0xbf800000U)
        return 2;
    DualBoundaryAuditV1 audit{};
    if (!VerifyDualBoundary(frame, c98.pair_before, c98.pair_intended,
                            c9c.pair_before, c9c.pair_intended, &audit) ||
        audit.tick != 12 || audit.brake_bits != 0xbf800000U)
        return 3;
    if (VerifyDualBoundary(frame, c98.pair_before,
                           c98.pair_intended ^ 1ULL, c9c.pair_before,
                           c9c.pair_intended, &audit))
        return 4;
    frame.skip_override_flags |= a9tas::unified_tick_v1::kSkipBrake;
    if (FrameIsSteeringBrake(frame)) return 5;
    frame.skip_override_flags = kSteeringBrakeSkipMask;
    frame.brake = 1.1f;
    if (FrameIsSteeringBrake(frame)) return 6;
    return 0;
}
#endif

#if !defined(A9TAS_AUTHORITATIVE_CONTROLS_TRANSPORT_NO_MAIN)
int main() {
#if defined(A9TAS_AUTHORITATIVE_CONTROLS_TRANSPORT_SELFTEST)
    const int result = a9tas_authoritative_controls_transport_selftest_v1();
    std::printf("AUTHORITATIVE_CONTROLS_TRANSPORT_SELFTEST passed=%d "
                "return=%d device_access=0 game_writes=0\n",
                result == 0 ? 1 : 0, result);
    return result;
#else
    std::puts("AUTHORITATIVE_CONTROLS_TRANSPORT_BUILD_ONLY "
              "device_access=0 game_writes=0");
    return 100;
#endif
}
#endif
