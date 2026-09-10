#pragma once

// G3 authoritative per-race/tick coordinator.
//
// This is the source-faithful ownership core that all later Android hooks use.
// The caller serializes access (the live payload uses one in-process lock).
// No process access, hook installation, game write or thread suspension exists
// in this file.

#include "unified_tick_recording_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace a9tas::g3_tick_coordinator_v1 {

namespace recording = a9tas::unified_tick_v1;

enum class Lifecycle : std::uint32_t {
  kInactive = 0,
  kArmed = 1,
  kInRace = 2,
  kEnding = 3,
  kFaulted = 4,
};

enum class TickPhase : std::uint32_t {
  kClosed = 0,
  kBegun = 1,
  kPrePhysics = 2,
  kFinalWritten = 3,
};

enum class ReplayMode : std::uint32_t {
  kInactive = 0,
  kActiveBlock = 1,
  kActiveNoBlock = 2,
};

enum BoundaryFlag : std::uint32_t {
  kBoundaryTickBegin = 1u << 0,
  kBoundaryPrePhysics = 1u << 1,
  kBoundaryFinalWriter = 1u << 2,
  kBoundaryTickEnd = 1u << 3,
  // A completed native update may interpolate without integrating. Never
  // manufacture kBoundaryPrePhysics or increment an interval counter for it.
  kBoundaryNoIntegration = 1u << 4,
};

inline constexpr std::uint32_t kCompleteTickBoundaryMask =
    kBoundaryTickBegin | kBoundaryPrePhysics | kBoundaryFinalWriter |
    kBoundaryTickEnd;
inline constexpr std::size_t kNoPacket =
    std::numeric_limits<std::size_t>::max();

enum class Result : std::int32_t {
  kArmed = 1,
  kRaceStarted = 2,
  kTickBegun = 3,
  kTickBegunWithoutReplay = 4,
  kNeedReplayData = 5,
  kPausedNoAdvance = 6,
  kPrePhysicsObserved = 7,
  kFinalWriterObserved = 8,
  kTickPublished = 9,
  kRaceEnding = 10,
  kInactive = 11,
  kPhysicsIntervalRepeated = 12,
  kTickDiscarded = 13,
  kInvalidArgument = -1,
  kWrongLifecycle = -2,
  kWrongTransition = -3,
  kWrongGeneration = -4,
  kWrongTick = -5,
  kWrongOrder = -6,
  kReplayIdentityMismatch = -7,
  kPacketInvalid = -8,
  kTickOverflow = -9,
};

struct ReplayQueueView {
  ReplayMode mode{ReplayMode::kInactive};
  bool communication_version_matches{true};
  bool block_mode_still_active{true};
  const recording::RecordingFrameV1* packets{};
  std::size_t packet_count{};
};

// Cleared at the authoritative Brake-equivalent tick-begin boundary and
// published exactly once at tick end. Later G4-G6 hooks append their natural
// call receipts to the reserved fields without changing tick ownership.
struct TickScratch {
  std::uint64_t session_id{};
  std::uint32_t generation{};
  std::uint32_t boundary_flags{};
  std::uint64_t tick{};
  std::size_t selected_packet_index{kNoPacket};
  std::uint32_t begin_tid{};
  std::uint32_t pre_physics_tid{};
  std::uint32_t final_writer_tid{};
  std::uint32_t end_tid{};
  std::uint32_t nitro_calls{};
  std::uint32_t barrel_angular_calls{};
  std::uint32_t barrel_rbx_calls{};
  std::uint32_t physics_interval_calls{};
};

struct State {
  Lifecycle lifecycle{Lifecycle::kInactive};
  TickPhase tick_phase{TickPhase::kClosed};
  Result last_result{Result::kInactive};
  std::uint64_t session_id{};
  std::uint32_t generation{};
  std::uint32_t reserved{};
  std::uint64_t tick{};
  std::size_t replay_head{};
  bool active_packet_present{};
  std::uint8_t padding[7]{};
  recording::RecordingFrameV1 active_packet{};
  TickScratch scratch{};
  std::uint64_t race_starts{};
  std::uint64_t race_ends{};
  std::uint64_t ticks_begun{};
  std::uint64_t pre_physics_events{};
  std::uint64_t physics_interval_calls{};
  std::uint64_t final_writer_events{};
  std::uint64_t ticks_published{};
  std::uint64_t packet_selections{};
  std::uint64_t blocked_empty{};
  std::uint64_t paused_noops{};
  std::uint64_t cancelled_blocks{};
  std::uint64_t future_gaps{};
  std::uint64_t stale_packets_dropped{};
  std::uint64_t failures{};
};

