#include "g3_boundary_adapter_v1.h"

namespace adapter = a9tas::g3_boundary_adapter_v1;
namespace coordinator = a9tas::g3_tick_coordinator_v1;

namespace {

constexpr adapter::Config Config() {
  constexpr std::uintptr_t kBase = 0x700000000000u;
  constexpr std::uintptr_t kOwner = 0x1000;
  constexpr std::uintptr_t kVptr =
      kBase + adapter::kPhysicsImplementationVtableRva;
  return {.session_id = 0x11223344u,
          .generation = 7,
          .frame_limit = 5,
          .game_base = kBase,
          .expected_begin_owner = kOwner,
          .expected_begin_owner_vptr = kVptr,
          .expected_interval_owner = kOwner,
          .expected_interval_owner_vptr = kVptr,
          .expected_player = 0x5000,
          .expected_player_vptr = 0x6000};
}

constexpr std::uintptr_t TickVptr(const adapter::Config& config) {
  return config.game_base + adapter::kPhysicsImplementationVtableRva;
}

bool FiveNeutralCycles() {
  const adapter::Config config = Config();
  adapter::State state{};
  if (adapter::Initialize(config, &state) != adapter::Result::kObserved)
    return false;
  if (adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 2, 10) !=
          adapter::Result::kWaitingForRace ||
      state.coordinator.lifecycle != coordinator::Lifecycle::kArmed ||
      state.tick_begin_entries != 0 || state.physics_interval_calls != 0)
    return false;
  for (std::uint64_t tick = 0; tick < 5; ++tick) {
    if (adapter::ObserveTickBeginAndPrePhysics(
            config, &state, 0x1000, TickVptr(config), 3, 10) !=
            adapter::Result::kObserved ||
        adapter::ObserveFinalWriterReturn(config, &state, 0x5000, 0x6000,
                                          30) != adapter::Result::kObserved)
      return false;
    const adapter::Result end = adapter::ObserveFrameEventReturn(
        config, &state,
        config.game_base + adapter::kFrameEventSchedulerReturnRva, 40);
    if (end != (tick == 4 ? adapter::Result::kComplete
                          : adapter::Result::kObserved) ||
        state.receipts[tick].tick != tick ||
        state.receipts[tick].selected_packet_index != coordinator::kNoPacket ||
        state.receipts[tick].begin_tid != 10 ||
        state.receipts[tick].pre_physics_tid != 10)
      return false;
  }
  return state.complete == 1 && state.receipt_count == 5 &&
         state.tick_begin_entries == 5 && state.pre_physics_entries == 5 &&
         state.physics_interval_calls == 5 &&
         state.final_writer_returns == 5 && state.frame_event_returns == 5 &&
         state.inferred_race_starts == 1 && state.core_faults == 0 &&
         state.coordinator.ticks_published == 5;
}

bool RepeatedPhysicsIntervalsStayInOneTick() {
  adapter::Config config = Config();
  config.frame_limit = 1;
  adapter::State state{};
  if (adapter::Initialize(config, &state) != adapter::Result::kObserved ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 3, 10) !=
          adapter::Result::kObserved ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 3, 10) !=
          adapter::Result::kObserved ||
      adapter::ObserveFinalWriterReturn(config, &state, 0x5000, 0x6000,
                                        30) != adapter::Result::kObserved ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 3, 10) !=
          adapter::Result::kObserved ||
      adapter::ObserveFrameEventReturn(
          config, &state,
          config.game_base + adapter::kFrameEventSchedulerReturnRva,
          40) != adapter::Result::kComplete)
    return false;
  return state.receipt_count == 1 && state.coordinator.tick == 1 &&
         state.tick_begin_entries == 1 && state.pre_physics_entries == 1 &&
         state.physics_interval_calls == 3 &&
         state.coordinator.physics_interval_calls == 3 &&
         state.receipts[0].physics_interval_calls == 3 &&
         state.receipts[0].boundary_flags ==
             coordinator::kCompleteTickBoundaryMask &&
         state.core_faults == 0 && state.coordinator.failures == 0;
}

