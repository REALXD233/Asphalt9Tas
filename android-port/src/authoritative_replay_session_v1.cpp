// BUILD-ONLY/OFFLINE source-faithful replay-session core.
//
// This composes the upstream tool-side replay queue with the DLL-side exact
// tick selector and end-tick reset. It deliberately has no Android adapter,
// process access, thread control, input injection, action call, or game write.

#include "authoritative_replay_session_v1.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace a9tas::authoritative_session_v1 {

namespace {

using authoritative_tick_v1::ReplayMode;
using authoritative_tick_v1::SelectionOutcome;
using unified_tick_v1::RecordingFrameV1;

}  // namespace

bool ReplaySessionV1::ReplayShapeValid(
    const RecordingFrameV1* frames,
    std::size_t frame_count,
    std::uint32_t fixed_interval_us) const noexcept {
    if (frames == nullptr || frame_count == 0 ||
        frame_count > unified_tick_v1::kMaximumFrames ||
        fixed_interval_us < unified_tick_v1::kMinimumFixedIntervalUs ||
        fixed_interval_us > unified_tick_v1::kMaximumFixedIntervalUs ||
        frame_count - 1 > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    std::uint64_t previous_time = 0;
    for (std::size_t index = 0; index < frame_count; ++index) {
        const auto& frame = frames[index];
        if (frame.tick != index ||
            (index != 0 && frame.monotonic_ns < previous_time)) {
            return false;
        }
        previous_time = frame.monotonic_ns;
    }
    return true;
}

void ReplaySessionV1::ClearInputQueue() noexcept {
    queue_size_ = 0;
    has_selected_packet_ = false;
    std::memset(&selected_packet_, 0, sizeof(selected_packet_));
}

void ReplaySessionV1::InitializeQueuedReplay() noexcept {
    playback_index_ = 0;
    replay_mode_ = ReplayMode::kActiveBlockThread;
    installed_fixed_interval_us_ = queued_fixed_interval_us_;
}

QueueReplayOutcome ReplaySessionV1::QueueReplay(
    const RecordingFrameV1* frames,
    std::size_t frame_count,
    std::uint32_t fixed_interval_us,
    std::uint32_t target_tick,
    bool in_race,
    std::uint32_t current_race_tick) noexcept {
    if (!ReplayShapeValid(frames, frame_count, fixed_interval_us)) {
        return QueueReplayOutcome::kInvalidReplay;
    }
    if (in_race && has_replay_ && final_tick_ >= current_race_tick) {
        return QueueReplayOutcome::kUnfinishedInRaceReplay;
    }

    ClearInputQueue();
    frames_ = frames;
    frame_count_ = frame_count;
    final_tick_ = std::min<std::uint32_t>(
        target_tick, static_cast<std::uint32_t>(frame_count - 1));
    playback_index_ = 0;
    has_replay_ = true;
    queued_fixed_interval_us_ = fixed_interval_us;
    if (!in_race) {
        InitializeQueuedReplay();
    }
    return QueueReplayOutcome::kAccepted;
}

bool ReplaySessionV1::ChangeTargetTick(std::uint32_t target_tick) noexcept {
    if (!has_replay_ || frame_count_ == 0) return false;
    final_tick_ = std::min<std::uint32_t>(
        target_tick, static_cast<std::uint32_t>(frame_count_ - 1));
    return true;
}

void ReplaySessionV1::ClearQueuedReplay() noexcept {
    frames_ = nullptr;
    frame_count_ = 0;
    final_tick_ = 0;
    playback_index_ = 0;
    has_replay_ = false;
    replay_mode_ = ReplayMode::kInactive;
    queued_fixed_interval_us_ = 0;
    installed_fixed_interval_us_ = 0;
    ClearInputQueue();
}

void ReplaySessionV1::OnRaceEnded() noexcept {
    if (has_replay_) InitializeQueuedReplay();
    // Literal upstream order: re-arm retained playback, then discard queued
    // packets from the completed race.
    ClearInputQueue();
}

void ReplaySessionV1::OnUpdate(std::uint32_t current_race_tick) noexcept {
    // Literal upstream order, including the target-tick-zero edge case.
    if (!has_replay_ || current_race_tick >= final_tick_) {
        replay_mode_ = ReplayMode::kInactive;
        return;
    }

    while (queue_size_ < queue_.size()) {
        const auto& frame = frames_[playback_index_];
        if (frame.tick > final_tick_) break;
        queue_[queue_size_++] = frame;
        // Literal Replay::IncrementFrameIndex saturation. When final_tick_ is
        // the physical last frame, the last packet repeats until capacity.
        playback_index_ =
            std::min(playback_index_ + 1, frame_count_ - 1);
    }
}

void ReplaySessionV1::ConsumeQueuePrefix(std::size_t count) noexcept {
    if (count == 0) return;
    if (count >= queue_size_) {
        queue_size_ = 0;
        return;
    }
    std::memmove(queue_.data(), queue_.data() + count,
                 (queue_size_ - count) * sizeof(queue_.front()));
    queue_size_ -= count;
}

BeginTickResultV1 ReplaySessionV1::BeginTick(
    std::uint32_t current_race_tick,
    bool in_race,
    bool communication_version_matches,
    bool block_mode_still_active) noexcept {
    has_selected_packet_ = false;
    std::memset(&selected_packet_, 0, sizeof(selected_packet_));

    const authoritative_tick_v1::SelectionInputV1 input{
        current_race_tick,
        replay_mode_,
        in_race,
        communication_version_matches,
        block_mode_still_active,
    };
    BeginTickResultV1 result{};
    result.selection = authoritative_tick_v1::SelectOnNewTick(
        input, queue_.data(), queue_size_, 0);
    if (result.selection.has_selected_packet) {
        result.packet = queue_[result.selection.selected_index];
        selected_packet_ = result.packet;
        has_selected_packet_ = true;
    }
    ConsumeQueuePrefix(result.selection.head_after);
    if (result.selection.outcome == SelectionOutcome::kModeCancelled) {
        replay_mode_ = ReplayMode::kInactive;
    }
    return result;
}

bool ReplaySessionV1::EndTick(
    authoritative_tick_v1::TickInputStateV1* current,
    bool in_race,
    authoritative_tick_v1::TickInputStateV1* published) noexcept {
    return authoritative_tick_v1::EndTick(current, in_race, published);
}

bool ReplaySessionV1::QueueTickAt(std::size_t index,
                                  std::uint64_t* tick) const noexcept {
    if (tick == nullptr || index >= queue_size_) return false;
    *tick = queue_[index].tick;
    return true;
}

}  // namespace a9tas::authoritative_session_v1

