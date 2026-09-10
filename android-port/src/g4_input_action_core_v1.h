#pragma once

// Pure G4 semantics shared by the recorder and the in-process replay runtime.
// This file deliberately performs no process access and calls no game method.
// It models the behavior of AluTasV2's input, Nitro and interval detours at the
// already proven Android Physics Interval tick boundary.

#include "unified_tick_recording_v1.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace a9tas::g4_input_action_v1 {

namespace recording = a9tas::unified_tick_v1;

inline constexpr std::uint32_t kVersion = 1;

// A G4 action-stage recording deliberately leaves the fields owned by later
// mainline stages disabled.  Brake, steering, Nitro and the exact Physics
// Interval stream are authoritative here; accelerator, respawn, barrel tails
// and final transform/linear correction are not yet claimed.
inline constexpr std::uint32_t kActionRecordSkipFlags =
    recording::kSkipAccelerator | recording::kSkipBarrelAngular |
    recording::kSkipBarrelRbx | recording::kSkipRespawnButton |
    recording::kSkipTransformForced;
static_assert(kActionRecordSkipFlags == 0xf8,
              "G4 action-stage recording scope");
inline constexpr std::uint32_t kActionRecordFrameFlags =
    recording::kControlsValid | recording::kActionsValid;
static_assert(kActionRecordFrameFlags == 0x3,
              "G4 action-stage frame completeness");

// G5 closes the field that G4 deliberately left pending.  Transform and
// linear velocity are sampled after the real FinalRacerTransformWriter and
// before the authoritative TickEnd receipt, matching AluTasV2's ordering.
// Accelerator and respawn remain later mainline stages.  G6 closes the two
// independent AluTasV2 barrel tails while preserving the same 144-byte frame.
inline constexpr std::uint32_t kLegacyPhysicsRecordSkipFlags = 0x78;
inline constexpr std::uint32_t kPhysicsRecordSkipFlags =
    kActionRecordSkipFlags & ~recording::kSkipTransformForced &
    ~recording::kSkipBarrelAngular & ~recording::kSkipBarrelRbx;
static_assert(kPhysicsRecordSkipFlags == 0x48,
              "G6 physics-and-barrel recording scope");
inline constexpr std::uint32_t kPhysicsRecordFrameFlags =
    recording::kRequiredFrameFlags;

// Physics Interval is a call stream, not one constant per logical tick.  The
// ordinal makes repeated calls within one tick lossless and deterministic.
#pragma pack(push, 1)
struct IntervalSampleV1 {
  std::uint64_t tick{};
  std::uint32_t ordinal{};
  std::uint32_t output_bits{};
};
#pragma pack(pop)

static_assert(sizeof(IntervalSampleV1) == 16, "G4 interval sample ABI");

struct IntervalReplayViewV1 {
  const IntervalSampleV1* samples{};
  std::size_t sample_count{};
};

struct PhysicsSnapshotV1 {
  std::uint64_t tick{};
  std::uint8_t transform_bits[recording::kTransformSize]{};
  std::uint8_t linear_velocity_bits[recording::kLinearVelocitySize]{};
  bool captured{};
};

enum class PhysicsCorrectionKindV1 : std::uint32_t {
  kSkipped = 0,
  kNaturalEqual = 1,
  kCorrectBoth = 2,
};

struct PhysicsCorrectionDecisionV1 {
  std::uint64_t tick{};
  PhysicsCorrectionKindV1 kind{PhysicsCorrectionKindV1::kSkipped};
};

enum class Result : std::int32_t {
  kReady = 1,
  kForwardNaturalNitro = 2,
  kSuppressNaturalNitro = 3,
  kInvalidArgument = -1,
  kWrongTick = -2,
  kWrongOrder = -3,
  kInvalidPacket = -4,
  kInvalidNaturalValue = -5,
  kIntervalMismatch = -6,
  kOverflow = -7,
  kNitroCountMismatch = -8,
};

struct PrePhysicsDecisionV1 {
  std::uint64_t tick{};
  std::uint64_t natural_control_pair{};
  std::uint64_t consumed_control_pair{};
  std::int64_t fixed_delta_us{};
  std::uint32_t replay_nitro_calls{};
  bool replay_packet_present{};
  bool write_control_pair{};
  bool write_fixed_delta{};
};