bool IdleTailCallbacksDoNotCreateOrFaultATick() {
  adapter::Config config = Config();
  config.frame_limit = 2;
  adapter::State state{};
  if (adapter::Initialize(config, &state) != adapter::Result::kObserved ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 3, 10) !=
          adapter::Result::kObserved ||
      adapter::ObserveFinalWriterReturn(config, &state, 0x5000, 0x6000,
                                        30) != adapter::Result::kObserved ||
      adapter::ObserveFrameEventReturn(
          config, &state,
          config.game_base + adapter::kFrameEventSchedulerReturnRva,
          40) != adapter::Result::kObserved ||
      adapter::ObserveFinalWriterReturn(config, &state, 0x5000, 0x6000,
                                        30) !=
          adapter::Result::kWaitingForTick ||
      adapter::ObserveFrameEventReturn(
          config, &state,
          config.game_base + adapter::kFrameEventSchedulerReturnRva,
          40) != adapter::Result::kWaitingForTick)
    return false;
  return state.receipt_count == 1 && state.coordinator.tick == 1 &&
         state.coordinator.tick_phase == coordinator::TickPhase::kClosed &&
         state.idle_final_writer_returns == 1 &&
         state.idle_frame_event_returns == 1 && state.core_faults == 0 &&
         state.coordinator.failures == 0;
}

bool UnqualifiedNoiseDoesNotAdvance() {
  const adapter::Config config = Config();
  adapter::State state{};
  if (adapter::Initialize(config, &state) != adapter::Result::kObserved)
    return false;
  if (adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1001, TickVptr(config), 3, 10) !=
          adapter::Result::kIgnoredUnqualified ||
      adapter::ObserveFinalWriterReturn(config, &state, 0x5001, 0x6000, 30) !=
          adapter::Result::kIgnoredUnqualified ||
      adapter::ObserveFrameEventReturn(config, &state,
                                       config.game_base + 1, 40) !=
          adapter::Result::kIgnoredUnqualified)
    return false;
  return state.ignored_unqualified == 3 &&
         state.coordinator.lifecycle == coordinator::Lifecycle::kArmed &&
         state.receipt_count == 0;
}

bool PreRaceTailIsWaiting() {
  const adapter::Config config = Config();
  adapter::State state{};
  if (adapter::Initialize(config, &state) != adapter::Result::kObserved)
    return false;
  return adapter::ObserveFinalWriterReturn(config, &state, 0x5000, 0x6000,
                                           30) ==
             adapter::Result::kWaitingForRace &&
         adapter::ObserveFrameEventReturn(
             config, &state,
             config.game_base + adapter::kFrameEventSchedulerReturnRva,
             40) == adapter::Result::kWaitingForRace &&
         state.coordinator.lifecycle == coordinator::Lifecycle::kArmed &&
         state.receipt_count == 0 && state.core_faults == 0;
}

bool QualifiedOrderErrorFaults() {
  const adapter::Config config = Config();
  adapter::State state{};
  if (adapter::Initialize(config, &state) != adapter::Result::kObserved ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 3, 10) !=
          adapter::Result::kObserved)
    return false;
  return adapter::ObserveFrameEventReturn(
             config, &state,
             config.game_base + adapter::kFrameEventSchedulerReturnRva,
             40) == adapter::Result::kCoreFault &&
         state.core_faults == 1 &&
         state.coordinator.lifecycle == coordinator::Lifecycle::kFaulted;
}

bool ExactPhysicsOwnerBinding() {
  const adapter::Config config = Config();
  adapter::State state{};
  if (adapter::Initialize(config, &state) != adapter::Result::kObserved)
    return false;
  if (adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1001, TickVptr(config), 2, 10) !=
          adapter::Result::kIgnoredUnqualified ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config) + 8, 2, 10) !=
          adapter::Result::kIgnoredUnqualified ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 2, 10) !=
          adapter::Result::kWaitingForRace ||
      state.coordinator.lifecycle != coordinator::Lifecycle::kArmed ||
      state.inferred_race_starts != 0 ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 3, 10) !=
          adapter::Result::kObserved ||
      state.bound_begin_owner != 0x1000 ||
      state.bound_begin_owner_vptr != TickVptr(config) ||
      state.coordinator.lifecycle != coordinator::Lifecycle::kInRace ||
      state.inferred_race_starts != 1)
    return false;
  adapter::Config invalid = config;
  invalid.expected_interval_owner = 0x2000;
  adapter::State invalid_state{};
  return adapter::Initialize(invalid, &invalid_state) ==
             adapter::Result::kInvalidArgument &&
         state.core_faults == 0;
}

