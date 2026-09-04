#pragma once

#include "authoritative_adapter_bridge_v1.h"

#include <cstddef>
#include <cstdint>

namespace a9tas::authoritative_natural_v1 {

enum class ModeV1 : std::uint32_t {
    kSearching = 0,
    kReplay = 1,
    kComplete = 2,
    kPoisoned = 3,
};

struct AdvanceResultV1 {
    bool accepted = false;
    bool search_cycle_committed = false;
    bool anchor_matched = false;
    ModeV1 mode = ModeV1::kPoisoned;
    std::uint32_t search_cycles = 0;
    std::uint32_t matched_cycle = 0xffffffffU;
    std::uint32_t phase_stage = 0;
    std::uint32_t phase_frame_index = 0;
    authoritative_bridge_v1::AdvanceResultV1 replay{};
};

struct ControllerV1;

ControllerV1* Create(
    const unified_tick_v1::RecordingFrameV1* frames,
    std::size_t frame_count, std::uint32_t fixed_interval_us,
    std::uint32_t target_tick) noexcept;
void Destroy(ControllerV1* controller) noexcept;
bool RejectSearchPrefix(ControllerV1* controller) noexcept;
AdvanceResultV1 Inspect(const ControllerV1* controller) noexcept;

AdvanceResultV1 Advance(
    ControllerV1* controller, authoritative_bridge_v1::EventV1 event,
    bool anchor_comparator_available = false,
    bool anchor_matches = false, bool payload_equal = false,
    bool consumed_tick_valid = false,
    std::uint32_t consumed_tick = 0) noexcept;

}  // namespace a9tas::authoritative_natural_v1
