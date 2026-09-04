// BUILD-ONLY/OFFLINE source-faithful tick selection core.
//
// This artifact models AluTasV2 StateManager::OnNewTick/OnEndTick. It has no
// game binding, process access, thread control, input, action, or memory-write
// transport. Runtime integration remains unavailable.

#include "authoritative_tick_state_machine_v1.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace a9tas::authoritative_tick_v1 {

namespace {

SelectionResultV1 Result(SelectionOutcome outcome,
                         std::size_t head_after,
                         std::size_t selected_index,
                         std::size_t stale_dropped,
                         bool has_selected_packet) noexcept {
    return {outcome, head_after, selected_index, stale_dropped,
            has_selected_packet, true};
}

}  // namespace

SelectionResultV1 SelectOnNewTick(
    const SelectionInputV1& input,
    const unified_tick_v1::RecordingFrameV1* packets,
    std::size_t packet_count,
    std::size_t head) noexcept {
    if (head > packet_count || (packet_count != 0 && packets == nullptr)) {
        return Result(SelectionOutcome::kInvalidArgument, head, 0, 0, false);
    }
    if (!input.in_race) {
        return Result(SelectionOutcome::kOutsideRace, head, 0, 0, false);
    }
    if (!input.communication_version_matches) {
        return Result(SelectionOutcome::kVersionMismatch, head, 0, 0, false);
    }
    if (input.mode == ReplayMode::kInactive) {
        return Result(SelectionOutcome::kInactive, head, 0, 0, false);
    }
    if (input.mode != ReplayMode::kActiveBlockThread &&
        input.mode != ReplayMode::kActiveNoBlock) {
        return Result(SelectionOutcome::kInvalidArgument, head, 0, 0, false);
    }

    if (input.mode == ReplayMode::kActiveNoBlock) {
        if (head == packet_count) {
            return Result(SelectionOutcome::kNoPacket, head, 0, 0, false);
        }
        const std::uint64_t packet_tick = packets[head].tick;
        if (packet_tick > std::numeric_limits<std::uint32_t>::max()) {
            return Result(SelectionOutcome::kInvalidArgument, head, 0, 0,
                          false);
        }
        if (packet_tick == input.current_tick) {
            return Result(SelectionOutcome::kExactPacket, head + 1, head, 0,
                          true);
        }
        if (packet_tick < input.current_tick) {
            return Result(SelectionOutcome::kStaleDroppedNoBlock, head + 1, 0,
                          1, false);
        }
        return Result(SelectionOutcome::kFutureHeadGap, head, 0, 0, false);
    }

    if (!input.block_mode_still_active) {
        return Result(SelectionOutcome::kModeCancelled, head, 0, 0, false);
    }

    std::size_t cursor = head;
    std::size_t stale_dropped = 0;
    while (cursor < packet_count) {
        const std::uint64_t packet_tick = packets[cursor].tick;
        if (packet_tick > std::numeric_limits<std::uint32_t>::max()) {
            return Result(SelectionOutcome::kInvalidArgument, cursor, 0,
                          stale_dropped, false);
        }
        if (packet_tick == input.current_tick) {
            return Result(SelectionOutcome::kExactPacket, cursor + 1, cursor,
                          stale_dropped, true);
        }
        if (packet_tick < input.current_tick) {
            ++cursor;
            ++stale_dropped;
            continue;
        }
        // Literal upstream behavior: a future queue head breaks the blocking
        // loop, remains queued and leaves the current tick without a packet.
        return Result(SelectionOutcome::kFutureHeadGap, cursor, 0,
                      stale_dropped, false);
    }
    return Result(SelectionOutcome::kBlockedEmpty, cursor, 0, stale_dropped,
                  false);
}

bool EndTick(TickInputStateV1* current,
             bool in_race,
             TickInputStateV1* published) noexcept {
    if (current == nullptr || published == nullptr || current == published) {
        return false;
    }
    if (in_race && current->race_tick ==
                       std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    std::memcpy(published, current, sizeof(*published));
    current->nitro_activation_count = 0;
    current->respawn_button_press = 0;
    std::memset(current->padding, 0, sizeof(current->padding));
    std::memset(current->barrel_angular_velocity, 0,
                sizeof(current->barrel_angular_velocity));
    std::memset(current->barrel_rbx, 0, sizeof(current->barrel_rbx));
    if (in_race) {
        ++current->race_tick;
    }
    return true;
}

}  // namespace a9tas::authoritative_tick_v1

#if defined(A9TAS_AUTHORITATIVE_TICK_SELFTEST)

namespace {

using a9tas::authoritative_tick_v1::EndTick;
using a9tas::authoritative_tick_v1::ReplayMode;
using a9tas::authoritative_tick_v1::SelectionInputV1;
using a9tas::authoritative_tick_v1::SelectionOutcome;
using a9tas::authoritative_tick_v1::SelectOnNewTick;
using a9tas::authoritative_tick_v1::TickInputStateV1;
using a9tas::unified_tick_v1::RecordingFrameV1;

bool SelfTest() {
    RecordingFrameV1 packets[4]{};
    packets[0].tick = 3;
    packets[1].tick = 6;
    packets[2].tick = 7;
    packets[3].tick = 8;
    SelectionInputV1 input{7, ReplayMode::kActiveBlockThread, true, true,
                           true};
    const auto exact = SelectOnNewTick(input, packets, 4, 0);
    if (exact.outcome != SelectionOutcome::kExactPacket ||
        exact.selected_index != 2 || exact.head_after != 3 ||
        exact.stale_dropped != 2 || !exact.has_selected_packet ||
        !exact.clear_previous_packet) {
        return false;
    }
    const auto future = SelectOnNewTick(input, packets, 4, 3);
    if (future.outcome != SelectionOutcome::kFutureHeadGap ||
        future.head_after != 3 || future.has_selected_packet) {
        return false;
    }
    const auto empty = SelectOnNewTick(input, packets, 4, 4);
    if (empty.outcome != SelectionOutcome::kBlockedEmpty) {
        return false;
    }
    packets[3].tick = std::numeric_limits<std::uint32_t>::max();
    ++packets[3].tick;
    const auto wide_tick = SelectOnNewTick(input, packets, 4, 3);
    if (wide_tick.outcome != SelectionOutcome::kInvalidArgument) {
        return false;
    }

    TickInputStateV1 state{};
    state.race_tick = 9;
    state.steering = 0.25f;
    state.brake = -1.0f;
    state.accelerator = 0.75f;
    state.nitro_activation_count = 2;
    state.respawn_button_press = 1;
    state.barrel_angular_velocity[0] = 1.0f;
    state.barrel_rbx[0] = 4.0f;
    TickInputStateV1 published{};
    if (!EndTick(&state, true, &published) || published.race_tick != 9 ||
        published.nitro_activation_count != 2 || state.race_tick != 10 ||
        state.steering != 0.25f || state.brake != -1.0f ||
        state.accelerator != 0.75f || state.nitro_activation_count != 0 ||
        state.respawn_button_press != 0 ||
        state.barrel_angular_velocity[0] != 0.0f ||
        state.barrel_rbx[0] != 0.0f) {
        return false;
    }
    return true;
}

}  // namespace

extern "C" int a9tas_authoritative_tick_selftest_v1() {
    return SelfTest() ? 0 : 1;
}

#endif

#if !defined(A9TAS_AUTHORITATIVE_TICK_NO_MAIN)
int main() {
    std::puts(
        "AUTH_TICK_BUILD_ONLY runtime=disabled return=-100 device_access=0");
    return 0;
}
#endif
