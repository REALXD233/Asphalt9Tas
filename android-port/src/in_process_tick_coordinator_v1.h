#pragma once

// Pure in-process replay cursor for the controller-shadow migration.
//
// The game-owned GameplayInputController::UpdatePerTick wrapper calls
// BeginTick exactly once per active gameplay tick.  The coordinator publishes
// the natural-action command and arms the existing final-writer permit for the
// same A9UTK1 frame.  On the next UpdatePerTick call it first requires both
// receipts, which also proves that the previous physics/world cycle returned.
//
// This file deliberately contains no hook installation, process access,
// address discovery, fixed-delta write, control-value substitution or game
// function call.  Those remain separately reviewable runtime mechanisms.

#include "final_writer_replay_protocol_v1.h"
#include "natural_action_replay_transport_v1.h"

#include <cstdint>
#include <cstring>

namespace a9tas::in_process_tick_coordinator_v1 {

namespace action = a9tas::natural_action_replay_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;
namespace writer = a9tas::final_writer_replay_v1;

enum class Phase : std::uint32_t {
  kReady = 0,
  kInFlight = 1,
  kComplete = 2,
  kFaulted = 3,
};

enum class Result : std::int32_t {
  kFrameSelected = 1,
  kComplete = 2,
  kInvalidArgument = -1,
  kWrongProducerThread = -2,
  kWrongPhase = -3,
  kWriterControlInvalid = -4,
  kWriterEvidenceInvalid = -5,
  kPreviousWriterPending = -6,
  kPreviousActionPending = -7,
  kFrameInvalid = -8,
  kActionPublishFailed = -9,
  kWriterPermitBusy = -10,
  kWriterPermitVerificationFailed = -11,
};

struct Config {
  std::uint32_t frame_count{};
  std::uint32_t session_id{};
  std::uintptr_t vehicle_owner{};
  std::uint32_t producer_tid{};
  const recording::RecordingFrameV1* frames{};
  writer::Control* writer_control{};
  const writer::Evidence* writer_evidence{};
  mailbox::Mailbox* action_mailbox{};
};

struct State {
  std::uint32_t next_index{};
  std::uint32_t selected_index{};
  Phase phase{Phase::kReady};
  Result last_result{Result::kFrameSelected};
};

inline bool WriterControlValid(const Config& config) noexcept {
  if (config.writer_control == nullptr) return false;
  const writer::Control& control = *config.writer_control;
  return std::memcmp(control.magic, writer::kControlMagic,
                     sizeof(writer::kControlMagic)) == 0 &&
         control.version == writer::kProtocolVersion &&
         control.size == sizeof(writer::Control) &&
         control.flags ==
             (writer::kControlConfigured | writer::kControlTargetsLoaded) &&
         control.frame_count == config.frame_count &&
         control.expected_object != 0 && control.original_vptr != 0 &&
         control.shadow_vptr != 0 && control.original_callback != 0 &&
         control.native_pose != 0 && control.native_linear != 0;
}

inline bool WriterEvidenceValid(const Config& config) noexcept {
  if (config.writer_evidence == nullptr) return false;
  const writer::Evidence& evidence = *config.writer_evidence;
  return std::memcmp(evidence.magic, writer::kEvidenceMagic,
                     sizeof(writer::kEvidenceMagic)) == 0 &&
         evidence.version == writer::kProtocolVersion &&
         evidence.size == sizeof(writer::Evidence) && evidence.failures == 0 &&
         evidence.recursive_entries == 0 &&
         evidence.processed_frames <= config.frame_count;
}

inline bool ConfigValid(const Config& config) noexcept {
  return config.frame_count != 0 &&
         config.frame_count <= writer::kMaximumFrames &&
         config.frame_count <= recording::kMaximumFrames &&
         config.session_id != 0 && config.vehicle_owner != 0 &&
         config.producer_tid != 0 && config.frames != nullptr &&
         config.action_mailbox != nullptr &&
         mailbox::MailboxValid(*config.action_mailbox) &&
         WriterControlValid(config) && WriterEvidenceValid(config);
}

inline Result Fault(State* state, Result result) noexcept {
  if (state != nullptr) {
    state->phase = Phase::kFaulted;
    state->last_result = result;
  }
  return result;
}

inline std::uint32_t ExpectedActionCalls(
    const recording::RecordingFrameV1& frame) noexcept {
  return (frame.skip_override_flags & recording::kSkipNitroActivation) != 0
             ? 0u
             : frame.nitro_activation_count;
}

inline Result CommitPrevious(const Config& config, State* state) noexcept {
  if (state == nullptr || state->phase != Phase::kInFlight ||
      state->selected_index != state->next_index)
    return Fault(state, Result::kWrongPhase);
  if (!WriterEvidenceValid(config))
    return Fault(state, Result::kWriterEvidenceInvalid);

  const std::uint32_t expected_processed = state->next_index + 1u;
  const std::uint32_t observed_processed = __atomic_load_n(
      &config.writer_evidence->processed_frames, __ATOMIC_ACQUIRE);
  const std::uint64_t observed_permit = __atomic_load_n(
      &config.writer_control->reserved[0], __ATOMIC_ACQUIRE);
  if (observed_processed != expected_processed ||
      observed_permit != writer::kFramePermitDisarmed)
    return Fault(state, Result::kPreviousWriterPending);

  const recording::RecordingFrameV1& frame =
      config.frames[state->next_index];
  if (!mailbox::CompletionMatches(
          *config.action_mailbox,
          static_cast<std::uint64_t>(state->next_index) + 1u,
          state->next_index, ExpectedActionCalls(frame)))
    return Fault(state, Result::kPreviousActionPending);

  ++state->next_index;
  state->selected_index = state->next_index;
  state->phase = state->next_index == config.frame_count ? Phase::kComplete
                                                         : Phase::kReady;
  return state->phase == Phase::kComplete ? Result::kComplete
                                          : Result::kFrameSelected;
}

// Select and publish one complete authoritative frame.  The caller must
// validate every runtime object/pointer used for control substitution before
// calling this function, because publication intentionally cannot be rolled
// back after a natural callback has observed the selector.
inline Result BeginTick(const Config& config, State* state,
                        std::uint32_t actual_producer_tid,
                        std::uint32_t* selected_index) noexcept {
  if (state == nullptr || selected_index == nullptr || !ConfigValid(config))
    return Fault(state, Result::kInvalidArgument);
  if (state->phase == Phase::kFaulted)
    return state->last_result = Result::kWrongPhase;
  if (actual_producer_tid != config.producer_tid)
    return Fault(state, Result::kWrongProducerThread);

  if (state->phase == Phase::kInFlight) {
    const Result committed = CommitPrevious(config, state);
    if (committed == Result::kComplete) {
      state->last_result = committed;
      return committed;
    }
    if (state->phase == Phase::kFaulted) return committed;
  }
  if (state->phase == Phase::kComplete)
    return state->last_result = Result::kComplete;
  if (state->phase != Phase::kReady ||
      state->next_index >= config.frame_count)
    return Fault(state, Result::kWrongPhase);

  if (!WriterControlValid(config))
    return Fault(state, Result::kWriterControlInvalid);
  if (!WriterEvidenceValid(config) ||
      __atomic_load_n(&config.writer_evidence->processed_frames,
                      __ATOMIC_ACQUIRE) != state->next_index)
    return Fault(state, Result::kWriterEvidenceInvalid);
  if (__atomic_load_n(&config.writer_control->reserved[0],
                      __ATOMIC_ACQUIRE) != writer::kFramePermitDisarmed)
    return Fault(state, Result::kWriterPermitBusy);

  const recording::RecordingFrameV1& frame =
      config.frames[state->next_index];
  mailbox::Command command{};
  if (!action::BuildFrameCommand(
          frame, static_cast<std::uint64_t>(state->next_index) + 1u,
          config.session_id, config.vehicle_owner, config.producer_tid,
          &command))
    return Fault(state, Result::kFrameInvalid);

  // Publish the action first.  Both consumers are later in the same game-owned
  // cycle, but this order guarantees that an armed final writer never exists
  // without its matching immutable action packet.
  if (mailbox::Publish(config.action_mailbox, command) !=
      mailbox::Result::kPublished)
    return Fault(state, Result::kActionPublishFailed);

  const std::uint64_t permit = writer::FramePermit(state->next_index);
  __atomic_store_n(&config.writer_control->reserved[0], permit,
                   __ATOMIC_RELEASE);
  if (__atomic_load_n(&config.writer_control->reserved[0],
                      __ATOMIC_ACQUIRE) != permit)
    return Fault(state, Result::kWriterPermitVerificationFailed);

  state->selected_index = state->next_index;
  state->phase = Phase::kInFlight;
  state->last_result = Result::kFrameSelected;
  *selected_index = state->selected_index;
  return Result::kFrameSelected;
}

}  // namespace a9tas::in_process_tick_coordinator_v1
