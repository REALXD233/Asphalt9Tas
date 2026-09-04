#include "g4_input_action_core_v1.h"

#include <cstdio>

namespace g4 = a9tas::g4_input_action_v1;
namespace recording = a9tas::unified_tick_v1;

namespace {

recording::RecordingFrameV1 Packet(std::uint64_t tick) {
  recording::RecordingFrameV1 packet{};
  packet.tick = tick;
  packet.steering = -0.75f;
  packet.brake = -1.0f;
  packet.accelerator = 0.5f;
  packet.nitro_activation_count = 2;
  packet.flags = recording::kRequiredFrameFlags;
  return packet;
}

bool NaturalRecord() {
  g4::StateV1 state{};
  g4::SetterDecisionV1 setter{};
  if (g4::ConsumeSteeringBeforeOriginal(
          &state, g4::FloatBits(0.25f), &setter) != g4::Result::kReady ||
      setter.replay_override || setter.tick_open ||
      g4::ConsumeBrakeBeforeOriginal(
          &state, g4::FloatBits(-1.0f), &setter) != g4::Result::kReady ||
      setter.replay_override || setter.tick_open)
    return false;
  g4::PrePhysicsDecisionV1 pre{};
  const std::uint64_t natural = g4::CurrentControlPair(state);
  if (g4::BeginTick(&state, 0, nullptr, natural, 16667, &pre) !=
          g4::Result::kReady ||
      pre.write_control_pair || !pre.write_fixed_delta ||
      pre.consumed_control_pair != natural || pre.replay_nitro_calls != 0)
    return false;
  if (g4::OnNaturalNitroCall(&state) !=
      g4::Result::kForwardNaturalNitro)
    return false;
  if (g4::ConsumeSteeringBeforeOriginal(
          &state, g4::FloatBits(0.5f), &setter) != g4::Result::kReady ||
      setter.replay_override || !setter.tick_open ||
      setter.final_bits != g4::FloatBits(0.5f) ||
      g4::ConsumeBrakeBeforeOriginal(
          &state, g4::FloatBits(0.0f), &setter) != g4::Result::kReady ||
      setter.replay_override || setter.final_bits != g4::FloatBits(0.0f))
    return false;
  std::uint32_t accelerator = 0;
  if (g4::ConsumeAcceleratorAfterOriginal(
          &state, g4::FloatBits(0.875f), &accelerator) != g4::Result::kReady ||
      accelerator != g4::FloatBits(0.875f))
    return false;
  g4::IntervalDecisionV1 interval{};
  if (g4::OnPhysicsIntervalAfterOriginal(
          &state, {}, g4::FloatBits(1.0f / 60.0f), &interval) !=
          g4::Result::kReady ||
      interval.override_after_original ||
      interval.final_output_bits != interval.original_output_bits)
    return false;
  g4::TickReceiptV1 receipt{};
  if (g4::EndTick(&state, {}, &receipt) != g4::Result::kReady ||
      receipt.recorded_nitro_calls != 1)
    return false;
  recording::RecordingFrameV1 frame{};
  if (g4::BuildActionRecordingFrame(receipt, 16667000, &frame) !=
      g4::Result::kReady)
    return false;
  return receipt.natural_nitro_calls == 1 &&
         receipt.physics_interval_calls == 1 && receipt.steering_observed &&
         receipt.brake_observed && receipt.accelerator_observed &&
         receipt.steering_calls == 1 && receipt.brake_calls == 1 &&
         receipt.accelerator_calls == 1 &&
         receipt.setter_sequence_at_begin == 2 &&
         receipt.setter_sequence_at_end == 5 &&
         receipt.steering_bits == g4::FloatBits(0.5f) &&
         receipt.brake_bits == g4::FloatBits(0.0f) &&
         receipt.accelerator_bits == g4::FloatBits(0.875f) &&
         frame.tick == 0 && frame.monotonic_ns == 16667000 &&
         frame.steering == 0.5f && frame.brake == 0.0f &&
         frame.accelerator == 0.0f && frame.nitro_activation_count == 1 &&
         frame.skip_override_flags == g4::kActionRecordSkipFlags &&
         frame.flags == g4::kActionRecordFrameFlags &&
         g4::ActionRecordingFrameValid(frame) && !g4::PacketValid(frame);
}

bool PhysicsRecord() {
  g4::TickReceiptV1 receipt{};
  receipt.tick = 3;
  receipt.fixed_delta_us = 16667;
  receipt.physics_interval_calls = 1;
  receipt.steering_bits = g4::FloatBits(0.25f);
  receipt.brake_bits = g4::FloatBits(-1.0f);
  receipt.recorded_nitro_calls = 1;
  receipt.natural_nitro_calls = 1;
  receipt.steering_calls = 1;
  receipt.brake_calls = 1;
  receipt.setter_sequence_at_begin = 4;
  receipt.setter_sequence_at_end = 6;

  float transform[16]{};
  transform[0] = transform[5] = transform[10] = transform[15] = 1.0f;
  transform[12] = 12.5f;
  const float linear[3] = {4.0f, -2.0f, 0.5f};
  g4::PhysicsSnapshotV1 snapshot{};
  if (g4::CapturePhysicsSnapshot(
          receipt.tick, reinterpret_cast<const std::uint8_t*>(transform),
          reinterpret_cast<const std::uint8_t*>(linear), &snapshot) !=
          g4::Result::kReady ||
      g4::CapturePhysicsSnapshot(
          receipt.tick, reinterpret_cast<const std::uint8_t*>(transform),
          reinterpret_cast<const std::uint8_t*>(linear), &snapshot) !=
          g4::Result::kWrongOrder)
    return false;

  recording::RecordingFrameV1 frame{};
  if (g4::BuildPhysicsRecordingFrame(receipt, snapshot, 50001000, &frame) !=
      g4::Result::kReady)
    return false;
  return frame.tick == 3 && frame.monotonic_ns == 50001000 &&
         frame.skip_override_flags == g4::kPhysicsRecordSkipFlags &&
         frame.flags == recording::kRequiredFrameFlags &&
         std::memcmp(frame.transform_bits, transform, sizeof(transform)) == 0 &&
         std::memcmp(frame.linear_velocity_bits, linear, sizeof(linear)) == 0 &&
         g4::PhysicsRecordingFrameValid(frame) && g4::PacketValid(frame) &&
         !g4::ActionRecordingFrameValid(frame);
}

bool ConditionalPhysicsReplay() {
  auto packet = Packet(4);
  packet.skip_override_flags = g4::kPhysicsRecordSkipFlags;
  float target_transform[16]{};
  target_transform[0] = target_transform[5] = target_transform[10] =
      target_transform[15] = 1.0f;
  target_transform[12] = 7.5f;
  const float target_linear[3] = {3.0f, -1.0f, 0.25f};
  std::memcpy(packet.transform_bits, target_transform,
              sizeof(target_transform));
  std::memcpy(packet.linear_velocity_bits, target_linear,
              sizeof(target_linear));

  g4::StateV1 state{};
  g4::PrePhysicsDecisionV1 pre{};
  const std::uint64_t natural_pair =
      static_cast<std::uint64_t>(g4::FloatBits(0.0f)) << 32 |
      g4::FloatBits(0.0f);
  if (g4::BeginTick(&state, 4, &packet, natural_pair, 16667, &pre) !=
      g4::Result::kReady)
    return false;

  g4::PhysicsCorrectionDecisionV1 decision{};
  if (g4::DecidePhysicsCorrection(
          state, reinterpret_cast<const std::uint8_t*>(target_transform),
          reinterpret_cast<const std::uint8_t*>(target_linear), &decision) !=
          g4::Result::kReady ||
      decision.tick != 4 ||
      decision.kind != g4::PhysicsCorrectionKindV1::kNaturalEqual)
    return false;

  float signed_zero_transform[16]{};
  std::memcpy(signed_zero_transform, target_transform,
              sizeof(signed_zero_transform));
  signed_zero_transform[1] = -0.0f;
  if (g4::DecidePhysicsCorrection(
          state, reinterpret_cast<const std::uint8_t*>(signed_zero_transform),
          reinterpret_cast<const std::uint8_t*>(target_linear), &decision) !=
          g4::Result::kReady ||
      decision.kind != g4::PhysicsCorrectionKindV1::kNaturalEqual)
    return false;

  const float nan = std::numeric_limits<float>::quiet_NaN();
  if (g4::ComponentFloatRangesEqual(
          reinterpret_cast<const std::uint8_t*>(&nan),
          reinterpret_cast<const std::uint8_t*>(&nan), sizeof(nan)))
    return false;

  float divergent_transform[16]{};
  std::memcpy(divergent_transform, target_transform,
              sizeof(divergent_transform));
  divergent_transform[12] += 1.0f;
  if (g4::DecidePhysicsCorrection(
          state, reinterpret_cast<const std::uint8_t*>(divergent_transform),
          reinterpret_cast<const std::uint8_t*>(target_linear), &decision) !=
          g4::Result::kReady ||
      decision.kind != g4::PhysicsCorrectionKindV1::kCorrectBoth)
    return false;

  float divergent_linear[3] = {target_linear[0], target_linear[1],
                               target_linear[2] + 1.0f};
  if (g4::DecidePhysicsCorrection(
          state, reinterpret_cast<const std::uint8_t*>(target_transform),
          reinterpret_cast<const std::uint8_t*>(divergent_linear), &decision) !=
          g4::Result::kReady ||
      decision.kind != g4::PhysicsCorrectionKindV1::kCorrectBoth)
    return false;

  state.packet.skip_override_flags |= recording::kSkipTransformForced;
  return g4::DecidePhysicsCorrection(
             state,
             reinterpret_cast<const std::uint8_t*>(divergent_transform),
             reinterpret_cast<const std::uint8_t*>(target_linear),
             &decision) == g4::Result::kReady &&
         decision.kind == g4::PhysicsCorrectionKindV1::kSkipped;
}

bool AluDefaultIntervalPassthroughWithCallCountDrift() {
  auto packet = Packet(7);
  const g4::IntervalSampleV1 samples[] = {
      {7, 0, g4::FloatBits(1.0f / 60.0f)},
      {7, 1, g4::FloatBits(1.0f / 120.0f)},
      {8, 0, g4::FloatBits(1.0f / 120.0f)},
  };
  const g4::IntervalReplayViewV1 view{samples, 3};
  g4::StateV1 state{};
  g4::PrePhysicsDecisionV1 pre{};
  const std::uint64_t natural =
      static_cast<std::uint64_t>(g4::FloatBits(0.0f)) << 32 |
      g4::FloatBits(0.0f);
  if (g4::BeginTick(&state, 7, &packet, natural, 16667, &pre) !=
          g4::Result::kReady ||
      pre.write_control_pair || pre.replay_nitro_calls != 2 ||
      pre.consumed_control_pair != natural)
    return false;
  g4::SetterDecisionV1 setter{};
  if (g4::ConsumeSteeringBeforeOriginal(
          &state, g4::FloatBits(0.0f), &setter) != g4::Result::kReady ||
      !setter.replay_override || setter.final_bits != g4::FloatBits(-0.75f) ||
      g4::ConsumeBrakeBeforeOriginal(
          &state, g4::FloatBits(0.0f), &setter) != g4::Result::kReady ||
      !setter.replay_override || setter.final_bits != g4::FloatBits(-1.0f))
    return false;
  if (g4::OnNaturalNitroCall(&state) !=
          g4::Result::kSuppressNaturalNitro ||
      g4::AccountInjectedNitroCalls(&state, 2) != g4::Result::kReady)
    return false;
  std::uint32_t accelerator = 0;
  if (g4::ConsumeAcceleratorAfterOriginal(
          &state, g4::FloatBits(0.0f), &accelerator) != g4::Result::kReady ||
      accelerator != g4::FloatBits(0.5f))
    return false;
  // The diagnostic source contains two calls for this tick while this replay
  // sees three. Default AluTasV2 interval override is disabled, so every real
  // getter result passes through unchanged.
  for (std::uint32_t ordinal = 0; ordinal < 3; ++ordinal) {
    g4::IntervalDecisionV1 interval{};
    if (g4::OnPhysicsIntervalAfterOriginal(
            &state, view, g4::FloatBits(1.0f / 30.0f), &interval) !=
            g4::Result::kReady ||
        interval.override_after_original || interval.ordinal != ordinal ||
        interval.final_output_bits != g4::FloatBits(1.0f / 30.0f))
      return false;
  }
  g4::TickReceiptV1 receipt{};
  return g4::EndTick(&state, view, &receipt) == g4::Result::kReady &&
         state.interval_cursor == 2 && receipt.physics_interval_calls == 3 &&
         receipt.injected_nitro_calls == 2 &&
         receipt.recorded_nitro_calls == 2 &&
         receipt.suppressed_nitro_calls == 1;
}

bool IndependentSkipBits() {
  auto packet = Packet(2);
  packet.skip_override_flags = recording::kSkipSteer |
                               recording::kSkipNitroActivation |
                               recording::kSkipAccelerator;
  const std::uint64_t natural =
      static_cast<std::uint64_t>(g4::FloatBits(0.25f)) << 32 |
      g4::FloatBits(0.0f);
  g4::StateV1 state{};
  g4::PrePhysicsDecisionV1 pre{};
  if (g4::BeginTick(&state, 2, &packet, natural, 16667, &pre) !=
          g4::Result::kReady ||
      pre.consumed_control_pair != natural ||
      pre.replay_nitro_calls != 0)
    return false;
  g4::SetterDecisionV1 setter{};
  if (g4::ConsumeSteeringBeforeOriginal(
          &state, g4::FloatBits(0.25f), &setter) != g4::Result::kReady ||
      setter.replay_override || setter.final_bits != g4::FloatBits(0.25f) ||
      g4::ConsumeBrakeBeforeOriginal(
          &state, g4::FloatBits(0.0f), &setter) != g4::Result::kReady ||
      !setter.replay_override || setter.final_bits != g4::FloatBits(-1.0f))
    return false;
  std::uint32_t accelerator = 0;
  return g4::ConsumeAcceleratorAfterOriginal(
             &state, g4::FloatBits(0.875f), &accelerator) ==
             g4::Result::kReady &&
         accelerator == g4::FloatBits(0.875f) &&
         g4::AccountInjectedNitroCalls(&state, 0) == g4::Result::kReady;
}

bool RejectMalformedSequences() {
  auto packet = Packet(4);
  g4::StateV1 state{};
  g4::PrePhysicsDecisionV1 pre{};
  const std::uint64_t natural =
      static_cast<std::uint64_t>(g4::FloatBits(0.0f)) << 32 |
      g4::FloatBits(0.0f);
  if (g4::BeginTick(&state, 4, &packet, natural, 16667, &pre) !=
      g4::Result::kReady)
    return false;
  if (g4::AccountInjectedNitroCalls(&state, 1) !=
      g4::Result::kNitroCountMismatch)
    return false;
  const g4::IntervalSampleV1 wrong[] = {
      {4, 1, g4::FloatBits(1.0f / 60.0f)},
  };
  g4::IntervalDecisionV1 interval{};
  if (g4::OnPhysicsIntervalAfterOriginal(
          &state, {wrong, 1}, g4::FloatBits(1.0f / 60.0f), &interval) !=
      g4::Result::kIntervalMismatch)
    return false;

  g4::StateV1 extra_state{};
  if (g4::BeginTick(&extra_state, 4, &packet, natural, 16667, &pre) !=
      g4::Result::kReady)
    return false;
  const g4::IntervalSampleV1 extra[] = {
      {4, 0, g4::FloatBits(1.0f / 60.0f)},
      {4, 1, g4::FloatBits(1.0f / 60.0f)},
  };
  if (g4::OnPhysicsIntervalAfterOriginal(
          &extra_state, {extra, 2}, g4::FloatBits(1.0f / 60.0f),
          &interval) != g4::Result::kReady)
    return false;
  g4::TickReceiptV1 receipt{};
  // One live call is also valid when recording observed two.  EndTick consumes
  // the diagnostic group rather than requiring identical call topology.
  if (g4::EndTick(&extra_state, {extra, 2}, &receipt) !=
          g4::Result::kReady ||
      extra_state.interval_cursor != 2 || receipt.physics_interval_calls != 1)
    return false;

  g4::StateV1 variable_source_state{};
  if (g4::BeginTick(&variable_source_state, 4, &packet, natural, 16667, &pre) !=
      g4::Result::kReady)
    return false;
  const g4::IntervalSampleV1 variable_source[] = {
      {4, 0, g4::FloatBits(1.0f / 60.0f)},
      {4, 1, g4::FloatBits(1.0f / 120.0f)},
  };
  if (g4::OnPhysicsIntervalAfterOriginal(
          &variable_source_state, {variable_source, 2},
          g4::FloatBits(1.0f / 30.0f), &interval) != g4::Result::kReady ||
      interval.override_after_original ||
      interval.final_output_bits != g4::FloatBits(1.0f / 30.0f) ||
      g4::EndTick(&variable_source_state, {variable_source, 2}, &receipt) !=
          g4::Result::kReady)
    return false;

  auto invalid = Packet(9);
  invalid.steering = std::numeric_limits<float>::quiet_NaN();
  g4::StateV1 invalid_state{};
  g4::SetterDecisionV1 setter{};
  return g4::BeginTick(&invalid_state, 9, &invalid, natural, 16667, &pre) ==
             g4::Result::kInvalidPacket &&
         g4::ConsumeSteeringBeforeOriginal(
             &invalid_state, g4::FloatBits(1.1f), &setter) ==
             g4::Result::kInvalidNaturalValue;
}

}  // namespace

int main() {
  const bool natural = NaturalRecord();
  const bool physics = PhysicsRecord();
  const bool correction = ConditionalPhysicsReplay();
  const bool replay = AluDefaultIntervalPassthroughWithCallCountDrift();
  const bool skips = IndependentSkipBits();
  const bool malformed = RejectMalformedSequences();
  std::printf("G4_INPUT_ACTION_CORE_SELFTEST passed=%d natural=%d physics=%d "
               "correction=%d replay=%d skip=%d malformed=%d "
               "device_access=0 game_writes=0\n",
               natural && physics && correction && replay && skips && malformed ? 1 : 0,
               natural ? 1 : 0, physics ? 1 : 0, correction ? 1 : 0,
               replay ? 1 : 0, skips ? 1 : 0, malformed ? 1 : 0);
  return natural && physics && correction && replay && skips && malformed ? 0 : 1;
}