struct IntervalDecisionV1 {
  std::uint64_t tick{};
  std::uint32_t ordinal{};
  std::uint32_t original_output_bits{};
  std::uint32_t final_output_bits{};
  bool override_after_original{};
};

struct TickReceiptV1 {
  std::uint64_t tick{};
  std::uint64_t natural_control_pair{};
  std::uint64_t consumed_control_pair{};
  std::int64_t fixed_delta_us{};
  std::uint32_t physics_interval_calls{};
  std::uint32_t natural_nitro_calls{};
  std::uint32_t suppressed_nitro_calls{};
  std::uint32_t injected_nitro_calls{};
  std::uint32_t recorded_nitro_calls{};
  std::uint32_t steering_bits{};
  std::uint32_t brake_bits{};
  std::uint32_t accelerator_bits{};
  std::uint32_t steering_calls{};
  std::uint32_t brake_calls{};
  std::uint32_t accelerator_calls{};
  std::uint64_t setter_sequence_at_begin{};
  std::uint64_t setter_sequence_at_end{};
  bool steering_observed{};
  bool brake_observed{};
  bool replay_packet_present{};
  bool accelerator_observed{};
};

struct SetterCacheV1 {
  std::uint32_t steering_bits{};
  std::uint32_t brake_bits{};
  std::uint32_t accelerator_bits{};
  std::uint32_t steering_calls{};
  std::uint32_t brake_calls{};
  std::uint32_t accelerator_calls{};
  std::uint64_t event_sequence{};
};

struct SetterDecisionV1 {
  std::uint32_t natural_bits{};
  std::uint32_t final_bits{};
  bool replay_override{};
  bool tick_open{};
};

struct StateV1 {
  bool tick_open{};
  bool replay_packet_present{};
  std::uint64_t tick{};
  std::size_t interval_cursor{};
  std::uint32_t interval_ordinal{};
  recording::RecordingFrameV1 packet{};
  TickReceiptV1 receipt{};
  SetterCacheV1 setter_cache{};
};