inline Result Fault(State* state, Result result) noexcept {
  if (state != nullptr) {
    state->lifecycle = Lifecycle::kFaulted;
    state->last_result = result;
    ++state->failures;
  }
  return result;
}

inline bool QueueValid(const ReplayQueueView& queue) noexcept {
  if (queue.mode != ReplayMode::kInactive &&
      queue.mode != ReplayMode::kActiveBlock &&
      queue.mode != ReplayMode::kActiveNoBlock)
    return false;
  return queue.packet_count == 0 || queue.packets != nullptr;
}

inline bool PacketValid(const recording::RecordingFrameV1& packet) noexcept {
  return packet.flags == recording::kRequiredFrameFlags &&
         packet.reserved == 0 &&
         (packet.skip_override_flags & ~recording::kSupportedSkipMask) == 0;
}

inline Result Arm(State* state, std::uint64_t session_id,
                  std::uint32_t generation) noexcept {
  if (state == nullptr || session_id == 0 || generation == 0)
    return Fault(state, Result::kInvalidArgument);
  if (state->lifecycle != Lifecycle::kInactive)
    return Fault(state, Result::kWrongLifecycle);
  if (generation <= state->generation)
    return Fault(state, Result::kWrongGeneration);

  const std::uint64_t prior_starts = state->race_starts;
  const std::uint64_t prior_ends = state->race_ends;
  const std::uint64_t prior_failures = state->failures;
  *state = {};
  state->lifecycle = Lifecycle::kArmed;
  state->tick_phase = TickPhase::kClosed;
  state->last_result = Result::kArmed;
  state->session_id = session_id;
  state->generation = generation;
  state->race_starts = prior_starts;
  state->race_ends = prior_ends;
  state->failures = prior_failures;
  state->scratch.selected_packet_index = kNoPacket;
  return Result::kArmed;
}

// Editing hand-off used after a replay has reached its exact target tick while
// the race is paused.  The game lifecycle deliberately remains IN_RACE; only
// TAS tick ownership is restarted so the newly recorded suffix begins at tick
// zero and can later be retimestamped directly after the immutable prefix.
inline Result RearmAtPausedRace(State* state, std::uint64_t session_id,
                                 std::uint32_t generation) noexcept {
  if (state == nullptr || session_id == 0 || generation == 0)
    return Fault(state, Result::kInvalidArgument);
  if (state->lifecycle != Lifecycle::kInRace ||
      state->tick_phase != TickPhase::kClosed)
    return Fault(state, Result::kWrongLifecycle);
  if (generation <= state->generation)
    return Fault(state, Result::kWrongGeneration);

  const std::uint64_t prior_starts = state->race_starts;
  const std::uint64_t prior_ends = state->race_ends;
  const std::uint64_t prior_failures = state->failures;
  *state = {};
  state->lifecycle = Lifecycle::kInRace;
  state->tick_phase = TickPhase::kClosed;
  state->last_result = Result::kRaceStarted;
  state->session_id = session_id;
  state->generation = generation;
  state->race_starts = prior_starts;
  state->race_ends = prior_ends;
  state->failures = prior_failures;
  state->scratch.selected_packet_index = kNoPacket;
  return Result::kRaceStarted;
}

// Upstream ReplayStateManager keeps recording while replay overrides are
// active.  Editing therefore stops the override at a closed tick and continues
// on the same monotonic tick timeline.  Preserve every already-published
// boundary/counter and change only the session identity used by future ticks.
inline Result ContinueReplayAsRecordAtClosedBoundary(
    State* state, std::uint64_t session_id,
    std::uint32_t generation) noexcept {
  if (state == nullptr || session_id == 0 || generation == 0)
    return Fault(state, Result::kInvalidArgument);
  if (state->lifecycle != Lifecycle::kInRace ||
      state->tick_phase != TickPhase::kClosed || state->tick == 0 ||
      state->ticks_begun != state->tick ||
      state->ticks_published != state->tick)
    return Fault(state, Result::kWrongLifecycle);
  if (generation <= state->generation)
    return Fault(state, Result::kWrongGeneration);

  state->session_id = session_id;
  state->generation = generation;
  state->last_result = Result::kRaceStarted;
  state->replay_head = 0;
  state->active_packet_present = false;
  std::memset(&state->active_packet, 0, sizeof(state->active_packet));
  std::memset(&state->scratch, 0, sizeof(state->scratch));
  state->scratch.selected_packet_index = kNoPacket;
  // The prefix is now represented as recording history, not an active replay
  // queue.  These counters have no bearing on the authoritative tick totals.
  state->packet_selections = 0;
  state->blocked_empty = 0;
  state->cancelled_blocks = 0;
  state->future_gaps = 0;
  state->stale_packets_dropped = 0;
  return Result::kRaceStarted;
}