#if defined(A9TAS_AUTHORITATIVE_REPLAY_SESSION_SELFTEST)

namespace {

using a9tas::authoritative_session_v1::QueueReplayOutcome;
using a9tas::authoritative_session_v1::ReplaySessionV1;
using a9tas::authoritative_session_v1::kReplayQueueCapacity;
using a9tas::authoritative_tick_v1::ReplayMode;
using a9tas::authoritative_tick_v1::SelectionOutcome;
using a9tas::authoritative_tick_v1::TickInputStateV1;
using a9tas::unified_tick_v1::RecordingFrameV1;

bool ReplaySessionSelfTest() {
    RecordingFrameV1 frames[4]{};
    for (std::size_t index = 0; index < 4; ++index) {
        frames[index].tick = index;
        frames[index].monotonic_ns = index * 100;
    }

    ReplaySessionV1 session;
    if (session.QueueReplay(frames, 4, 16667, 99, false, 0) !=
            QueueReplayOutcome::kAccepted ||
        session.replay_mode() != ReplayMode::kActiveBlockThread ||
        session.final_tick() != 3 ||
        session.queued_fixed_interval_us() != 16667 ||
        session.installed_fixed_interval_us() != 16667) {
        return false;
    }
    session.OnUpdate(0);
    if (session.queued_packets() != kReplayQueueCapacity ||
        session.playback_index() != 3) {
        return false;
    }
    std::uint64_t tail_tick = 0;
    if (!session.QueueTickAt(kReplayQueueCapacity - 1, &tail_tick) ||
        tail_tick != 3) {
        return false;
    }

    const auto tick0 = session.BeginTick(0, true, true, true);
    if (tick0.selection.outcome != SelectionOutcome::kExactPacket ||
        !tick0.selection.has_selected_packet || tick0.packet.tick != 0 ||
        !session.has_selected_packet()) {
        return false;
    }
    const auto tick2 = session.BeginTick(2, true, true, true);
    if (tick2.selection.outcome != SelectionOutcome::kExactPacket ||
        tick2.selection.stale_dropped != 1 || tick2.packet.tick != 2) {
        return false;
    }
    const auto future = session.BeginTick(1, true, true, true);
    if (future.selection.outcome != SelectionOutcome::kFutureHeadGap ||
        future.selection.has_selected_packet || session.has_selected_packet()) {
        return false;
    }

    TickInputStateV1 current{};
    current.race_tick = 7;
    current.steering = 0.5f;
    current.brake = -1.0f;
    current.accelerator = 0.75f;
    current.nitro_activation_count = 2;
    current.respawn_button_press = 1;
    current.barrel_angular_velocity[2] = 3.0f;
    current.barrel_rbx[1] = 4.0f;
    TickInputStateV1 published{};
    if (!session.EndTick(&current, true, &published) ||
        published.race_tick != 7 || published.nitro_activation_count != 2 ||
        current.race_tick != 8 || current.steering != 0.5f ||
        current.nitro_activation_count != 0 ||
        current.respawn_button_press != 0 ||
        current.barrel_angular_velocity[2] != 0.0f ||
        current.barrel_rbx[1] != 0.0f) {
        return false;
    }

    session.OnRaceEnded();
    if (!session.has_replay() ||
        session.replay_mode() != ReplayMode::kActiveBlockThread ||
        session.playback_index() != 0 || session.queued_packets() != 0) {
        return false;
    }

    ReplaySessionV1 target_zero;
    if (target_zero.QueueReplay(frames, 4, 16667, 0, false, 0) !=
        QueueReplayOutcome::kAccepted) {
        return false;
    }
    target_zero.OnUpdate(0);
    if (target_zero.replay_mode() != ReplayMode::kInactive ||
        target_zero.queued_packets() != 0) {
        return false;
    }

    ReplaySessionV1 in_race_queue;
    if (in_race_queue.QueueReplay(frames, 4, 20000, 3, true, 4) !=
            QueueReplayOutcome::kAccepted ||
        in_race_queue.replay_mode() != ReplayMode::kInactive ||
        in_race_queue.queued_fixed_interval_us() != 20000 ||
        in_race_queue.installed_fixed_interval_us() != 0) {
        return false;
    }
    in_race_queue.OnRaceEnded();
    if (in_race_queue.replay_mode() != ReplayMode::kActiveBlockThread ||
        in_race_queue.installed_fixed_interval_us() != 20000) {
        return false;
    }

    // The tool observes the state published by EndTick, not the incremented
    // next-state tick. This ordering is what lets the inclusive final packet
    // be consumed before OnUpdate disables block mode.
    ReplaySessionV1 integrated;
    if (integrated.QueueReplay(frames, 4, 16667, 3, false, 0) !=
        QueueReplayOutcome::kAccepted) {
        return false;
    }
    integrated.OnUpdate(0);
    TickInputStateV1 integrated_state{};
    for (std::uint32_t tick = 0; tick < 4; ++tick) {
        const auto selected = integrated.BeginTick(tick, true, true, true);
        if (selected.selection.outcome != SelectionOutcome::kExactPacket ||
            selected.packet.tick != tick) {
            return false;
        }
        integrated_state.race_tick = tick;
        TickInputStateV1 published_tick{};
        if (!integrated.EndTick(&integrated_state, true, &published_tick) ||
            published_tick.race_tick != tick ||
            integrated_state.race_tick != tick + 1) {
            return false;
        }
        integrated.OnUpdate(published_tick.race_tick);
    }
    if (integrated.replay_mode() != ReplayMode::kInactive) return false;
    return true;
}

}  // namespace

extern "C" int a9tas_authoritative_replay_session_selftest_v1() {
    return ReplaySessionSelfTest() ? 0 : 1;
}

#endif

#if !defined(A9TAS_AUTHORITATIVE_REPLAY_SESSION_NO_MAIN)
int main() {
    std::puts(
        "AUTH_REPLAY_SESSION_BUILD_ONLY runtime=disabled return=-100 "
        "device_access=0");
    return 0;
}
#endif