inline std::uint32_t FloatBits(float value) noexcept {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

inline float BitsFloat(std::uint32_t bits) noexcept {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline bool AxisBitsValid(std::uint32_t bits, float magnitude) noexcept {
  const float value = BitsFloat(bits);
  return std::isfinite(value) && std::fabs(value) <= magnitude;
}

inline bool IntervalBitsValid(std::uint32_t bits) noexcept {
  const float value = BitsFloat(bits);
  return std::isfinite(value) && value >= 0.001f && value <= 0.1f;
}

inline bool RawPhysicsBitsValid(const std::uint8_t* transform,
                                const std::uint8_t* linear) noexcept;

inline bool PacketValid(const recording::RecordingFrameV1& packet) noexcept {
  return packet.flags == recording::kRequiredFrameFlags &&
         packet.reserved == 0 &&
         (packet.skip_override_flags & ~recording::kSupportedSkipMask) == 0 &&
         packet.nitro_activation_count <= 2 &&
         AxisBitsValid(FloatBits(packet.steering), 1.0f) &&
         AxisBitsValid(FloatBits(packet.brake), 1.05f) &&
         AxisBitsValid(FloatBits(packet.accelerator), 1.05f) &&
         (((packet.skip_override_flags & recording::kSkipTransformForced) !=
           0) ||
          RawPhysicsBitsValid(packet.transform_bits,
                              packet.linear_velocity_bits));
}

inline bool ActionRecordingFrameValid(
    const recording::RecordingFrameV1& frame) noexcept {
  const recording::RecordingFrameV1 zero{};
  return frame.flags == kActionRecordFrameFlags && frame.reserved == 0 &&
         frame.skip_override_flags == kActionRecordSkipFlags &&
         AxisBitsValid(FloatBits(frame.steering), 1.0f) &&
         AxisBitsValid(FloatBits(frame.brake), 1.05f) &&
         FloatBits(frame.accelerator) == 0 &&
         frame.nitro_activation_count <= 2 && frame.respawn_button_press == 0 &&
         std::memcmp(frame.padding, zero.padding, sizeof(frame.padding)) == 0 &&
         std::memcmp(frame.barrel_angular_velocity,
                     zero.barrel_angular_velocity,
                     sizeof(frame.barrel_angular_velocity)) == 0 &&
         std::memcmp(frame.barrel_rbx, zero.barrel_rbx,
                     sizeof(frame.barrel_rbx)) == 0 &&
         std::memcmp(frame.transform_bits, zero.transform_bits,
                     sizeof(frame.transform_bits)) == 0 &&
         std::memcmp(frame.linear_velocity_bits, zero.linear_velocity_bits,
                     sizeof(frame.linear_velocity_bits)) == 0;
}

inline bool RawPhysicsBitsValid(const std::uint8_t* transform,
                                const std::uint8_t* linear) noexcept {
  if (transform == nullptr || linear == nullptr) return false;
  std::uint32_t bits[19]{};
  std::memcpy(bits, transform, recording::kTransformSize);
  std::memcpy(bits + 16, linear, recording::kLinearVelocitySize);
  for (std::uint32_t value : bits) {
    const float component = BitsFloat(value);
    if (!std::isfinite(component) || std::fabs(component) > 1000000.0f)
      return false;
  }
  return true;
}

inline bool PhysicsSnapshotValid(const PhysicsSnapshotV1& snapshot) noexcept {
  return snapshot.captured &&
         RawPhysicsBitsValid(snapshot.transform_bits,
                             snapshot.linear_velocity_bits);
}

// AluTasV2's Bullet transform/vector operators compare float components, not
// their byte representation. Preserve the raw source bytes for publication,
// while treating +0.0f and -0.0f as equal and every NaN as unequal.
inline bool ComponentFloatRangesEqual(const std::uint8_t* lhs,
                                      const std::uint8_t* rhs,
                                      std::size_t byte_count) noexcept {
  if (lhs == nullptr || rhs == nullptr || byte_count == 0 ||
      byte_count % sizeof(float) != 0)
    return false;
  for (std::size_t offset = 0; offset < byte_count; offset += sizeof(float)) {
    float lhs_value = 0.0f;
    float rhs_value = 0.0f;
    std::memcpy(&lhs_value, lhs + offset, sizeof(lhs_value));
    std::memcpy(&rhs_value, rhs + offset, sizeof(rhs_value));
    if (lhs_value != rhs_value) return false;
  }
  return true;
}

inline bool PhysicsRecordingFrameValid(
    const recording::RecordingFrameV1& frame) noexcept {
  const recording::RecordingFrameV1 zero{};
  const bool legacy =
      frame.skip_override_flags == kLegacyPhysicsRecordSkipFlags;
  const bool barrel = frame.skip_override_flags == kPhysicsRecordSkipFlags;
  bool barrel_values_valid = barrel;
  for (float value : frame.barrel_angular_velocity)
    barrel_values_valid = barrel_values_valid && std::isfinite(value) &&
                          std::fabs(value) <= 1000000.0f;
  for (float value : frame.barrel_rbx)
    barrel_values_valid = barrel_values_valid && std::isfinite(value) &&
                          std::fabs(value) <= 1000000.0f;
  return frame.flags == kPhysicsRecordFrameFlags && frame.reserved == 0 &&
         (legacy || barrel) &&
         AxisBitsValid(FloatBits(frame.steering), 1.0f) &&
         AxisBitsValid(FloatBits(frame.brake), 1.05f) &&
         FloatBits(frame.accelerator) == 0 &&
         frame.nitro_activation_count <= 2 && frame.respawn_button_press == 0 &&
         std::memcmp(frame.padding, zero.padding, sizeof(frame.padding)) == 0 &&
         ((legacy &&
           std::memcmp(frame.barrel_angular_velocity,
                       zero.barrel_angular_velocity,
                       sizeof(frame.barrel_angular_velocity)) == 0 &&
           std::memcmp(frame.barrel_rbx, zero.barrel_rbx,
                       sizeof(frame.barrel_rbx)) == 0) ||
          barrel_values_valid) &&
         RawPhysicsBitsValid(frame.transform_bits,
                             frame.linear_velocity_bits);
}

inline Result CapturePhysicsSnapshot(
    std::uint64_t tick, const std::uint8_t* transform,
    const std::uint8_t* linear, PhysicsSnapshotV1* snapshot) noexcept {
  if (snapshot == nullptr || transform == nullptr || linear == nullptr)
    return Result::kInvalidArgument;
  if (snapshot->captured) return Result::kWrongOrder;
  if (!RawPhysicsBitsValid(transform, linear))
    return Result::kInvalidNaturalValue;
  PhysicsSnapshotV1 output{};
  output.tick = tick;
  std::memcpy(output.transform_bits, transform, sizeof(output.transform_bits));
  std::memcpy(output.linear_velocity_bits, linear,
              sizeof(output.linear_velocity_bits));
  output.captured = true;
  *snapshot = output;
  return Result::kReady;
}

// AluTasV2's FinalRacerTransformWriter detour compares the natural state after
// original and only corrects when it diverges.  Transform and linear velocity
// form one semantic correction: if either differs, both recorded ranges are
// published together by the in-process caller.
inline Result DecidePhysicsCorrection(
    const StateV1& state, const std::uint8_t* natural_transform,
    const std::uint8_t* natural_linear,
    PhysicsCorrectionDecisionV1* decision) noexcept {
  if (decision == nullptr || natural_transform == nullptr ||
      natural_linear == nullptr)
    return Result::kInvalidArgument;
  if (!state.tick_open || !state.replay_packet_present)
    return Result::kWrongOrder;
  if (state.packet.tick != state.tick || !PacketValid(state.packet))
    return Result::kInvalidPacket;
  if (!RawPhysicsBitsValid(natural_transform, natural_linear))
    return Result::kInvalidNaturalValue;

  PhysicsCorrectionDecisionV1 output{.tick = state.tick};
  if ((state.packet.skip_override_flags &
       recording::kSkipTransformForced) != 0) {
    output.kind = PhysicsCorrectionKindV1::kSkipped;
  } else if (ComponentFloatRangesEqual(
                 natural_transform, state.packet.transform_bits,
                 recording::kTransformSize) &&
             ComponentFloatRangesEqual(
                 natural_linear, state.packet.linear_velocity_bits,
                 recording::kLinearVelocitySize)) {
    output.kind = PhysicsCorrectionKindV1::kNaturalEqual;
  } else {
    output.kind = PhysicsCorrectionKindV1::kCorrectBoth;
  }
  *decision = output;
  return Result::kReady;
}

inline bool IntervalViewValid(const IntervalReplayViewV1& view) noexcept {
  return view.sample_count == 0 || view.samples != nullptr;
}

// Diagnostic groups are dense for legacy files and may be absent for an
// explicitly supported interpolation-only logical packet. Ordinals remain
// contiguous within each present group; missing groups never invent samples.
inline bool IntervalSequenceValid(const IntervalReplayViewV1& view,
                                 std::uint64_t frame_count,
                                 bool allow_sparse = false) noexcept {
  if (!IntervalViewValid(view) || frame_count == 0) return false;
  if (view.sample_count == 0) return allow_sparse;
  std::uint64_t last_tick = 0;
  std::uint64_t next_ordinal = 0;
  for (std::size_t i = 0; i < view.sample_count; ++i) {
    const auto& sample = view.samples[i];
    if (sample.tick >= frame_count || !IntervalBitsValid(sample.output_bits))
      return false;
    if (i == 0 || sample.tick != last_tick) {
      if ((i != 0 && sample.tick <= last_tick) ||
          (!allow_sparse && sample.tick != (i == 0 ? 0 : last_tick + 1)))
        return false;
      last_tick = sample.tick;
      next_ordinal = 0;
    }
    if (sample.ordinal != next_ordinal++) return false;
  }
  return allow_sparse || last_tick == frame_count - 1;
}

inline std::uint64_t CurrentControlPair(const StateV1& state) noexcept {
  return static_cast<std::uint64_t>(state.setter_cache.brake_bits) |
         (static_cast<std::uint64_t>(state.setter_cache.steering_bits) << 32);
}

inline Result ConsumeSteeringBeforeOriginal(
    StateV1* state, std::uint32_t natural_bits,
    SetterDecisionV1* decision) noexcept {
  if (state == nullptr || decision == nullptr) return Result::kInvalidArgument;
  if (!AxisBitsValid(natural_bits, 1.0f)) return Result::kInvalidNaturalValue;
  if (state->setter_cache.steering_calls ==
          std::numeric_limits<std::uint32_t>::max() ||
      state->setter_cache.event_sequence ==
          std::numeric_limits<std::uint64_t>::max())
    return Result::kOverflow;
  std::uint32_t final_bits = natural_bits;
  const bool override = state->tick_open && state->replay_packet_present &&
      (state->packet.skip_override_flags & recording::kSkipSteer) == 0;
  if (override) final_bits = FloatBits(state->packet.steering);
  state->setter_cache.steering_bits = final_bits;
  ++state->setter_cache.steering_calls;
  ++state->setter_cache.event_sequence;
  if (state->tick_open) {
    state->receipt.steering_bits = final_bits;
    state->receipt.steering_observed = true;
    ++state->receipt.steering_calls;
    state->receipt.consumed_control_pair = CurrentControlPair(*state);
  }
  *decision = {natural_bits, final_bits, override, state->tick_open};
  return Result::kReady;
}

inline Result ConsumeBrakeBeforeOriginal(
    StateV1* state, std::uint32_t natural_bits,
    SetterDecisionV1* decision) noexcept {
  if (state == nullptr || decision == nullptr) return Result::kInvalidArgument;
  if (!AxisBitsValid(natural_bits, 1.05f)) return Result::kInvalidNaturalValue;
  if (state->setter_cache.brake_calls ==
          std::numeric_limits<std::uint32_t>::max() ||
      state->setter_cache.event_sequence ==
          std::numeric_limits<std::uint64_t>::max())
    return Result::kOverflow;
  std::uint32_t final_bits = natural_bits;
  const bool override = state->tick_open && state->replay_packet_present &&
      (state->packet.skip_override_flags & recording::kSkipBrake) == 0;
  if (override) final_bits = FloatBits(state->packet.brake);
  state->setter_cache.brake_bits = final_bits;
  ++state->setter_cache.brake_calls;
  ++state->setter_cache.event_sequence;
  if (state->tick_open) {
    state->receipt.brake_bits = final_bits;
    state->receipt.brake_observed = true;
    ++state->receipt.brake_calls;
    state->receipt.consumed_control_pair = CurrentControlPair(*state);
  }
  *decision = {natural_bits, final_bits, override, state->tick_open};
  return Result::kReady;
}

inline Result BeginTick(StateV1* state, std::uint64_t tick,
                        const recording::RecordingFrameV1* selected_packet,
                        std::uint64_t natural_control_pair,
                        std::uint32_t fixed_delta_us,
                        PrePhysicsDecisionV1* decision) noexcept {
  if (state == nullptr || decision == nullptr ||
      fixed_delta_us < recording::kMinimumFixedIntervalUs ||
      fixed_delta_us > recording::kMaximumFixedIntervalUs)
    return Result::kInvalidArgument;
  if (state->tick_open) return Result::kWrongOrder;
  if (selected_packet != nullptr &&
      (!PacketValid(*selected_packet) || selected_packet->tick != tick))
    return Result::kInvalidPacket;

  const std::uint32_t natural_brake =
      static_cast<std::uint32_t>(natural_control_pair);
  const std::uint32_t natural_steering =
      static_cast<std::uint32_t>(natural_control_pair >> 32);
  if (!AxisBitsValid(natural_brake, 1.05f) ||
      !AxisBitsValid(natural_steering, 1.0f))
    return Result::kInvalidNaturalValue;

  state->tick_open = true;
  state->replay_packet_present = selected_packet != nullptr;
  state->tick = tick;
  state->interval_ordinal = 0;
  state->packet = selected_packet == nullptr
                      ? recording::RecordingFrameV1{}
                      : *selected_packet;
  state->receipt = {};
  state->receipt.tick = tick;
  state->receipt.natural_control_pair = natural_control_pair;
  state->receipt.replay_packet_present = selected_packet != nullptr;
  state->receipt.fixed_delta_us = fixed_delta_us;
  state->setter_cache.brake_bits = natural_brake;
  state->setter_cache.steering_bits = natural_steering;
  state->receipt.brake_bits = natural_brake;
  state->receipt.steering_bits = natural_steering;
  state->receipt.accelerator_bits = state->setter_cache.accelerator_bits;
  state->receipt.setter_sequence_at_begin =
      state->setter_cache.event_sequence;
  state->receipt.consumed_control_pair = natural_control_pair;

  std::uint32_t nitro_calls = 0;
  if (selected_packet != nullptr &&
      (selected_packet->skip_override_flags &
       recording::kSkipNitroActivation) == 0)
    nitro_calls = selected_packet->nitro_activation_count;

  *decision = {
      .tick = tick,
      .natural_control_pair = natural_control_pair,
      .consumed_control_pair = natural_control_pair,
      .fixed_delta_us = static_cast<std::int64_t>(fixed_delta_us),
      .replay_nitro_calls = nitro_calls,
      .replay_packet_present = selected_packet != nullptr,
      // AluTasV2 applies steering/brake overrides inside the corresponding
      // natural setter detours.  A pre-physics pair write is not equivalent.
      .write_control_pair = false,
      .write_fixed_delta = true,
  };
  return Result::kReady;
}

// Mirrors AluTasV2 AcceleratorValue: the game original runs first, then the
// member value is conditionally replaced and the final consumed value records.
inline Result ConsumeAcceleratorAfterOriginal(StateV1* state,
                                              std::uint32_t natural_bits,
                                              std::uint32_t* final_bits) noexcept {
  if (state == nullptr || final_bits == nullptr) return Result::kInvalidArgument;
  if (!AxisBitsValid(natural_bits, 1.05f))
    return Result::kInvalidNaturalValue;
  if (state->setter_cache.accelerator_calls ==
          std::numeric_limits<std::uint32_t>::max() ||
      state->setter_cache.event_sequence ==
          std::numeric_limits<std::uint64_t>::max())
    return Result::kOverflow;
  std::uint32_t consumed = natural_bits;
  if (state->tick_open && state->replay_packet_present &&
      (state->packet.skip_override_flags & recording::kSkipAccelerator) == 0)
    consumed = FloatBits(state->packet.accelerator);
  state->setter_cache.accelerator_bits = consumed;
  ++state->setter_cache.accelerator_calls;
  ++state->setter_cache.event_sequence;
  if (state->tick_open) {
    state->receipt.accelerator_bits = consumed;
    state->receipt.accelerator_observed = true;
    ++state->receipt.accelerator_calls;
  }
  *final_bits = consumed;
  return Result::kReady;
}

// Mirrors AluTasV2 EnableNitro: natural input is suppressed only while an
// exact replay packet is selected.  Natural recording otherwise counts the
// call before forwarding it to the real game function.
inline Result OnNaturalNitroCall(StateV1* state) noexcept {
  if (state == nullptr) return Result::kInvalidArgument;
  if (!state->tick_open) return Result::kWrongOrder;
  if (state->replay_packet_present) {
    if (state->receipt.suppressed_nitro_calls ==
        std::numeric_limits<std::uint32_t>::max())
      return Result::kOverflow;
    ++state->receipt.suppressed_nitro_calls;
    return Result::kSuppressNaturalNitro;
  }
  if (state->receipt.natural_nitro_calls ==
          std::numeric_limits<std::uint32_t>::max() ||
      state->receipt.recorded_nitro_calls ==
          std::numeric_limits<std::uint32_t>::max())
    return Result::kOverflow;
  ++state->receipt.natural_nitro_calls;
  ++state->receipt.recorded_nitro_calls;
  return Result::kForwardNaturalNitro;
}

inline Result AccountInjectedNitroCalls(StateV1* state,
                                        std::uint32_t completed) noexcept {
  if (state == nullptr) return Result::kInvalidArgument;
  if (!state->tick_open || !state->replay_packet_present)
    return Result::kWrongOrder;
  const std::uint32_t planned =
      (state->packet.skip_override_flags & recording::kSkipNitroActivation) == 0
          ? state->packet.nitro_activation_count
          : 0;
  if (completed != planned ||
      completed > std::numeric_limits<std::uint32_t>::max() -
                      state->receipt.recorded_nitro_calls)
    return completed == planned ? Result::kOverflow
                                : Result::kNitroCountMismatch;
  state->receipt.injected_nitro_calls = completed;
  state->receipt.recorded_nitro_calls += completed;
  return Result::kReady;
}

// Called after the real Physics Interval getter. AluTasV2 exposes interval
// override as an independent session option whose default is disabled. Normal
// replay therefore forwards the real getter's dynamic 1/60 or 1/120 result.
// Android recording keeps every call only as diagnostic evidence. EndTick
// validates and consumes each recorded diagnostic group without incorrectly
// turning those observations into replay commands.
inline Result OnPhysicsIntervalAfterOriginal(
    StateV1* state, const IntervalReplayViewV1& replay,
    std::uint32_t original_output_bits, IntervalDecisionV1* decision) noexcept {
  if (state == nullptr || decision == nullptr || !IntervalViewValid(replay))
    return Result::kInvalidArgument;
  if (!state->tick_open) return Result::kWrongOrder;
  if (!IntervalBitsValid(original_output_bits))
    return Result::kInvalidNaturalValue;
  if (state->interval_ordinal == std::numeric_limits<std::uint32_t>::max() ||
      state->receipt.physics_interval_calls ==
          std::numeric_limits<std::uint32_t>::max())
    return Result::kOverflow;

  *decision = {
      .tick = state->tick,
      .ordinal = state->interval_ordinal,
      .original_output_bits = original_output_bits,
      .final_output_bits = original_output_bits,
      .override_after_original = false,
  };
  ++state->interval_ordinal;
  ++state->receipt.physics_interval_calls;
  return Result::kReady;
}

inline Result EndTick(StateV1* state, const IntervalReplayViewV1& replay,
                      TickReceiptV1* receipt,
                      bool allow_zero_integration_updates = false) noexcept {
  if (state == nullptr || receipt == nullptr || !IntervalViewValid(replay))
    return Result::kInvalidArgument;
  if (!state->tick_open) return Result::kWrongOrder;
  if (state->receipt.physics_interval_calls == 0 && !allow_zero_integration_updates)
    return Result::kWrongOrder;
  if (state->replay_packet_present) {
    const std::uint32_t planned_nitro =
        (state->packet.skip_override_flags & recording::kSkipNitroActivation) == 0
            ? state->packet.nitro_activation_count : 0;
    if (state->receipt.injected_nitro_calls != planned_nitro ||
        state->receipt.recorded_nitro_calls != planned_nitro)
      return Result::kNitroCountMismatch;
    if (state->interval_cursor > replay.sample_count)
      return Result::kIntervalMismatch;
    const bool empty_group = state->interval_cursor == replay.sample_count ||
        (state->interval_cursor < replay.sample_count &&
         replay.samples[state->interval_cursor].tick > state->tick);
    if (!allow_zero_integration_updates && empty_group)
      return Result::kIntervalMismatch;
    if (!empty_group) {
      const IntervalSampleV1& first = replay.samples[state->interval_cursor];
      if (first.tick != state->tick || first.ordinal != 0 ||
          !IntervalBitsValid(first.output_bits))
        return Result::kIntervalMismatch;
      std::uint32_t expected_ordinal = 0;
      while (state->interval_cursor < replay.sample_count) {
        const IntervalSampleV1& sample = replay.samples[state->interval_cursor];
        if (sample.tick != state->tick) break;
        if (sample.ordinal != expected_ordinal ||
            !IntervalBitsValid(sample.output_bits))
          return Result::kIntervalMismatch;
        ++expected_ordinal;
        ++state->interval_cursor;
      }
      if (expected_ordinal == 0 ||
          (state->interval_cursor < replay.sample_count &&
           replay.samples[state->interval_cursor].tick <= state->tick))
        return Result::kIntervalMismatch;
    }
  }
  state->receipt.setter_sequence_at_end =
      state->setter_cache.event_sequence;
  *receipt = state->receipt;
  state->tick_open = false;
  state->replay_packet_present = false;
  state->packet = {};
  state->receipt = {};
  state->interval_ordinal = 0;
  return Result::kReady;
}

inline Result DiscardOpenTickForRaceEnd(StateV1* state) noexcept {
  if (state == nullptr) return Result::kInvalidArgument;
  if (!state->tick_open) return Result::kWrongOrder;
  state->tick_open = false;
  state->replay_packet_present = false;
  state->packet = {};
  state->receipt = {};
  state->interval_ordinal = 0;
  return Result::kReady;
}

// Convert one already-closed authoritative tick into the common recording
// packet without inventing values owned by later stages.  The skip mask is the
// semantic receipt for those deliberately absent fields.
inline Result BuildActionRecordingFrame(
    const TickReceiptV1& receipt, std::uint64_t monotonic_ns,
    recording::RecordingFrameV1* frame,
    bool allow_zero_integration_updates = false) noexcept {
  if (frame == nullptr || receipt.replay_packet_present ||
      receipt.fixed_delta_us < recording::kMinimumFixedIntervalUs ||
      receipt.fixed_delta_us > recording::kMaximumFixedIntervalUs ||
      (receipt.physics_interval_calls == 0 && !allow_zero_integration_updates) ||
      !AxisBitsValid(receipt.steering_bits, 1.0f) ||
      !AxisBitsValid(receipt.brake_bits, 1.05f) ||
      receipt.recorded_nitro_calls > 2 ||
      receipt.recorded_nitro_calls != receipt.natural_nitro_calls ||
      receipt.suppressed_nitro_calls != 0 ||
      receipt.injected_nitro_calls != 0 ||
      receipt.setter_sequence_at_end < receipt.setter_sequence_at_begin ||
      receipt.setter_sequence_at_end - receipt.setter_sequence_at_begin !=
          static_cast<std::uint64_t>(receipt.steering_calls) +
              receipt.brake_calls + receipt.accelerator_calls)
    return Result::kInvalidArgument;

  recording::RecordingFrameV1 output{};
  output.tick = receipt.tick;
  output.monotonic_ns = monotonic_ns;
  output.steering = BitsFloat(receipt.steering_bits);
  output.brake = BitsFloat(receipt.brake_bits);
  output.accelerator = 0.0f;
  output.nitro_activation_count = receipt.recorded_nitro_calls;
  output.skip_override_flags = kActionRecordSkipFlags;
  output.flags = kActionRecordFrameFlags;
  *frame = output;
  return Result::kReady;
}


inline Result BuildPhysicsRecordingFrame(
    const TickReceiptV1& receipt, const PhysicsSnapshotV1& snapshot,
    std::uint64_t monotonic_ns,
    recording::RecordingFrameV1* frame,
    bool allow_zero_integration_updates = false) noexcept {
  if (frame == nullptr || snapshot.tick != receipt.tick ||
      !PhysicsSnapshotValid(snapshot))
    return Result::kInvalidArgument;
  recording::RecordingFrameV1 output{};
  const Result action = BuildActionRecordingFrame(receipt, monotonic_ns,
                                                   &output, allow_zero_integration_updates);
  if (action != Result::kReady) return action;
  std::memcpy(output.transform_bits, snapshot.transform_bits,
              sizeof(output.transform_bits));
  std::memcpy(output.linear_velocity_bits, snapshot.linear_velocity_bits,
              sizeof(output.linear_velocity_bits));
  output.skip_override_flags = kPhysicsRecordSkipFlags;
  output.flags = kPhysicsRecordFrameFlags;
  if (!PhysicsRecordingFrameValid(output)) return Result::kInvalidArgument;
  *frame = output;
  return Result::kReady;
}

}  // namespace a9tas::g4_input_action_v1