inline Result OnRaceStart(State* state, std::uint32_t generation,
                          std::uint32_t before_state,
                          std::uint32_t after_state) noexcept {
  if (state == nullptr) return Result::kInvalidArgument;
  if (state->lifecycle != Lifecycle::kArmed)
    return Fault(state, Result::kWrongLifecycle);
  if (generation != state->generation)
    return Fault(state, Result::kWrongGeneration);
  if (before_state != 2u || after_state != 3u)
    return Fault(state, Result::kWrongTransition);
  state->lifecycle = Lifecycle::kInRace;
  state->tick_phase = TickPhase::kClosed;
  state->tick = 0;
  state->replay_head = 0;
  state->active_packet_present = false;
  std::memset(&state->active_packet, 0, sizeof(state->active_packet));
  std::memset(&state->scratch, 0, sizeof(state->scratch));
  state->scratch.selected_packet_index = kNoPacket;
  ++state->race_starts;
  return state->last_result = Result::kRaceStarted;
}

inline Result BeginTick(State* state, std::uint32_t generation,
                        std::uint32_t tid, bool paused,
                        const ReplayQueueView& queue) noexcept {
  if (state == nullptr || tid == 0 || !QueueValid(queue))
    return Fault(state, Result::kInvalidArgument);
  if (state->lifecycle != Lifecycle::kInRace)
    return Fault(state, Result::kWrongLifecycle);
  if (generation != state->generation)
    return Fault(state, Result::kWrongGeneration);
  if (state->tick_phase != TickPhase::kClosed)
    return Fault(state, Result::kWrongOrder);
  if (paused) {
    ++state->paused_noops;
    return state->last_result = Result::kPausedNoAdvance;
  }
  if (queue.mode != ReplayMode::kInactive &&
      !queue.communication_version_matches)
    return Fault(state, Result::kReplayIdentityMismatch);

  bool has_packet = false;
  std::size_t selected = kNoPacket;
  if (queue.mode != ReplayMode::kInactive) {
    while (state->replay_head < queue.packet_count &&
           queue.packets[state->replay_head].tick < state->tick) {
      ++state->replay_head;
      ++state->stale_packets_dropped;
    }
    if (state->replay_head == queue.packet_count) {
      if (queue.mode == ReplayMode::kActiveBlock &&
          queue.block_mode_still_active) {
        ++state->blocked_empty;
        return state->last_result = Result::kNeedReplayData;
      }
      if (queue.mode == ReplayMode::kActiveBlock)
        ++state->cancelled_blocks;
    } else {
      const recording::RecordingFrameV1& candidate =
          queue.packets[state->replay_head];
      if (!PacketValid(candidate))
        return Fault(state, Result::kPacketInvalid);
      if (candidate.tick == state->tick) {
        selected = state->replay_head++;
        state->active_packet = candidate;
        has_packet = true;
        ++state->packet_selections;
      } else {
        // A future packet remains queued.  Exact TAS playback must not let the
        // current tick run without its packet while ActiveBlock is still in
        // force.  ActiveNoBlock deliberately retains the upstream optional
        // record-without-override behavior.
        ++state->future_gaps;
        if (queue.mode == ReplayMode::kActiveBlock &&
            queue.block_mode_still_active) {
          ++state->blocked_empty;
          return state->last_result = Result::kNeedReplayData;
        }
      }
    }
  }

  std::memset(&state->scratch, 0, sizeof(state->scratch));
  state->scratch.session_id = state->session_id;
  state->scratch.generation = state->generation;
  state->scratch.boundary_flags = kBoundaryTickBegin;
  state->scratch.tick = state->tick;
  state->scratch.selected_packet_index = selected;
  state->scratch.begin_tid = tid;
  state->active_packet_present = has_packet;
  if (!has_packet)
    std::memset(&state->active_packet, 0, sizeof(state->active_packet));
  state->tick_phase = TickPhase::kBegun;
  ++state->ticks_begun;
  return state->last_result = has_packet ? Result::kTickBegun
                                         : Result::kTickBegunWithoutReplay;
}

