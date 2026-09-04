#pragma once

#include "unified_tick_recording_v1.h"

#include <cstdint>

namespace a9tas::authoritative_steering_v1 {

inline constexpr std::uint32_t kSteeringOnlySkipMask =
    unified_tick_v1::kSupportedSkipMask & ~unified_tick_v1::kSkipSteer;

enum class PlanOutcomeV1 : std::uint32_t {
    kReady = 0,
    kInvalidFrame = 1,
};

struct PairPlanV1 {
    PlanOutcomeV1 outcome = PlanOutcomeV1::kInvalidFrame;
    std::uint64_t tick = 0;
    std::uint32_t steering_bits = 0;
    std::uint32_t preserved_brake_bits = 0;
    std::uint64_t pair_before = 0;
    std::uint64_t pair_intended = 0;
};

struct DualBoundaryAuditV1 {
    std::uint64_t tick = 0;
    std::uint32_t steering_bits = 0;
    std::uint32_t reserved = 0;
    PairPlanV1 c98{};
    PairPlanV1 c9c{};
    std::uint64_t c98_pair_after = 0;
    std::uint64_t c9c_pair_after = 0;
};

bool FrameIsSteeringOnly(
    const unified_tick_v1::RecordingFrameV1& frame) noexcept;

PairPlanV1 PlanPair(
    const unified_tick_v1::RecordingFrameV1& selected_packet,
    std::uint64_t live_pair_before) noexcept;

bool VerifyAppliedPair(const PairPlanV1& plan,
                       std::uint64_t live_pair_after) noexcept;

bool VerifyDualBoundary(
    const unified_tick_v1::RecordingFrameV1& selected_packet,
    std::uint64_t c98_pair_before, std::uint64_t c98_pair_after,
    std::uint64_t c9c_pair_before, std::uint64_t c9c_pair_after,
    DualBoundaryAuditV1* audit) noexcept;

}  // namespace a9tas::authoritative_steering_v1

