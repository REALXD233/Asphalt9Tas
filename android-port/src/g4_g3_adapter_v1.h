#pragma once

// Minimal bridge from the live-proven G3 tick coordinator to the pure G4
// input/action semantics.  It owns no hook and performs no memory access.

#include "g3_boundary_adapter_v1.h"
#include "g4_input_action_core_v1.h"

#include <cstdint>

namespace a9tas::g4_g3_adapter_v1 {

namespace g3 = a9tas::g3_boundary_adapter_v1;
namespace coordinator = a9tas::g3_tick_coordinator_v1;
namespace g4 = a9tas::g4_input_action_v1;

inline constexpr std::uint32_t kMaximumReceipts =
    g3::kMaximumNeutralReceipts;

enum class Result : std::int32_t {
  kObserved = 1,
  kRepeatedInterval = 2,
  kComplete = 3,
  kIgnored = 4,
  kRaceEnded = 5,
  kInvalidArgument = -1,
  kG3Fault = -2,
  kG4Fault = -3,
  kReceiptOverflow = -4,
};

// The Physics Interval entry is shared by multiple implementation objects.
// Only a before-original observation that actually belongs to the bound race
// owner may consume the matching after-original interval sample.
inline constexpr bool IntervalAfterQualified(Result before_result) noexcept {
  return before_result == Result::kObserved ||
         before_result == Result::kRepeatedInterval;
}

struct ConfigV1 {
  g3::Config tick{};
  std::uint32_t fixed_delta_us{};
  g4::IntervalReplayViewV1 interval_replay{};
};

struct StateV1 {
  g3::State tick{};
  g4::StateV1 input_action{};
  g4::TickReceiptV1 receipts[kMaximumReceipts]{};
  std::uint32_t receipt_count{};
  std::int32_t last_g3_result{};
  std::int32_t last_g4_result{};
};

inline void ResetSessionMetadata(StateV1* state) noexcept {
  state->input_action = {};
  state->receipt_count = 0;
  state->last_g3_result = 0;
  state->last_g4_result = 0;
}

struct IntervalBeforeReceiptV1 {
  bool first_call_in_tick{};
  g4::PrePhysicsDecisionV1 decision{};
};

using SubmitBeforeReceiptV1 = IntervalBeforeReceiptV1;

inline bool ConfigValid(const ConfigV1& config) noexcept {
  return g3::ConfigValid(config.tick) &&
         config.fixed_delta_us >=
             a9tas::unified_tick_v1::kMinimumFixedIntervalUs &&
         config.fixed_delta_us <=
             a9tas::unified_tick_v1::kMaximumFixedIntervalUs &&
         g4::IntervalViewValid(config.interval_replay);
}

inline Result Initialize(const ConfigV1& config, StateV1* state) noexcept {
  if (state == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  ResetSessionMetadata(state);
  const g3::Result result = g3::Initialize(config.tick, &state->tick);
  state->last_g3_result = static_cast<std::int32_t>(result);
  return result == g3::Result::kObserved ? Result::kObserved
                                         : Result::kG3Fault;
}

// G8 same-process Retry reuse. The caller must have archived the completed
// race before invoking this seam. The proven G3 coordinator retains only its
// monotonic race counters and accepts a strictly newer generation; all G4
// per-race receipts and action state are cleared for authoritative tick 0.
inline Result RearmAfterArchivedRace(const ConfigV1& config,
                                     StateV1* state) noexcept {
  if (state == nullptr || !ConfigValid(config) ||
      state->input_action.tick_open)
    return Result::kInvalidArgument;
  const g3::Result result =
      g3::RearmAfterArchivedRace(config.tick, &state->tick);
  if (result != g3::Result::kObserved) {
    state->last_g3_result = static_cast<std::int32_t>(result);
    return Result::kG3Fault;
  }
  ResetSessionMetadata(state);
  state->last_g3_result = static_cast<std::int32_t>(result);
  return Result::kObserved;
}

inline Result RearmAfterSealedCheckpointAtRetry(
    const ConfigV1& config, StateV1* state) noexcept {
  if (state == nullptr || !ConfigValid(config) ||
      state->input_action.tick_open)
    return Result::kInvalidArgument;
  const g3::Result result =
      g3::RearmAfterSealedCheckpointAtRetry(config.tick, &state->tick);
  if (result != g3::Result::kObserved) {
    state->last_g3_result = static_cast<std::int32_t>(result);
    return Result::kG3Fault;
  }
  ResetSessionMetadata(state);
  state->last_g3_result = static_cast<std::int32_t>(result);
  return Result::kObserved;
}

inline Result RearmAtPausedRace(const ConfigV1& config,
                                StateV1* state) noexcept {
  if (state == nullptr || !ConfigValid(config) ||
      state->input_action.tick_open)
    return Result::kInvalidArgument;
  const g3::Result result = g3::RearmAtPausedRace(config.tick, &state->tick);
  if (result != g3::Result::kObserved) {
    state->last_g3_result = static_cast<std::int32_t>(result);
    return Result::kG3Fault;
  }
  ResetSessionMetadata(state);
  state->last_g3_result = static_cast<std::int32_t>(result);
  return Result::kObserved;
}

inline Result RearmFromActiveReplayAtClosedBoundary(
    const ConfigV1& config, StateV1* state) noexcept {
  if (state == nullptr || !ConfigValid(config) ||
      state->input_action.tick_open)
    return Result::kInvalidArgument;
  const g3::Result result = g3::RearmFromActiveReplayAtClosedBoundary(
      config.tick, &state->tick);
  if (result != g3::Result::kObserved) {
    state->last_g3_result = static_cast<std::int32_t>(result);
    return Result::kG3Fault;
  }
  ResetSessionMetadata(state);
  state->last_g3_result = static_cast<std::int32_t>(result);
  return Result::kObserved;
}

inline Result ContinueReplayAsRecordAtClosedBoundary(
    const ConfigV1& config,
    const a9tas::unified_tick_v1::RecordingFrameV1* replayed_prefix,
    std::uint32_t prefix_count, StateV1* state) noexcept {
  if (state == nullptr || replayed_prefix == nullptr ||
      !ConfigValid(config) || prefix_count == 0 ||
      prefix_count != state->receipt_count ||
      prefix_count != state->tick.coordinator.tick ||
      state->input_action.tick_open)
    return Result::kInvalidArgument;

  for (std::uint32_t index = 0; index < prefix_count; ++index) {
    if (!g4::PacketValid(replayed_prefix[index]) ||
        replayed_prefix[index].tick != index)
      return Result::kInvalidArgument;
  }
  const g3::Result result = g3::ContinueReplayAsRecordAtClosedBoundary(
      config.tick, &state->tick);
  if (result != g3::Result::kObserved) {
    state->last_g3_result = static_cast<std::int32_t>(result);
    return Result::kG3Fault;
  }

  // Preserve the live setter cache so the first natural suffix tick starts
  // from the exact controls that ended the prefix.  Only per-open-tick state is
  // cleared.  Rewrite retained action receipts as recording receipts because
  // the prefix is now part of the exported recording, not an active replay.
  const g4::SetterCacheV1 setter_cache = state->input_action.setter_cache;
  state->input_action = {};
  state->input_action.setter_cache = setter_cache;
  for (std::uint32_t index = 0; index < prefix_count; ++index) {
    const auto& frame = replayed_prefix[index];
    auto& receipt = state->receipts[index];
    const std::uint64_t pair =
        static_cast<std::uint64_t>(g4::FloatBits(frame.brake)) |
        (static_cast<std::uint64_t>(g4::FloatBits(frame.steering)) << 32u);
    receipt.tick = index;
    receipt.natural_control_pair = pair;
    receipt.consumed_control_pair = pair;
    receipt.fixed_delta_us = config.fixed_delta_us;
    receipt.natural_nitro_calls = frame.nitro_activation_count;
    receipt.suppressed_nitro_calls = 0;
    receipt.injected_nitro_calls = 0;
    receipt.recorded_nitro_calls = frame.nitro_activation_count;
    receipt.steering_bits = g4::FloatBits(frame.steering);
    receipt.brake_bits = g4::FloatBits(frame.brake);
    receipt.accelerator_bits = g4::FloatBits(frame.accelerator);
    receipt.replay_packet_present = false;
  }
  state->last_g3_result = static_cast<std::int32_t>(result);
  state->last_g4_result = static_cast<std::int32_t>(g4::Result::kReady);
  return Result::kObserved;
}

inline Result ObservePhysicsSubmitBeforeOriginal(
    const ConfigV1& config, StateV1* state, std::uintptr_t context,
    std::uintptr_t context_vptr, std::uint32_t lifecycle_state,
    std::uint32_t tid, std::uint64_t natural_control_pair,
    SubmitBeforeReceiptV1* receipt) noexcept {
  if (state == nullptr || receipt == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  *receipt = {};
  const g3::Result observed = g3::ObserveTickBeginAtSubmit(
      config.tick, &state->tick, context, context_vptr, lifecycle_state, tid);
  state->last_g3_result = static_cast<std::int32_t>(observed);
  if (static_cast<std::int32_t>(observed) < 0) return Result::kG3Fault;
  if (observed == g3::Result::kIgnoredUnqualified ||
      observed == g3::Result::kWaitingForRace ||
      observed == g3::Result::kWaitingForTick)
    return Result::kIgnored;
  if (observed == g3::Result::kComplete) return Result::kComplete;

  const a9tas::unified_tick_v1::RecordingFrameV1* packet = nullptr;
  (void)coordinator::ActivePacket(
      state->tick.coordinator, config.tick.generation,
      state->tick.coordinator.tick, &packet);
  const g4::Result begun = g4::BeginTick(
      &state->input_action, state->tick.coordinator.tick, packet,
      natural_control_pair, config.fixed_delta_us, &receipt->decision);
  state->last_g4_result = static_cast<std::int32_t>(begun);
  if (begun != g4::Result::kReady) return Result::kG4Fault;
  receipt->first_call_in_tick = true;
  return Result::kObserved;
}

inline Result ObserveIntervalBeforeOriginal(
    const ConfigV1& config, StateV1* state, std::uintptr_t owner,
    std::uintptr_t owner_vptr, std::uint32_t lifecycle_state,
    std::uint32_t tid, std::uint64_t natural_control_pair,
    IntervalBeforeReceiptV1* receipt) noexcept {
  if (state == nullptr || receipt == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  *receipt = {};
  (void)natural_control_pair;
  const coordinator::TickPhase before = state->tick.coordinator.tick_phase;
  const bool legacy_combined =
      config.tick.expected_begin_owner ==
      config.tick.expected_interval_owner;
  const g3::Result observed = legacy_combined
      ? g3::ObserveTickBeginAndPrePhysics(
            config.tick, &state->tick, owner, owner_vptr, lifecycle_state, tid)
      : g3::ObservePhysicsInterval(
            config.tick, &state->tick, owner, owner_vptr, lifecycle_state, tid);
  state->last_g3_result = static_cast<std::int32_t>(observed);
  if (static_cast<std::int32_t>(observed) < 0) return Result::kG3Fault;
  if (observed == g3::Result::kIgnoredUnqualified ||
      observed == g3::Result::kWaitingForRace ||
      observed == g3::Result::kWaitingForTick)
    return Result::kIgnored;
  if (observed == g3::Result::kComplete) return Result::kComplete;
  if (legacy_combined &&
      before == coordinator::TickPhase::kClosed) {
    const a9tas::unified_tick_v1::RecordingFrameV1* packet = nullptr;
    (void)coordinator::ActivePacket(
        state->tick.coordinator, config.tick.generation,
        state->tick.coordinator.tick, &packet);
    const g4::Result begun = g4::BeginTick(
        &state->input_action, state->tick.coordinator.tick, packet,
        natural_control_pair, config.fixed_delta_us, &receipt->decision);
    state->last_g4_result = static_cast<std::int32_t>(begun);
    if (begun != g4::Result::kReady) return Result::kG4Fault;
  }
  if (!state->input_action.tick_open) return Result::kG4Fault;
  receipt->first_call_in_tick =
      before == coordinator::TickPhase::kBegun;
  return receipt->first_call_in_tick ? Result::kObserved
                                     : Result::kRepeatedInterval;
}

inline Result ObserveIntervalAfterOriginal(
    const ConfigV1& config, StateV1* state,
    std::uint32_t original_output_bits,
    g4::IntervalDecisionV1* decision) noexcept {
  if (state == nullptr || decision == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  const g4::Result result = g4::OnPhysicsIntervalAfterOriginal(
      &state->input_action, config.interval_replay, original_output_bits,
      decision);
  state->last_g4_result = static_cast<std::int32_t>(result);
  return result == g4::Result::kReady ? Result::kObserved
                                      : Result::kG4Fault;
}

inline Result ObserveSteeringBeforeOriginal(
    StateV1* state, std::uint32_t natural_bits,
    g4::SetterDecisionV1* decision) noexcept {
  if (state == nullptr || decision == nullptr) return Result::kInvalidArgument;
  const g4::Result result = g4::ConsumeSteeringBeforeOriginal(
      &state->input_action, natural_bits, decision);
  state->last_g4_result = static_cast<std::int32_t>(result);
  return result == g4::Result::kReady ? Result::kObserved
                                      : Result::kG4Fault;
}

inline Result ObserveBrakeBeforeOriginal(
    StateV1* state, std::uint32_t natural_bits,
    g4::SetterDecisionV1* decision) noexcept {
  if (state == nullptr || decision == nullptr) return Result::kInvalidArgument;
  const g4::Result result = g4::ConsumeBrakeBeforeOriginal(
      &state->input_action, natural_bits, decision);
  state->last_g4_result = static_cast<std::int32_t>(result);
  return result == g4::Result::kReady ? Result::kObserved
                                      : Result::kG4Fault;
}

inline Result ObserveAcceleratorAfterOriginal(
    StateV1* state, std::uint32_t natural_bits,
    std::uint32_t* final_bits) noexcept {
  if (state == nullptr || final_bits == nullptr) return Result::kInvalidArgument;
  const g4::Result result = g4::ConsumeAcceleratorAfterOriginal(
      &state->input_action, natural_bits, final_bits);
  state->last_g4_result = static_cast<std::int32_t>(result);
  return result == g4::Result::kReady ? Result::kObserved
                                      : Result::kG4Fault;
}

inline Result ObserveFinalWriterReturn(const ConfigV1& config,
                                       StateV1* state,
                                       std::uintptr_t player,
                                       std::uintptr_t player_vptr,
                                       std::uint32_t tid) noexcept {
  if (state == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  const g3::Result result = g3::ObserveFinalWriterReturn(
      config.tick, &state->tick, player, player_vptr, tid);
  state->last_g3_result = static_cast<std::int32_t>(result);
  if (static_cast<std::int32_t>(result) < 0) return Result::kG3Fault;
  return result == g3::Result::kIgnoredUnqualified ||
                 result == g3::Result::kWaitingForRace ||
                 result == g3::Result::kWaitingForTick
             ? Result::kIgnored
             : Result::kObserved;
}

inline Result ObserveTickEndReturn(const ConfigV1& config, StateV1* state,
                                   std::uintptr_t caller_return,
                                   std::uint32_t tid) noexcept {
  if (state == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  if (caller_return != config.tick.game_base +
                           config.tick.frame_event_scheduler_return_rva) {
    const g3::Result ignored = g3::ObserveFrameEventReturn(
        config.tick, &state->tick, caller_return, tid);
    state->last_g3_result = static_cast<std::int32_t>(ignored);
    return ignored == g3::Result::kIgnoredUnqualified ? Result::kIgnored
                                                       : Result::kG3Fault;
  }
  // The frame-event return also runs while the session is armed but before the
  // first Physics Interval, and between already-closed ticks.  G3 deliberately
  // classifies both as idle observations.  Do not call the G4 EndTick path
  // until G3 has an open authoritative tick.
  if (state->tick.complete != 0) return Result::kComplete;
  if (state->tick.coordinator.lifecycle ==
          a9tas::g3_tick_coordinator_v1::Lifecycle::kArmed ||
      state->tick.coordinator.tick_phase ==
          a9tas::g3_tick_coordinator_v1::TickPhase::kClosed) {
    const g3::Result idle = g3::ObserveFrameEventReturn(
        config.tick, &state->tick, caller_return, tid);
    state->last_g3_result = static_cast<std::int32_t>(idle);
    if (static_cast<std::int32_t>(idle) < 0) return Result::kG3Fault;
    return idle == g3::Result::kWaitingForRace ||
                   idle == g3::Result::kWaitingForTick ||
                   idle == g3::Result::kIgnoredUnqualified
               ? Result::kIgnored
               : Result::kG3Fault;
  }
  if (state->receipt_count >= kMaximumReceipts)
    return Result::kReceiptOverflow;

  g4::TickReceiptV1 input_receipt{};
  const g4::Result ended = g4::EndTick(
      &state->input_action, config.interval_replay, &input_receipt);
  state->last_g4_result = static_cast<std::int32_t>(ended);
  if (ended != g4::Result::kReady) return Result::kG4Fault;

  const g3::Result published = g3::ObserveFrameEventReturn(
      config.tick, &state->tick, caller_return, tid);
  state->last_g3_result = static_cast<std::int32_t>(published);
  if (static_cast<std::int32_t>(published) < 0) return Result::kG3Fault;
  state->receipts[state->receipt_count++] = input_receipt;
  return published == g3::Result::kComplete ? Result::kComplete
                                            : Result::kObserved;
}

inline Result ObserveRaceEndAtClosedBoundary(
    const ConfigV1& config, StateV1* state,
    std::uint32_t lifecycle_state) noexcept {
  if (state == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  if (state->input_action.tick_open) return Result::kG4Fault;
  const g3::Result ended = g3::ObserveRaceEndAtClosedBoundary(
      config.tick, &state->tick, lifecycle_state);
  state->last_g3_result = static_cast<std::int32_t>(ended);
  if (static_cast<std::int32_t>(ended) < 0) return Result::kG3Fault;
  return ended == g3::Result::kRaceEnded ? Result::kRaceEnded
                                         : Result::kIgnored;
}

inline Result DiscardOpenTickForRaceEnd(const ConfigV1& config,
                                        StateV1* state) noexcept {
  if (state == nullptr || !ConfigValid(config))
    return Result::kInvalidArgument;
  if (!state->input_action.tick_open ||
      state->tick.coordinator.tick_phase == coordinator::TickPhase::kClosed)
    return Result::kInvalidArgument;
  const g3::Result tick_discarded =
      g3::DiscardOpenTickForRaceEnd(config.tick, &state->tick);
  state->last_g3_result = static_cast<std::int32_t>(tick_discarded);
  if (tick_discarded != g3::Result::kObserved) return Result::kG3Fault;
  const g4::Result action_discarded =
      g4::DiscardOpenTickForRaceEnd(&state->input_action);
  state->last_g4_result = static_cast<std::int32_t>(action_discarded);
  return action_discarded == g4::Result::kReady ? Result::kObserved
                                                : Result::kG4Fault;
}

inline Result ObserveNaturalNitroCall(StateV1* state,
                                      bool* call_original) noexcept {
  if (state == nullptr || call_original == nullptr)
    return Result::kInvalidArgument;
  const g4::Result result = g4::OnNaturalNitroCall(&state->input_action);
  state->last_g4_result = static_cast<std::int32_t>(result);
  if (result == g4::Result::kForwardNaturalNitro) {
    *call_original = true;
    return Result::kObserved;
  }
  if (result == g4::Result::kSuppressNaturalNitro) {
    *call_original = false;
    return Result::kObserved;
  }
  return Result::kG4Fault;
}

inline Result AccountInjectedNitroCalls(StateV1* state,
                                        std::uint32_t completed) noexcept {
  if (state == nullptr) return Result::kInvalidArgument;
  const g4::Result result =
      g4::AccountInjectedNitroCalls(&state->input_action, completed);
  state->last_g4_result = static_cast<std::int32_t>(result);
  return result == g4::Result::kReady ? Result::kObserved
                                      : Result::kG4Fault;
}

}  // namespace a9tas::g4_g3_adapter_v1