inline bool ActivePacket(const State& state, std::uint32_t generation,
                         std::uint64_t tick,
                         const recording::RecordingFrameV1** packet) noexcept {
  if (packet == nullptr) return false;
  *packet = nullptr;
  if (state.lifecycle != Lifecycle::kInRace ||
      state.tick_phase == TickPhase::kClosed ||
      generation != state.generation || tick != state.tick ||
      !state.active_packet_present)
    return false;
  *packet = &state.active_packet;
  return true;
}

inline Result OnPrePhysics(State* state, std::uint32_t generation,
                           std::uint64_t tick, std::uint32_t tid) noexcept {
  if (state == nullptr || tid == 0) return Fault(state, Result::kInvalidArgument);
  if (state->lifecycle != Lifecycle::kInRace)
    return Fault(state, Result::kWrongLifecycle);
  if (generation != state->generation)
    return Fault(state, Result::kWrongGeneration);
  if (tick != state->tick) return Fault(state, Result::kWrongTick);
  if (state->tick_phase != TickPhase::kBegun &&
      state->tick_phase != TickPhase::kPrePhysics &&
      state->tick_phase != TickPhase::kFinalWritten)
    return Fault(state, Result::kWrongOrder);
  if (state->scratch.physics_interval_calls ==
      std::numeric_limits<std::uint32_t>::max())
    return Fault(state, Result::kTickOverflow);
  ++state->scratch.physics_interval_calls;
  ++state->physics_interval_calls;
  if (state->tick_phase != TickPhase::kBegun)
    return state->last_result = Result::kPhysicsIntervalRepeated;
  state->scratch.pre_physics_tid = tid;
  state->scratch.boundary_flags |= kBoundaryPrePhysics;
  state->tick_phase = TickPhase::kPrePhysics;
  ++state->pre_physics_events;
  return state->last_result = Result::kPrePhysicsObserved;
}

inline Result OnFinalWriter(State* state, std::uint32_t generation,
                            std::uint64_t tick, std::uint32_t tid,
                            bool completed_without_integration = false) noexcept {
  if (state == nullptr || tid == 0) return Fault(state, Result::kInvalidArgument);
  if (state->lifecycle != Lifecycle::kInRace)
    return Fault(state, Result::kWrongLifecycle);
  if (generation != state->generation)
    return Fault(state, Result::kWrongGeneration);
  if (tick != state->tick) return Fault(state, Result::kWrongTick);
  const bool zero_integration = completed_without_integration &&
      state->tick_phase == TickPhase::kBegun &&
      state->scratch.physics_interval_calls == 0;
  if (state->tick_phase != TickPhase::kPrePhysics && !zero_integration)
    return Fault(state, Result::kWrongOrder);
  if (zero_integration)
    state->scratch.boundary_flags |= kBoundaryNoIntegration;
  state->scratch.final_writer_tid = tid;
  state->scratch.boundary_flags |= kBoundaryFinalWriter;
  state->tick_phase = TickPhase::kFinalWritten;
  ++state->final_writer_events;
  return state->last_result = Result::kFinalWriterObserved;
}

inline Result OnTickEnd(State* state, std::uint32_t generation,
                        std::uint64_t tick, std::uint32_t tid,
                        TickScratch* published) noexcept {
  if (state == nullptr || published == nullptr || tid == 0)
    return Fault(state, Result::kInvalidArgument);
  if (state->lifecycle != Lifecycle::kInRace)
    return Fault(state, Result::kWrongLifecycle);
  if (generation != state->generation)
    return Fault(state, Result::kWrongGeneration);
  if (tick != state->tick) return Fault(state, Result::kWrongTick);
  if (state->tick_phase != TickPhase::kFinalWritten)
    return Fault(state, Result::kWrongOrder);
  if (state->tick == std::numeric_limits<std::uint64_t>::max())
    return Fault(state, Result::kTickOverflow);
  state->scratch.end_tid = tid;
  state->scratch.boundary_flags |= kBoundaryTickEnd;
  const auto expected_mask = state->scratch.physics_interval_calls == 0
      ? (kCompleteTickBoundaryMask & ~kBoundaryPrePhysics) | kBoundaryNoIntegration
      : kCompleteTickBoundaryMask;
  if (state->scratch.boundary_flags != expected_mask)
    return Fault(state, Result::kWrongOrder);
  *published = state->scratch;
  ++state->tick;
  ++state->ticks_published;
  state->tick_phase = TickPhase::kClosed;
  state->active_packet_present = false;
  std::memset(&state->active_packet, 0, sizeof(state->active_packet));
  std::memset(&state->scratch, 0, sizeof(state->scratch));
  state->scratch.selected_packet_index = kNoPacket;
  return state->last_result = Result::kTickPublished;
}

