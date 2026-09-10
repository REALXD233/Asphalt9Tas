#pragma once

// Exact-build Android adapter for the AluTasV2 tick order. Unlike the
// Windows build, Android does not call the BrakeValue setter every physics
// tick.  The source-faithful split is PhysicsContext token submission for
// BeginTick, followed by the natural Physics Interval getter stream for the
// PrePhysics receipt.  The legacy combined interval path remains available to
// the frozen G3 neutral baseline only.

#include "g3_tick_coordinator_v1.h"

#include <cstddef>
#include <cstdint>

namespace a9tas::g3_boundary_adapter_v1 {

namespace coordinator = a9tas::g3_tick_coordinator_v1;

inline constexpr std::uintptr_t kTickBeginRva = 0x38B7AC4;
inline constexpr std::uintptr_t kPhysicsSubmitRva = kTickBeginRva;
inline constexpr std::uintptr_t kPhysicsIntervalRva = 0x3695474;
inline constexpr std::uintptr_t kPhysicsContextVtableRva = 0x8103830;
inline constexpr std::uintptr_t kPhysicsImplementationVtableRva = 0x7EECD88;
inline constexpr std::uintptr_t kTickBeginInterfaceAddressPointRva =
    kPhysicsContextVtableRva;
inline constexpr std::uintptr_t kFinalWriterRva = 0x367D66C;
inline constexpr std::uintptr_t kFrameEventRva = 0x38B78E4;
inline constexpr std::uintptr_t kFrameEventSchedulerReturnRva = 0x3794C78;
// G8 lifecycle recordings must not inherit the historical 900-tick Gate
// capacity. 24000 ticks cover more than 150 seconds even at 6944 us (144Hz).
// Controllers keep complete diagnostic snapshots on the heap, not the stack.
inline constexpr std::uint32_t kMaximumNeutralReceipts = 24000;

enum class Result : std::int32_t {
  kObserved = 1,
  kIgnoredUnqualified = 2,
  kWaitingForRace = 3,
  kComplete = 4,
  kWaitingForTick = 5,
  kRaceEnded = 6,
  kInvalidArgument = -1,
  kCoreFault = -2,
};

struct Config {
  std::uint64_t session_id{};
  std::uint32_t generation{};
  std::uint32_t frame_limit{};
  std::uintptr_t game_base{};
  std::uintptr_t expected_begin_owner{};
  std::uintptr_t expected_begin_owner_vptr{};
  std::uintptr_t expected_interval_owner{};
  std::uintptr_t expected_interval_owner_vptr{};
  std::uintptr_t expected_player{};
  std::uintptr_t expected_player_vptr{};
  // Build identities are data, not Android channel policy.  Defaults preserve
  // every frozen reference-build G3 caller; G8 supplies the resolver-published
  // values for other native-library builds.
  std::uintptr_t physics_context_vtable_rva{kPhysicsContextVtableRva};
  std::uintptr_t physics_implementation_vtable_rva{
      kPhysicsImplementationVtableRva};
  std::uintptr_t frame_event_scheduler_return_rva{
      kFrameEventSchedulerReturnRva};
  // G3 neutral gates leave this inactive. G4 supplies the already validated
  // recording queue so packet selection remains owned by the proven G3
  // coordinator rather than by a second tick state machine.
  coordinator::ReplayQueueView replay_queue{};
};

struct State {
  coordinator::State coordinator{};
  coordinator::TickScratch receipts[kMaximumNeutralReceipts]{};
  std::uintptr_t bound_begin_owner{};
  std::uintptr_t bound_begin_owner_vptr{};
  std::uint32_t receipt_count{};
  std::uint32_t complete{};
  std::uint64_t tick_begin_entries{};
  std::uint64_t pre_physics_entries{};
  std::uint64_t physics_interval_calls{};
  std::uint64_t final_writer_returns{};
  std::uint64_t frame_event_returns{};
  std::uint64_t idle_final_writer_returns{};
  std::uint64_t idle_frame_event_returns{};
  std::uint64_t ignored_unqualified{};
  std::uint64_t inferred_race_starts{};
  std::uint64_t inferred_race_ends{};
  std::uint64_t core_faults{};
};

// Receipt slots are authoritative only below receipt_count, and OnTickEnd
// overwrites the complete TickScratch before incrementing that count.  Do not
// clear the full capacity inside a bounded NativeBridge command.
// Reset only session metadata; stale unreachable slots remain non-authoritative.
inline void ResetSessionMetadata(State* state) noexcept {
  state->coordinator = {};
  state->bound_begin_owner = 0;
  state->bound_begin_owner_vptr = 0;
  state->receipt_count = 0;
  state->complete = 0;
  state->tick_begin_entries = 0;
  state->pre_physics_entries = 0;
  state->physics_interval_calls = 0;
  state->final_writer_returns = 0;
  state->frame_event_returns = 0;
  state->idle_final_writer_returns = 0;
  state->idle_frame_event_returns = 0;
  state->ignored_unqualified = 0;
  state->inferred_race_starts = 0;
  state->inferred_race_ends = 0;
  state->core_faults = 0;
}

inline bool ConfigValid(const Config& config) noexcept {
  const bool legacy_combined =
      config.expected_begin_owner == config.expected_interval_owner;
  const bool begin_identity =
      legacy_combined
          ? config.expected_begin_owner_vptr ==
                config.game_base + config.physics_implementation_vtable_rva
          : config.expected_begin_owner_vptr ==
                config.game_base + config.physics_context_vtable_rva;
  return config.session_id != 0 && config.generation != 0 &&
         config.frame_limit != 0 &&
         config.frame_limit <= kMaximumNeutralReceipts &&
         config.game_base != 0 && config.expected_begin_owner != 0 &&
         config.expected_begin_owner_vptr != 0 && begin_identity &&
         config.expected_interval_owner != 0 &&
         config.expected_interval_owner_vptr ==
             config.game_base + config.physics_implementation_vtable_rva &&
         config.expected_player != 0 && config.expected_player_vptr != 0 &&
         config.physics_context_vtable_rva >= 0x1000 &&
         (config.physics_context_vtable_rva & 3u) == 0 &&
         config.physics_implementation_vtable_rva >= 0x1000 &&
         (config.physics_implementation_vtable_rva & 3u) == 0 &&
         config.frame_event_scheduler_return_rva >= 0x1000 &&
         (config.frame_event_scheduler_return_rva & 3u) == 0 &&
         coordinator::QueueValid(config.replay_queue);
}

inline Result Initialize(const Config& config, State* state) noexcept {
  if (state == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  ResetSessionMetadata(state);
  return coordinator::Arm(&state->coordinator, config.session_id,
                          config.generation) == coordinator::Result::kArmed
             ? Result::kObserved
             : Result::kCoreFault;
}

// G8 reuses the coordinator's existing generation and race-end semantics.
// The caller must archive the prior recording receipts before this reset; no
// hook, address or tick state machine is introduced here.
inline Result RearmAfterArchivedRace(const Config& config,
                                     State* state) noexcept {
  if (state == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  if (state->coordinator.lifecycle != coordinator::Lifecycle::kInactive ||
      state->coordinator.tick_phase != coordinator::TickPhase::kClosed) {
    (void)coordinator::Fault(&state->coordinator,
                             coordinator::Result::kWrongLifecycle);
    ++state->core_faults;
    return Result::kCoreFault;
  }
  coordinator::State next = state->coordinator;
  const coordinator::Result armed = coordinator::Arm(
      &next, config.session_id, config.generation);
  if (armed != coordinator::Result::kArmed) {
    state->coordinator = next;
    ++state->core_faults;
    return Result::kCoreFault;
  }
  ResetSessionMetadata(state);
  state->coordinator = next;
  return Result::kObserved;
}

// A manual checkpoint is sealed while the old race is still paused, so the
// coordinator deliberately remains IN_RACE.  If the host later proves that the
// game has entered Retry lifecycle 2, close that old attempt and arm the next
// generation.  The new attempt still owns no tick until the game's real 2 -> 3
// transition is observed.
inline Result RearmAfterSealedCheckpointAtRetry(
    const Config& config, State* state) noexcept {
  if (state == nullptr || !ConfigValid(config) ||
      state->coordinator.lifecycle != coordinator::Lifecycle::kInRace ||
      state->coordinator.tick_phase != coordinator::TickPhase::kClosed)
    return Result::kInvalidArgument;
  coordinator::State next = state->coordinator;
  const std::uint32_t archived_generation = next.generation;
  if (coordinator::BeginRaceEnd(&next, archived_generation, 3u, 2u) !=
          coordinator::Result::kRaceEnding ||
      coordinator::FinishRaceEnd(&next, archived_generation) !=
          coordinator::Result::kInactive) {
    state->coordinator = next;
    ++state->core_faults;
    return Result::kCoreFault;
  }
  if (coordinator::Arm(&next, config.session_id, config.generation) !=
          coordinator::Result::kArmed) {
    state->coordinator = next;
    ++state->core_faults;
    return Result::kCoreFault;
  }
  ResetSessionMetadata(state);
  state->coordinator = next;
  return Result::kObserved;
}

// Source-faithful replay-to-record hand-off.  Upstream disables ActiveBlock at
// the selected target and immediately resumes natural recording in the same
// race.  Android restarts only the local suffix tick numbering; no game object,
// lifecycle value or hook is replaced here.
inline Result RearmAtPausedRace(const Config& config, State* state) noexcept {
  if (state == nullptr || !ConfigValid(config) ||
      state->coordinator.lifecycle != coordinator::Lifecycle::kInRace ||
      state->coordinator.tick_phase != coordinator::TickPhase::kClosed ||
      state->complete == 0)
    return Result::kInvalidArgument;
  coordinator::State next = state->coordinator;
  const coordinator::Result rearmed = coordinator::RearmAtPausedRace(
      &next, config.session_id, config.generation);
  if (rearmed != coordinator::Result::kRaceStarted) {
    state->coordinator = next;
    ++state->core_faults;
    return Result::kCoreFault;
  }
  ResetSessionMetadata(state);
  state->coordinator = next;
  return Result::kObserved;
}

// Converts an in-progress replay into a fresh record suffix at an already
// closed authoritative tick.  Unlike RearmAtPausedRace, the source replay has
// not reached its configured frame limit, so `complete` must still be zero.
// The game lifecycle remains IN_RACE and no physical state is changed; only
// TAS tick ownership restarts at suffix tick zero.
inline Result RearmFromActiveReplayAtClosedBoundary(
    const Config& config, State* state) noexcept {
  if (state == nullptr || !ConfigValid(config) ||
      state->coordinator.lifecycle != coordinator::Lifecycle::kInRace ||
      state->coordinator.tick_phase != coordinator::TickPhase::kClosed ||
      state->complete != 0 || state->coordinator.tick == 0)
    return Result::kInvalidArgument;
  coordinator::State next = state->coordinator;
  const coordinator::Result rearmed = coordinator::RearmAtPausedRace(
      &next, config.session_id, config.generation);
  if (rearmed != coordinator::Result::kRaceStarted) {
    state->coordinator = next;
    ++state->core_faults;
    return Result::kCoreFault;
  }
  ResetSessionMetadata(state);
  state->coordinator = next;
  return Result::kObserved;
}

// Converts a replayed prefix into recording history without restarting its
// clock.  This is the Android equivalent of AluTasV2 disabling ActiveBlock
// while ReplayStateManager continues recording the same race timeline.
inline Result ContinueReplayAsRecordAtClosedBoundary(
    const Config& config, State* state) noexcept {
  if (state == nullptr || !ConfigValid(config) ||
      state->coordinator.lifecycle != coordinator::Lifecycle::kInRace ||
      state->coordinator.tick_phase != coordinator::TickPhase::kClosed ||
      state->coordinator.tick == 0 ||
      state->receipt_count != state->coordinator.tick ||
      (state->complete != 0 && state->complete != 1))
    return Result::kInvalidArgument;

  coordinator::State next = state->coordinator;
  const coordinator::Result continued =
      coordinator::ContinueReplayAsRecordAtClosedBoundary(
          &next, config.session_id, config.generation);
  if (continued != coordinator::Result::kRaceStarted) {
    state->coordinator = next;
    ++state->core_faults;
    return Result::kCoreFault;
  }
  for (std::uint32_t index = 0; index < state->receipt_count; ++index) {
    state->receipts[index].session_id = config.session_id;
    state->receipts[index].generation = config.generation;
    state->receipts[index].selected_packet_index = coordinator::kNoPacket;
  }
  state->coordinator = next;
  state->complete = 0;
  return Result::kObserved;
}

// This observer is called only at a proven closed tick boundary.  The current
// G4/G6 payload does not call it yet; it is the narrow G8 seam for replacing
// fixed-frame completion with the already implemented 3 -> non-3 race end.
inline Result ObserveRaceEndAtClosedBoundary(
    const Config& config, State* state,
    std::uint32_t lifecycle_state) noexcept {
  if (state == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  if (state->coordinator.lifecycle != coordinator::Lifecycle::kInRace) {
    (void)coordinator::Fault(&state->coordinator,
                             coordinator::Result::kWrongLifecycle);
    ++state->core_faults;
    return Result::kCoreFault;
  }
  if (lifecycle_state == 3u) return Result::kWaitingForTick;
  if (state->coordinator.tick_phase != coordinator::TickPhase::kClosed) {
    (void)coordinator::Fault(&state->coordinator,
                             coordinator::Result::kWrongOrder);
    ++state->core_faults;
    return Result::kCoreFault;
  }
  const coordinator::Result ending = coordinator::BeginRaceEnd(
      &state->coordinator, config.generation, 3u, lifecycle_state);
  if (ending != coordinator::Result::kRaceEnding) {
    ++state->core_faults;
    return Result::kCoreFault;
  }
  const coordinator::Result ended = coordinator::FinishRaceEnd(
      &state->coordinator, config.generation);
  if (ended != coordinator::Result::kInactive) {
    ++state->core_faults;
    return Result::kCoreFault;
  }
  ++state->inferred_race_ends;
  return Result::kRaceEnded;
}

inline Result DiscardOpenTickForRaceEnd(const Config& config,
                                        State* state) noexcept {
  if (state == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  const coordinator::TickScratch scratch = state->coordinator.scratch;
  const coordinator::Result discarded = coordinator::DiscardOpenTickForRaceEnd(
      &state->coordinator, config.generation);
  if (discarded != coordinator::Result::kTickDiscarded) {
    ++state->core_faults;
    return Result::kCoreFault;
  }
  if (state->tick_begin_entries == 0 ||
      state->physics_interval_calls < scratch.physics_interval_calls) {
    ++state->core_faults;
    return Result::kCoreFault;
  }
  --state->tick_begin_entries;
  state->physics_interval_calls -= scratch.physics_interval_calls;
  if ((scratch.boundary_flags & coordinator::kBoundaryPrePhysics) != 0) {
    if (state->pre_physics_entries == 0) {
      ++state->core_faults;
      return Result::kCoreFault;
    }
    --state->pre_physics_entries;
  }
  if ((scratch.boundary_flags & coordinator::kBoundaryFinalWriter) != 0) {
    if (state->final_writer_returns == 0) {
      ++state->core_faults;
      return Result::kCoreFault;
    }
    --state->final_writer_returns;
  }
  return Result::kObserved;
}

inline Result Core(State* state, coordinator::Result result) noexcept {
  if (static_cast<std::int32_t>(result) > 0) return Result::kObserved;
  ++state->core_faults;
  return Result::kCoreFault;
}

inline Result Ignore(State* state) noexcept {
  ++state->ignored_unqualified;
  return Result::kIgnoredUnqualified;
}

inline Result ObserveTickBeginAtSubmit(
    const Config& config, State* state, std::uintptr_t owner,
    std::uintptr_t owner_vptr, std::uint32_t lifecycle_state,
    std::uint32_t tid) noexcept {
  if (state == nullptr || !ConfigValid(config) || tid == 0)
    return Result::kInvalidArgument;
  if (owner != config.expected_begin_owner ||
      owner_vptr != config.expected_begin_owner_vptr)
    return Ignore(state);

  if (state->bound_begin_owner == 0) {
    state->bound_begin_owner = owner;
    state->bound_begin_owner_vptr = owner_vptr;
  } else if (owner != state->bound_begin_owner ||
             owner_vptr != state->bound_begin_owner_vptr) {
    return Ignore(state);
  }

  if (state->complete != 0) return Result::kComplete;
  if (state->coordinator.lifecycle == coordinator::Lifecycle::kArmed) {
    // The host arms while the countdown is frozen in phase 2. Token submission
    // is already active during that phase, so those natural calls are pre-race
    // observations and must never consume recording packet 0. The exact-build
    // lifecycle owner has a live-proven unique 2 -> 3 transition; the first
    // qualified submit that observes phase 3 owns Android tick 0.
    if (lifecycle_state == 2u) return Result::kWaitingForRace;
    if (lifecycle_state != 3u)
      return Core(state, coordinator::Fault(
                             &state->coordinator,
                             coordinator::Result::kWrongTransition));
    const coordinator::Result started = coordinator::OnRaceStart(
        &state->coordinator, config.generation, 2u, 3u);
    if (started != coordinator::Result::kRaceStarted)
      return Core(state, started);
    ++state->inferred_race_starts;
  }
  if (state->coordinator.lifecycle != coordinator::Lifecycle::kInRace)
    return Core(state, coordinator::Fault(
                           &state->coordinator,
                           coordinator::Result::kWrongLifecycle));
  if (lifecycle_state != 3u)
    return Core(state, coordinator::Fault(
                           &state->coordinator,
                           coordinator::Result::kWrongTransition));

  if (state->coordinator.tick_phase != coordinator::TickPhase::kClosed)
    return Core(state, coordinator::Fault(
                           &state->coordinator,
                           coordinator::Result::kWrongOrder));
  const coordinator::Result begun = coordinator::BeginTick(
      &state->coordinator, config.generation, tid, false,
      config.replay_queue);
  if (static_cast<std::int32_t>(begun) <= 0) return Core(state, begun);
  ++state->tick_begin_entries;
  return Result::kObserved;
}

inline Result ObservePhysicsInterval(
    const Config& config, State* state, std::uintptr_t owner,
    std::uintptr_t owner_vptr, std::uint32_t lifecycle_state,
    std::uint32_t tid) noexcept {
  if (state == nullptr || !ConfigValid(config) || tid == 0)
    return Result::kInvalidArgument;
  if (owner != config.expected_interval_owner ||
      owner_vptr != config.expected_interval_owner_vptr)
    return Ignore(state);
  if (state->complete != 0) return Result::kComplete;
  if (state->coordinator.lifecycle == coordinator::Lifecycle::kArmed)
    return lifecycle_state == 2u ? Result::kWaitingForRace
                                 : Result::kWaitingForTick;
  if (state->coordinator.lifecycle != coordinator::Lifecycle::kInRace)
    return Core(state, coordinator::Fault(
                           &state->coordinator,
                           coordinator::Result::kWrongLifecycle));
  if (lifecycle_state != 3u)
    return Core(state, coordinator::Fault(
                           &state->coordinator,
                           coordinator::Result::kWrongTransition));
  if (state->coordinator.tick_phase == coordinator::TickPhase::kClosed)
    return Result::kWaitingForTick;
  const coordinator::TickPhase before = state->coordinator.tick_phase;
  const coordinator::Result interval = coordinator::OnPrePhysics(
      &state->coordinator, config.generation, state->coordinator.tick, tid);
  if (static_cast<std::int32_t>(interval) <= 0)
    return Core(state, interval);
  ++state->physics_interval_calls;
  if (before == coordinator::TickPhase::kBegun)
    ++state->pre_physics_entries;
  return Result::kObserved;
}

inline Result ObserveTickBeginAndPrePhysics(
    const Config& config, State* state, std::uintptr_t owner,
    std::uintptr_t owner_vptr, std::uint32_t lifecycle_state,
    std::uint32_t tid) noexcept {
  // Frozen G3 compatibility path.  New G4/G5 runtime code must use the split
  // submit and interval observations above.
  if (config.expected_begin_owner != config.expected_interval_owner ||
      config.expected_begin_owner_vptr !=
          config.expected_interval_owner_vptr)
    return Result::kInvalidArgument;
  if (state != nullptr &&
      state->coordinator.tick_phase == coordinator::TickPhase::kClosed) {
    const Result begun = ObserveTickBeginAtSubmit(
        config, state, owner, owner_vptr, lifecycle_state, tid);
    if (begun != Result::kObserved) return begun;
  }
  return ObservePhysicsInterval(config, state, owner, owner_vptr,
                                lifecycle_state, tid);
}

inline Result ObserveFinalWriterReturn(const Config& config, State* state,
                                       std::uintptr_t player,
                                       std::uintptr_t player_vptr,
                                       std::uint32_t tid,
                                       bool completed_without_integration = false) noexcept {
  if (state == nullptr || !ConfigValid(config) || tid == 0)
    return Result::kInvalidArgument;
  if (player != config.expected_player ||
      player_vptr != config.expected_player_vptr)
    return Ignore(state);
  ++state->final_writer_returns;
  if (state->complete != 0) return Result::kComplete;
  if (state->coordinator.lifecycle == coordinator::Lifecycle::kArmed)
    return Result::kWaitingForRace;
  // A player update can return after Submit opened the tick but before the
  // first qualified physics interval. It is not this tick's physics-final
  // boundary: leave the tick open and wait for a post-interval return. Never
  // manufacture an interval or publish/correct a frame from this callback.
  if (state->coordinator.lifecycle == coordinator::Lifecycle::kInRace &&
      state->coordinator.tick_phase == coordinator::TickPhase::kBegun &&
      !completed_without_integration) {
    ++state->idle_final_writer_returns;
    return Result::kWaitingForTick;
  }
  if (state->coordinator.tick_phase == coordinator::TickPhase::kClosed) {
    ++state->idle_final_writer_returns;
    return Result::kWaitingForTick;
  }
  return Core(state, coordinator::OnFinalWriter(
                         &state->coordinator, config.generation,
                         state->coordinator.tick, tid,
                         completed_without_integration));
}

inline Result ObserveFrameEventReturn(const Config& config, State* state,
                                      std::uintptr_t caller_return,
                                      std::uint32_t tid) noexcept {
  if (state == nullptr || !ConfigValid(config) || tid == 0)
    return Result::kInvalidArgument;
  if (caller_return !=
      config.game_base + config.frame_event_scheduler_return_rva)
    return Ignore(state);
  ++state->frame_event_returns;
  if (state->complete != 0) return Result::kComplete;
  if (state->coordinator.lifecycle == coordinator::Lifecycle::kArmed)
    return Result::kWaitingForRace;
  if (state->coordinator.tick_phase == coordinator::TickPhase::kClosed) {
    ++state->idle_frame_event_returns;
    return Result::kWaitingForTick;
  }
  if (state->receipt_count >= config.frame_limit ||
      state->receipt_count >= kMaximumNeutralReceipts)
    return Core(state, coordinator::Fault(
                           &state->coordinator,
                           coordinator::Result::kInvalidArgument));
  coordinator::TickScratch* receipt = &state->receipts[state->receipt_count];
  const coordinator::Result ended = coordinator::OnTickEnd(
      &state->coordinator, config.generation, state->coordinator.tick, tid,
      receipt);
  if (ended != coordinator::Result::kTickPublished)
    return Core(state, ended);
  ++state->receipt_count;
  if (state->receipt_count == config.frame_limit) {
    state->complete = 1;
    return Result::kComplete;
  }
  return Result::kObserved;
}

}  // namespace a9tas::g3_boundary_adapter_v1
