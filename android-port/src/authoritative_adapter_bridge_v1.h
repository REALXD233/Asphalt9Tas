#pragma once

#include "unified_tick_recording_v1.h"

#include <cstddef>
#include <cstdint>

namespace a9tas::authoritative_bridge_v1 {

enum class EventV1 : std::uint32_t {
    kDeltaNonzero = 0,
    kDeltaZero = 1,
    kC98 = 2,
    kC9C = 3,
    kPrefixCertified = 4,
    kCompletion = 5,
    kCallbackOpen = 6,
    kF64 = 7,
    kCallbackClose = 8,
    kDeferredCallbackClear = 9,
    kWorldCommit = 10,
};

struct AdvanceResultV1 {
    bool accepted = false;
    bool packet_selected = false;
    bool selection_rolled_back = false;
    bool committed = false;
    bool complete = false;
    bool selected_packet_valid = false;
    bool poisoned = false;
    std::uint32_t error = 0;
    std::uint32_t selection_outcome = 0;
    std::uint32_t phase_actions = 0;
    std::uint32_t current_tick = 0;
    std::uint32_t phase_stage = 0;
    std::uint32_t phase_frame_index = 0;
    std::uint32_t committed_frames = 0;
    unified_tick_v1::RecordingFrameV1 selected_packet{};
};

struct BridgeV1;

BridgeV1* Create(const unified_tick_v1::RecordingFrameV1* frames,
                 std::size_t frame_count, std::uint32_t fixed_interval_us,
                 std::uint32_t target_tick) noexcept;
void Destroy(BridgeV1* bridge) noexcept;

AdvanceResultV1 Advance(BridgeV1* bridge, EventV1 event,
                        bool comparator_available = false,
                        bool payload_equal = false,
                        bool consumed_tick_valid = false,
                        std::uint32_t consumed_tick = 0) noexcept;

}  // namespace a9tas::authoritative_bridge_v1