// A lifecycle transition may cut a real game frame after BeginTick but before
// Final Writer/TickEnd. Such a partial frame is not a replay packet. Discard
// only the current scratch and undo its aggregate semantic counters; already
// published ticks remain authoritative.
inline Result DiscardOpenTickForRaceEnd(State* state,
                                        std::uint32_t generation) noexcept {
  if (state == nullptr) return Result::kInvalidArgument;
  if (state->lifecycle != Lifecycle::kInRace)
    return Fault(state, Result::kWrongLifecycle);
  if (generation != state->generation)
    return Fault(state, Result::kWrongGeneration);
  if (state->tick_phase == TickPhase::kClosed ||
      (state->scratch.boundary_flags & kBoundaryTickBegin) == 0 ||
      state->ticks_begun == 0 ||
      state->physics_interval_calls < state->scratch.physics_interval_calls)
    return Fault(state, Result::kWrongOrder);
  --state->ticks_begun;
  state->physics_interval_calls -= state->scratch.physics_interval_calls;
  if ((state->scratch.boundary_flags & kBoundaryPrePhysics) != 0) {
    if (state->pre_physics_events == 0)
      return Fault(state, Result::kWrongOrder);
    --state->pre_physics_events;
  }
  if ((state->scratch.boundary_flags & kBoundaryFinalWriter) != 0) {
    if (state->final_writer_events == 0)
      return Fault(state, Result::kWrongOrder);
    --state->final_writer_events;
  }
  if (state->active_packet_present &&
      state->scratch.selected_packet_index != kNoPacket) {
    if (state->packet_selections == 0 || state->replay_head == 0 ||
        state->replay_head - 1 != state->scratch.selected_packet_index)
      return Fault(state, Result::kWrongOrder);
    --state->packet_selections;
    --state->replay_head;
  }
  state->tick_phase = TickPhase::kClosed;
  state->active_packet_present = false;
  std::memset(&state->active_packet, 0, sizeof(state->active_packet));
  std::memset(&state->scratch, 0, sizeof(state->scratch));
  state->scratch.selected_packet_index = kNoPacket;
  return state->last_result = Result::kTickDiscarded;
}

inline Result BeginRaceEnd(State* state, std::uint32_t generation,
                           std::uint32_t before_state,
                           std::uint32_t after_state) noexcept {
  if (state == nullptr) return Result::kInvalidArgument;
  if (state->lifecycle != Lifecycle::kInRace)
    return Fault(state, Result::kWrongLifecycle);
  if (generation != state->generation)
    return Fault(state, Result::kWrongGeneration);
  if (before_state != 3u || after_state == 3u)
    return Fault(state, Result::kWrongTransition);
  if (state->tick_phase != TickPhase::kClosed)
    return Fault(state, Result::kWrongOrder);
  state->lifecycle = Lifecycle::kEnding;
  return state->last_result = Result::kRaceEnding;
}

inline Result FinishRaceEnd(State* state, std::uint32_t generation) noexcept {
  if (state == nullptr) return Result::kInvalidArgument;
  if (state->lifecycle != Lifecycle::kEnding)
    return Fault(state, Result::kWrongLifecycle);
  if (generation != state->generation)
    return Fault(state, Result::kWrongGeneration);
  ++state->race_ends;
  state->lifecycle = Lifecycle::kInactive;
  state->tick_phase = TickPhase::kClosed;
  state->last_result = Result::kInactive;
  state->session_id = 0;
  state->tick = 0;
  state->replay_head = 0;
  state->active_packet_present = false;
  std::memset(&state->active_packet, 0, sizeof(state->active_packet));
  std::memset(&state->scratch, 0, sizeof(state->scratch));
  state->scratch.selected_packet_index = kNoPacket;
  return Result::kInactive;
}

}  // namespace a9tas::g3_tick_coordinator_v1
