#include "g4_g3_adapter_v1.h"

#include <cstdio>

namespace bridge = a9tas::g4_g3_adapter_v1;
namespace g3 = a9tas::g3_boundary_adapter_v1;
namespace g4 = a9tas::g4_input_action_v1;
namespace recording = a9tas::unified_tick_v1;

namespace {

recording::RecordingFrameV1 Packet(std::uint64_t tick, float steer,
                                   float brake, std::uint32_t nitro) {
  recording::RecordingFrameV1 packet{};
  packet.tick = tick;
  packet.steering = steer;
  packet.brake = brake;
  packet.accelerator = 0.0f;
  packet.nitro_activation_count = nitro;
  packet.flags = recording::kRequiredFrameFlags;
  return packet;
}

bool RunTwoTicks() {
  const std::uintptr_t base = 0x10000000u;
  const std::uintptr_t physics = 0x20000000u;
  const std::uintptr_t context = 0x21000000u;
  const std::uintptr_t player = 0x30000000u;
  const recording::RecordingFrameV1 packets[] = {
      Packet(0, 0.5f, -1.0f, 2),
      Packet(1, -0.25f, 0.0f, 0),
  };
  const g4::IntervalSampleV1 intervals[] = {
      {0, 0, g4::FloatBits(1.0f / 60.0f)},
      {0, 1, g4::FloatBits(1.0f / 120.0f)},
      {1, 0, g4::FloatBits(1.0f / 60.0f)},
  };
  bridge::ConfigV1 config{};
  config.tick.session_id = 11;
  config.tick.generation = 1;
  config.tick.frame_limit = 2;
  config.tick.game_base = base;
  config.tick.expected_begin_owner = context;
  config.tick.expected_begin_owner_vptr =
      base + g3::kPhysicsContextVtableRva;
  config.tick.expected_interval_owner = physics;
  config.tick.expected_interval_owner_vptr =
      base + g3::kPhysicsImplementationVtableRva;
  config.tick.expected_player = player;
  config.tick.expected_player_vptr = base + 0x777000;
  config.tick.replay_queue = {
      .mode = a9tas::g3_tick_coordinator_v1::ReplayMode::kActiveBlock,
      .communication_version_matches = true,
      .block_mode_still_active = true,
      .packets = packets,
      .packet_count = 2,
  };
  config.fixed_delta_us = 16667;
  config.interval_replay = {intervals, 3};

  bridge::StateV1 state{};
  if (bridge::Initialize(config, &state) != bridge::Result::kObserved)
    return false;
  g4::SetterDecisionV1 setter{};
  if (bridge::ObserveSteeringBeforeOriginal(
          &state, g4::FloatBits(0.125f), &setter) !=
          bridge::Result::kObserved || setter.tick_open ||
      bridge::ObserveBrakeBeforeOriginal(
          &state, g4::FloatBits(0.0f), &setter) !=
          bridge::Result::kObserved || setter.tick_open)
    return false;
  const std::uint64_t natural =
      g4::CurrentControlPair(state.input_action);

  // A call through the shared Physics Interval entry from another object must
  // not be paired with the after-original interval stream.
  bridge::IntervalBeforeReceiptV1 ignored_before{};
  const bridge::Result ignored_result = bridge::ObserveIntervalBeforeOriginal(
      config, &state, physics + 0x1000,
      base + g3::kPhysicsImplementationVtableRva, 2, 70, natural,
      &ignored_before);
  if (ignored_result != bridge::Result::kIgnored ||
      bridge::IntervalAfterQualified(ignored_result) ||
      state.input_action.tick_open || state.input_action.interval_ordinal != 0)
    return false;
  if (!bridge::IntervalAfterQualified(bridge::Result::kObserved) ||
      !bridge::IntervalAfterQualified(bridge::Result::kRepeatedInterval) ||
      bridge::IntervalAfterQualified(bridge::Result::kComplete) ||
      bridge::IntervalAfterQualified(bridge::Result::kG3Fault))
    return false;

  // Real game frame/final callbacks can arrive after arm and before the first
  // Physics Interval. They are idle G3 observations, not G4 EndTick faults.
  if (bridge::ObserveFinalWriterReturn(
          config, &state, player, base + 0x777000, 71) !=
          bridge::Result::kIgnored ||
      bridge::ObserveTickEndReturn(
          config, &state, base + g3::kFrameEventSchedulerReturnRva, 72) !=
          bridge::Result::kIgnored ||
      state.input_action.tick_open || state.receipt_count != 0)
    return false;

  bridge::SubmitBeforeReceiptV1 submit{};
  if (bridge::ObservePhysicsSubmitBeforeOriginal(
          config, &state, context,
          base + g3::kPhysicsContextVtableRva, 2, 71, natural,
          &submit) != bridge::Result::kIgnored ||
      state.input_action.tick_open || state.receipt_count != 0 ||
      state.tick.coordinator.lifecycle !=
          a9tas::g3_tick_coordinator_v1::Lifecycle::kArmed)
    return false;
  if (bridge::ObservePhysicsSubmitBeforeOriginal(
          config, &state, context,
          base + g3::kPhysicsContextVtableRva, 3, 71, natural,
          &submit) != bridge::Result::kObserved ||
      !submit.first_call_in_tick || submit.decision.replay_nitro_calls != 2)
    return false;
  // Reproduce error=13 / hook=FinalWriter / phase=Begun / intervals=0.
  // Early player returns must not capture/correct state or consume tick zero.
  for (unsigned repeat = 0; repeat < 3; ++repeat) {
    if (bridge::ObserveFinalWriterReturn(
            config, &state, player, config.tick.expected_player_vptr, 72) !=
            bridge::Result::kIgnored ||
        !state.input_action.tick_open || state.receipt_count != 0 ||
        state.tick.coordinator.tick != 0 ||
        state.tick.coordinator.physics_interval_calls != 0 ||
        state.tick.coordinator.failures != 0 ||
        state.tick.coordinator.tick_phase !=
            a9tas::g3_tick_coordinator_v1::TickPhase::kBegun)
      return false;
  }
  bridge::IntervalBeforeReceiptV1 before{};
  if (bridge::ObserveIntervalBeforeOriginal(
          config, &state, physics,
          base + g3::kPhysicsImplementationVtableRva, 3, 71, natural,
          &before) != bridge::Result::kObserved ||
      !before.first_call_in_tick)
    return false;
  if (bridge::ObserveSteeringBeforeOriginal(
          &state, g4::FloatBits(0.125f), &setter) !=
          bridge::Result::kObserved || !setter.replay_override ||
      setter.final_bits != g4::FloatBits(0.5f) ||
      bridge::ObserveBrakeBeforeOriginal(
          &state, g4::FloatBits(0.0f), &setter) !=
          bridge::Result::kObserved || !setter.replay_override ||
      setter.final_bits != g4::FloatBits(-1.0f))
    return false;
  std::uint32_t accelerator = 0;
  if (bridge::ObserveAcceleratorAfterOriginal(
          &state, g4::FloatBits(0.75f), &accelerator) !=
          bridge::Result::kObserved ||
      accelerator != g4::FloatBits(0.0f))
    return false;
  g4::IntervalDecisionV1 interval{};
  if (bridge::ObserveIntervalAfterOriginal(
          config, &state, g4::FloatBits(1.0f / 30.0f), &interval) !=
          bridge::Result::kObserved ||
      interval.final_output_bits != intervals[0].output_bits)
    return false;
  if (bridge::AccountInjectedNitroCalls(&state, 2) !=
      bridge::Result::kObserved)
    return false;
  bool call_original = true;
  if (bridge::ObserveNaturalNitroCall(&state, &call_original) !=
          bridge::Result::kObserved ||
      call_original)
    return false;

  if (bridge::ObserveIntervalBeforeOriginal(
          config, &state, physics,
          base + g3::kPhysicsImplementationVtableRva, 3, 71, natural,
          &before) != bridge::Result::kRepeatedInterval ||
      bridge::ObserveIntervalAfterOriginal(
          config, &state, g4::FloatBits(1.0f / 30.0f), &interval) !=
          bridge::Result::kObserved ||
      interval.final_output_bits != intervals[1].output_bits)
    return false;
  if (bridge::ObserveFinalWriterReturn(
          config, &state, player, base + 0x777000, 71) !=
      bridge::Result::kObserved)
    return false;
  if (bridge::ObserveTickEndReturn(
          config, &state, base + g3::kFrameEventSchedulerReturnRva, 72) !=
      bridge::Result::kObserved)
    return false;

  if (bridge::ObservePhysicsSubmitBeforeOriginal(
          config, &state, context,
          base + g3::kPhysicsContextVtableRva, 3, 71, natural,
          &submit) != bridge::Result::kObserved ||
      bridge::ObserveIntervalBeforeOriginal(
          config, &state, physics,
          base + g3::kPhysicsImplementationVtableRva, 3, 71, natural,
          &before) != bridge::Result::kObserved ||
      bridge::ObserveIntervalAfterOriginal(
          config, &state, g4::FloatBits(1.0f / 30.0f), &interval) !=
          bridge::Result::kObserved ||
      interval.final_output_bits != intervals[2].output_bits ||
      bridge::AccountInjectedNitroCalls(&state, 0) !=
          bridge::Result::kObserved ||
      bridge::ObserveFinalWriterReturn(
          config, &state, player, base + 0x777000, 71) !=
          bridge::Result::kObserved ||
      bridge::ObserveTickEndReturn(
          config, &state, base + g3::kFrameEventSchedulerReturnRva, 72) !=
          bridge::Result::kComplete)
    return false;

  return state.receipt_count == 2 &&
         state.tick.coordinator.tick == 2 &&
         state.tick.coordinator.packet_selections == 2 &&
         state.tick.coordinator.physics_interval_calls == 3 &&
         state.receipts[0].physics_interval_calls == 2 &&
         state.receipts[0].steering_bits == g4::FloatBits(0.5f) &&
         state.receipts[0].brake_bits == g4::FloatBits(-1.0f) &&
         state.receipts[0].accelerator_observed &&
         state.receipts[0].injected_nitro_calls == 2 &&
         state.receipts[0].suppressed_nitro_calls == 1 &&
         state.receipts[1].physics_interval_calls == 1 &&
         state.input_action.interval_cursor == 3;
}

bool LifecycleCompletionAfterActualTick() {
  const std::uintptr_t base = 0x40000000u;
  const std::uintptr_t physics = 0x50000000u;
  const std::uintptr_t context = 0x51000000u;
  const std::uintptr_t player = 0x60000000u;
  bridge::ConfigV1 config{};
  config.tick.session_id = 22;
  config.tick.generation = 4;
  config.tick.frame_limit = 3;
  config.tick.game_base = base;
  config.tick.expected_begin_owner = context;
  config.tick.expected_begin_owner_vptr =
      base + g3::kPhysicsContextVtableRva;
  config.tick.expected_interval_owner = physics;
  config.tick.expected_interval_owner_vptr =
      base + g3::kPhysicsImplementationVtableRva;
  config.tick.expected_player = player;
  config.tick.expected_player_vptr = base + 0x888000;
  config.fixed_delta_us = 16667;

  bridge::StateV1 state{};
  bridge::SubmitBeforeReceiptV1 submit{};
  bridge::IntervalBeforeReceiptV1 interval_before{};
  g4::IntervalDecisionV1 interval_after{};
  g4::SetterDecisionV1 setter{};
  if (bridge::Initialize(config, &state) != bridge::Result::kObserved)
    return false;
  const std::uint64_t natural = g4::CurrentControlPair(state.input_action);
  if (
      bridge::ObservePhysicsSubmitBeforeOriginal(
          config, &state, context,
          base + g3::kPhysicsContextVtableRva, 3, 80, natural,
          &submit) != bridge::Result::kObserved ||
      bridge::ObserveSteeringBeforeOriginal(
          &state, g4::FloatBits(0.0f), &setter) !=
          bridge::Result::kObserved ||
      bridge::ObserveBrakeBeforeOriginal(
          &state, g4::FloatBits(0.0f), &setter) !=
          bridge::Result::kObserved ||
      bridge::ObserveIntervalBeforeOriginal(
          config, &state, physics,
          base + g3::kPhysicsImplementationVtableRva, 3, 80, natural,
          &interval_before) != bridge::Result::kObserved ||
      bridge::ObserveIntervalAfterOriginal(
          config, &state, g4::FloatBits(1.0f / 60.0f),
          &interval_after) != bridge::Result::kObserved ||
      bridge::AccountInjectedNitroCalls(&state, 0) !=
          bridge::Result::kObserved ||
      bridge::ObserveFinalWriterReturn(
          config, &state, player, base + 0x888000, 80) !=
          bridge::Result::kObserved ||
      bridge::ObserveTickEndReturn(
          config, &state, base + g3::kFrameEventSchedulerReturnRva,
          81) != bridge::Result::kObserved ||
      bridge::ObserveRaceEndAtClosedBoundary(config, &state, 3) !=
          bridge::Result::kIgnored ||
      // Retry can cut the next real tick after begin/pre-physics but before
      // Final Writer. That incomplete tail must be discarded, not published
      // and not diagnosed as an ordering fault.
      bridge::ObservePhysicsSubmitBeforeOriginal(
          config, &state, context,
          base + g3::kPhysicsContextVtableRva, 3, 80, natural,
          &submit) != bridge::Result::kObserved ||
      bridge::ObserveIntervalBeforeOriginal(
          config, &state, physics,
          base + g3::kPhysicsImplementationVtableRva, 3, 80, natural,
          &interval_before) != bridge::Result::kObserved ||
      bridge::ObserveIntervalAfterOriginal(
          config, &state, g4::FloatBits(1.0f / 60.0f),
          &interval_after) != bridge::Result::kObserved ||
      bridge::DiscardOpenTickForRaceEnd(config, &state) !=
          bridge::Result::kObserved ||
      bridge::ObserveRaceEndAtClosedBoundary(config, &state, 2) !=
          bridge::Result::kRaceEnded)
    return false;
  return state.receipt_count == 1 && !state.input_action.tick_open &&
         state.tick.complete == 0 &&
         state.tick.coordinator.lifecycle ==
             a9tas::g3_tick_coordinator_v1::Lifecycle::kInactive &&
         state.tick.coordinator.ticks_published == 1 &&
         state.tick.coordinator.ticks_begun == 1 &&
         state.tick.coordinator.pre_physics_events == 1 &&
         state.tick.coordinator.physics_interval_calls == 1 &&
         state.tick.coordinator.race_ends == 1;
}

bool ArchivedRaceRearmsAtAuthoritativeTickZero() {
  const std::uintptr_t base = 0x70000000u;
  const std::uintptr_t physics = 0x71000000u;
  const std::uintptr_t context = 0x72000000u;
  const std::uintptr_t player = 0x73000000u;
  const recording::RecordingFrameV1 packet = Packet(0, 0.25f, 0.0f, 0);
  bridge::ConfigV1 first{};
  first.tick.session_id = 31;
  first.tick.generation = 7;
  first.tick.frame_limit = 1;
  first.tick.game_base = base;
  first.tick.expected_begin_owner = context;
  first.tick.expected_begin_owner_vptr =
      base + g3::kPhysicsContextVtableRva;
  first.tick.expected_interval_owner = physics;
  first.tick.expected_interval_owner_vptr =
      base + g3::kPhysicsImplementationVtableRva;
  first.tick.expected_player = player;
  first.tick.expected_player_vptr = base + 0x999000;
  first.fixed_delta_us = 16667;
  bridge::StateV1 state{};
  if (bridge::Initialize(first, &state) != bridge::Result::kObserved)
    return false;
  bridge::SubmitBeforeReceiptV1 submit{};
  bridge::IntervalBeforeReceiptV1 before{};
  g4::IntervalDecisionV1 after{};
  g4::SetterDecisionV1 setter{};
  const std::uint64_t natural = g4::CurrentControlPair(state.input_action);
  if (bridge::ObservePhysicsSubmitBeforeOriginal(
          first, &state, context, base + g3::kPhysicsContextVtableRva, 3,
          90, natural, &submit) != bridge::Result::kObserved ||
      bridge::ObserveSteeringBeforeOriginal(
          &state, g4::FloatBits(0.0f), &setter) !=
          bridge::Result::kObserved ||
      bridge::ObserveBrakeBeforeOriginal(
          &state, g4::FloatBits(0.0f), &setter) !=
          bridge::Result::kObserved ||
      bridge::ObserveIntervalBeforeOriginal(
          first, &state, physics,
          base + g3::kPhysicsImplementationVtableRva, 3, 90, natural,
          &before) != bridge::Result::kObserved ||
      bridge::ObserveIntervalAfterOriginal(
          first, &state, g4::FloatBits(1.0f / 60.0f), &after) !=
          bridge::Result::kObserved ||
      bridge::AccountInjectedNitroCalls(&state, 0) !=
          bridge::Result::kObserved ||
      bridge::ObserveFinalWriterReturn(
          first, &state, player, base + 0x999000, 90) !=
          bridge::Result::kObserved ||
      bridge::ObserveTickEndReturn(
          first, &state, base + g3::kFrameEventSchedulerReturnRva, 91) !=
          bridge::Result::kComplete ||
      bridge::ObserveRaceEndAtClosedBoundary(first, &state, 2) !=
          bridge::Result::kRaceEnded)
    return false;

  bridge::StateV1 stale = state;
  if (bridge::RearmAfterArchivedRace(first, &stale) !=
          bridge::Result::kG3Fault ||
      stale.tick.coordinator.lifecycle !=
          a9tas::g3_tick_coordinator_v1::Lifecycle::kFaulted)
    return false;

  bridge::ConfigV1 replay = first;
  replay.tick.session_id = 32;
  replay.tick.generation = 8;
  replay.tick.replay_queue = {
      .mode = a9tas::g3_tick_coordinator_v1::ReplayMode::kActiveBlock,
      .communication_version_matches = true,
      .block_mode_still_active = true,
      .packets = &packet,
      .packet_count = 1,
  };
  if (bridge::RearmAfterArchivedRace(replay, &state) !=
          bridge::Result::kObserved ||
      state.receipt_count != 0 || state.input_action.tick_open ||
      state.tick.coordinator.generation != 8 ||
      state.tick.coordinator.lifecycle !=
          a9tas::g3_tick_coordinator_v1::Lifecycle::kArmed ||
      state.tick.coordinator.race_starts != 1 ||
      state.tick.coordinator.race_ends != 1 ||
      bridge::ObservePhysicsSubmitBeforeOriginal(
          replay, &state, context, base + g3::kPhysicsContextVtableRva, 2,
          90, natural, &submit) != bridge::Result::kIgnored ||
      state.tick.coordinator.tick != 0 || state.receipt_count != 0 ||
      bridge::ObservePhysicsSubmitBeforeOriginal(
          replay, &state, context, base + g3::kPhysicsContextVtableRva, 3,
          90, natural, &submit) != bridge::Result::kObserved)
    return false;
  return state.tick.coordinator.tick == 0 &&
         state.tick.coordinator.replay_head == 1 &&
         state.tick.coordinator.active_packet_present &&
         state.input_action.tick_open &&
         state.input_action.tick == 0;
}

bool ReplayPrefixContinuesAsRecordingWithoutTickReset() {
  const std::uintptr_t base = 0x74000000u;
  recording::RecordingFrameV1 prefix[] = {
      Packet(0, 0.25f, -1.0f, 1),
      Packet(1, -0.5f, 0.0f, 0),
  };
  bridge::ConfigV1 config{};
  config.tick.session_id = 0x9002;
  config.tick.generation = 9;
  config.tick.frame_limit = 7200;
  config.tick.game_base = base;
  config.tick.expected_begin_owner = 0x75000000u;
  config.tick.expected_begin_owner_vptr =
      base + g3::kPhysicsContextVtableRva;
  config.tick.expected_interval_owner = 0x76000000u;
  config.tick.expected_interval_owner_vptr =
      base + g3::kPhysicsImplementationVtableRva;
  config.tick.expected_player = 0x77000000u;
  config.tick.expected_player_vptr = base + 0x880000u;
  config.fixed_delta_us = 16667;

  bridge::StateV1 state{};
  state.tick.coordinator.lifecycle =
      a9tas::g3_tick_coordinator_v1::Lifecycle::kInRace;
  state.tick.coordinator.tick_phase =
      a9tas::g3_tick_coordinator_v1::TickPhase::kClosed;
  state.tick.coordinator.session_id = 0x9001;
  state.tick.coordinator.generation = 8;
  state.tick.coordinator.tick = 2;
  state.tick.coordinator.ticks_begun = 2;
  state.tick.coordinator.ticks_published = 2;
  state.tick.receipt_count = 2;
  state.receipt_count = 2;
  for (std::uint32_t index = 0; index < 2; ++index) {
    state.tick.receipts[index].session_id = 0x9001;
    state.tick.receipts[index].generation = 8;
    state.tick.receipts[index].tick = index;
    state.tick.receipts[index].selected_packet_index = index;
    state.receipts[index].tick = index;
    state.receipts[index].replay_packet_present = true;
  }
  state.input_action.setter_cache.brake_bits = g4::FloatBits(-1.0f);
  state.input_action.setter_cache.steering_bits = g4::FloatBits(-0.5f);
  state.input_action.setter_cache.event_sequence = 17;

  if (bridge::ContinueReplayAsRecordAtClosedBoundary(
          config, prefix, 2, &state) != bridge::Result::kObserved)
    return false;
  return state.tick.coordinator.tick == 2 && state.receipt_count == 2 &&
         state.tick.coordinator.session_id == 0x9002 &&
         state.tick.coordinator.generation == 9 &&
         state.tick.receipts[0].session_id == 0x9002 &&
         state.tick.receipts[1].generation == 9 &&
         state.tick.receipts[0].selected_packet_index ==
             a9tas::g3_tick_coordinator_v1::kNoPacket &&
         !state.receipts[0].replay_packet_present &&
         !state.receipts[1].replay_packet_present &&
         state.receipts[0].recorded_nitro_calls == 1 &&
         state.receipts[0].natural_nitro_calls == 1 &&
         state.input_action.setter_cache.event_sequence == 17 &&
         state.input_action.setter_cache.steering_bits ==
             g4::FloatBits(-0.5f);
}

}  // namespace

int main() {
  const bool two_ticks = RunTwoTicks();
  const bool lifecycle = LifecycleCompletionAfterActualTick();
  const bool archived_rearm = ArchivedRaceRearmsAtAuthoritativeTickZero();
  const bool continuous = ReplayPrefixContinuesAsRecordingWithoutTickReset();
  const bool passed = two_ticks && lifecycle && archived_rearm && continuous;
  std::printf("G4_G3_ADAPTER_SELFTEST passed=%d cases=4 "
               "two_ticks=%d lifecycle=%d archived_rearm=%d "
               "continuous=%d "
               "ticks=2 interval_calls=3 lifecycle_end=1 "
               "archived_rearm_tick0=1 "
               "device_access=0 game_writes=0\n",
              passed ? 1 : 0, two_ticks ? 1 : 0, lifecycle ? 1 : 0,
              archived_rearm ? 1 : 0, continuous ? 1 : 0);
  return passed ? 0 : 1;
}
