#pragma once

#include "unified_tick_recording_v1.h"

#include <cstddef>
#include <cstdint>

namespace a9tas::authoritative_tick_v1 {

enum class ReplayMode : std::uint32_t {
    kInactive = 0,
    kActiveBlockThread = 1,
    kActiveNoBlock = 2,
};

enum class SelectionOutcome : std::uint32_t {
    kOutsideRace = 0,
    kVersionMismatch = 1,
    kInactive = 2,
    kBlockedEmpty = 3,
    kModeCancelled = 4,
    kExactPacket = 5,
    kFutureHeadGap = 6,
    kNoPacket = 7,
    kStaleDroppedNoBlock = 8,
    kInvalidArgument = 9,
};

struct SelectionInputV1 {
    std::uint32_t current_tick;
    ReplayMode mode;
    bool in_race;
    bool communication_version_matches;
    bool block_mode_still_active;
};

struct SelectionResultV1 {
    SelectionOutcome outcome;
    std::size_t head_after;
    std::size_t selected_index;
    std::size_t stale_dropped;
    bool has_selected_packet;
    bool clear_previous_packet;
};

// The caller owns the queue and may append after kBlockedEmpty.  The function
// never sleeps, allocates, mutates a packet, or touches a process.  A runtime
// adapter reproduces the upstream loop by pumping control messages and calling
// this function again while the result is kBlockedEmpty.
SelectionResultV1 SelectOnNewTick(
    const SelectionInputV1& input,
    const unified_tick_v1::RecordingFrameV1* packets,
    std::size_t packet_count,
    std::size_t head) noexcept;

struct TickInputStateV1 {
    std::uint32_t race_tick;
    float steering;
    float brake;
    float accelerator;
    std::uint32_t nitro_activation_count;
    std::uint8_t respawn_button_press;
    std::uint8_t padding[3];
    float barrel_angular_velocity[3];
    float barrel_rbx[2];
};

// Copies the publish snapshot before clearing tick-specific fields. Continuous
// controls persist. Race tick increments exactly once only while in race.
bool EndTick(TickInputStateV1* current,
             bool in_race,
             TickInputStateV1* published) noexcept;

}  // namespace a9tas::authoritative_tick_v1