bool RaceEndArchiveAndRetryGeneration() {
  adapter::Config config = Config();
  adapter::State state{};
  if (adapter::Initialize(config, &state) != adapter::Result::kObserved ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 3, 10) !=
          adapter::Result::kObserved ||
      adapter::ObserveFinalWriterReturn(config, &state, 0x5000, 0x6000,
                                        30) != adapter::Result::kObserved ||
      adapter::ObserveFrameEventReturn(
          config, &state,
          config.game_base + adapter::kFrameEventSchedulerReturnRva,
          40) != adapter::Result::kObserved ||
      adapter::ObserveRaceEndAtClosedBoundary(config, &state, 2) !=
          adapter::Result::kRaceEnded ||
      state.coordinator.lifecycle != coordinator::Lifecycle::kInactive ||
      state.coordinator.race_ends != 1 || state.inferred_race_ends != 1 ||
      state.receipt_count != 1)
    return false;

  // Reusing generation 7 must fail closed even after a clean race end.
  adapter::State stale = state;
  if (adapter::RearmAfterArchivedRace(config, &stale) !=
          adapter::Result::kCoreFault ||
      stale.coordinator.lifecycle != coordinator::Lifecycle::kFaulted)
    return false;

  // The caller archives the old receipt before rearming.  Generation 8 then
  // waits in phase 2 and owns tick 0 only on the next 2 -> 3 transition.
  config.session_id = 0x55667788u;
  config.generation = 8;
  if (adapter::RearmAfterArchivedRace(config, &state) !=
          adapter::Result::kObserved ||
      state.coordinator.lifecycle != coordinator::Lifecycle::kArmed ||
      state.coordinator.generation != 8 || state.receipt_count != 0 ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 2, 10) !=
          adapter::Result::kWaitingForRace ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 3, 10) !=
          adapter::Result::kObserved)
    return false;
  return state.coordinator.lifecycle == coordinator::Lifecycle::kInRace &&
         state.coordinator.tick == 0 && state.coordinator.generation == 8 &&
         state.coordinator.race_starts == 2 &&
         state.coordinator.race_ends == 1 && state.inferred_race_starts == 1;
}

bool ManualCheckpointRetryGeneration() {
  adapter::Config config = Config();
  adapter::State state{};
  if (adapter::Initialize(config, &state) != adapter::Result::kObserved ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 3, 10) !=
          adapter::Result::kObserved ||
      adapter::ObserveFinalWriterReturn(config, &state, 0x5000, 0x6000,
                                        30) != adapter::Result::kObserved ||
      adapter::ObserveFrameEventReturn(
          config, &state,
          config.game_base + adapter::kFrameEventSchedulerReturnRva,
          40) != adapter::Result::kObserved ||
      state.coordinator.lifecycle != coordinator::Lifecycle::kInRace ||
      state.coordinator.tick_phase != coordinator::TickPhase::kClosed)
    return false;
  config.session_id = 0x778899u;
  config.generation = 8;
  if (adapter::RearmAfterSealedCheckpointAtRetry(config, &state) !=
          adapter::Result::kObserved ||
      state.coordinator.lifecycle != coordinator::Lifecycle::kArmed ||
      state.coordinator.generation != 8 || state.receipt_count != 0 ||
      state.coordinator.race_ends != 1 ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 2, 10) !=
          adapter::Result::kWaitingForRace ||
      adapter::ObserveTickBeginAndPrePhysics(
          config, &state, 0x1000, TickVptr(config), 3, 10) !=
          adapter::Result::kObserved)
    return false;
  return state.coordinator.lifecycle == coordinator::Lifecycle::kInRace &&
      state.coordinator.tick == 0 && state.coordinator.race_starts == 2 &&
      state.coordinator.race_ends == 1;
}

}  // namespace

int main() {
  return FiveNeutralCycles() && UnqualifiedNoiseDoesNotAdvance() &&
                 PreRaceTailIsWaiting() && QualifiedOrderErrorFaults() &&
                  ExactPhysicsOwnerBinding() &&
                  RaceEndArchiveAndRetryGeneration() &&
                  ManualCheckpointRetryGeneration() &&
                  RepeatedPhysicsIntervalsStayInOneTick() &&
                 IdleTailCallbacksDoNotCreateOrFaultATick()
             ? 0
             : 1;
}
