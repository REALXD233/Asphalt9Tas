#pragma once

// Pure host decision core for M1.  The external controller observes only the
// MainTimeSource fixed-delta watchpoint.  It never selects replay frames and
// never publishes controls/actions/writer permits; those belong exclusively
// to the game-owned controller wrapper.

#include "controller_shadow_coordinator_protocol_v1.h"
#include "in_process_tick_coordinator_v1.h"

#include <cstdint>
#include <cstring>

namespace a9tas::m1_delta_only_executor_core_v1 {

namespace payload = a9tas::controller_shadow_coordinator_v1;
namespace coordinator = a9tas::in_process_tick_coordinator_v1;

enum class Phase : std::uint32_t {
  kWaitingForGameplay = 0,
  kRunning = 1,
  kCompletionPending = 2,
  kFaulted = 3,
};

enum class Action : std::uint32_t {
  kNone = 0,
  kWriteFixedDelta = 1,
  kFreezeAndValidateCompletion = 2,
  kFreezeAndRollback = 3,
  // The final controller frame has returned and the previous outer zero reset
  // armed completion-pending.  At the next positive accumulator boundary the
  // downstream consumers must be complete.  Freeze before writing that delta,
  // so no following gameplay tick or sixth fixed-delta write is created.
  kFreezeAndValidateFinalInFlight = 4,
};

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kInvalidDelta = -2,
  kEvidenceInvalid = -3,
  kEvidenceRegressed = -4,
  kPayloadFault = -5,
  kCompletionInconsistent = -6,
};

struct Config {
  std::uint32_t frame_count{};
  std::uint32_t fixed_interval_us{};
};

struct State {
  Phase phase{Phase::kWaitingForGameplay};
  Result last_result{Result::kOk};
  std::uint64_t delta_hits{};
  std::uint64_t positive_delta_hits{};
  std::uint64_t fixed_delta_requests{};
  // Highest fixed-delta request whose controller selection was followed by
  // the matching outer zero close.  A new request is legal only when this
  // cursor has caught up, preventing repeated positive accumulator accesses
  // from outrunning the game-owned controller wrapper.
  std::uint64_t closed_delta_requests{};
  std::uint64_t zero_delta_hits{};
  std::uint64_t non_racing_delta_hits{};
  std::uint64_t last_wrapper_entries{};
  std::uint64_t last_selected_frames{};
  std::uint64_t last_completed_frames{};
};

struct Decision {
  Action action{Action::kNone};
  std::int64_t write_value{};
  std::uint32_t observed_next_frame{};
};

inline bool ConfigValid(const Config& config) noexcept {
  return config.frame_count != 0 &&
         config.frame_count <= payload::kMaximumFrames &&
         config.fixed_interval_us >= 1000 &&
         config.fixed_interval_us <= 100000;
}

inline bool EvidenceHeaderValid(const payload::Evidence& evidence) noexcept {
  return std::memcmp(evidence.magic, payload::kEvidenceMagic,
                     sizeof(payload::kEvidenceMagic)) == 0 &&
         evidence.version == payload::kProtocolVersion &&
         evidence.size == sizeof(payload::Evidence);
}

inline Result Fault(State* state, Result result, Decision* decision) noexcept {
  if (state != nullptr) {
    state->phase = Phase::kFaulted;
    state->last_result = result;
  }
  if (decision != nullptr) {
    decision->action = Action::kFreezeAndRollback;
    decision->write_value = 0;
  }
  return result;
}

inline Result ObserveDelta(const Config& config, std::int64_t observed_delta_us,
                           std::uint32_t lifecycle_state,
                           const payload::Evidence& evidence, State* state,
                           Decision* decision) noexcept {
  if (!ConfigValid(config) || state == nullptr || decision == nullptr ||
      state->phase == Phase::kFaulted)
    return Result::kInvalidArgument;
  *decision = {};
  ++state->delta_hits;
  if (observed_delta_us < 0 || observed_delta_us > 1000000)
    return Fault(state, Result::kInvalidDelta, decision);
  if (!EvidenceHeaderValid(evidence) ||
      evidence.selected_frames > config.frame_count ||
      evidence.completed_frames > config.frame_count ||
      evidence.next_frame > config.frame_count ||
      evidence.original_returns > evidence.original_calls ||
      evidence.source_restores > evidence.source_swaps ||
      evidence.selected_frames > evidence.source_swaps ||
      evidence.completed_frames > evidence.selected_frames)
    return Fault(state, Result::kEvidenceInvalid, decision);
  if (evidence.wrapper_entries < state->last_wrapper_entries ||
      evidence.selected_frames < state->last_selected_frames ||
      evidence.completed_frames < state->last_completed_frames)
    return Fault(state, Result::kEvidenceRegressed, decision);
  state->last_wrapper_entries = evidence.wrapper_entries;
  state->last_selected_frames = evidence.selected_frames;
  state->last_completed_frames = evidence.completed_frames;
  decision->observed_next_frame = evidence.next_frame;
  if (evidence.failures != 0 || evidence.recursive_entries != 0 ||
      evidence.last_status < 0 || evidence.last_coordinator_result < 0)
    return Fault(state, Result::kPayloadFault, decision);

  const bool reports_complete =
      evidence.last_status == payload::kComplete ||
      evidence.coordinator_phase ==
          static_cast<std::uint32_t>(coordinator::Phase::kComplete) ||
      evidence.last_coordinator_result ==
          static_cast<std::int32_t>(coordinator::Result::kComplete);
  if (reports_complete) {
    if (evidence.last_status != payload::kComplete ||
        evidence.coordinator_phase !=
            static_cast<std::uint32_t>(coordinator::Phase::kComplete) ||
        evidence.last_coordinator_result !=
            static_cast<std::int32_t>(coordinator::Result::kComplete) ||
        evidence.selected_frames != config.frame_count ||
        evidence.completed_frames != config.frame_count ||
        evidence.next_frame != config.frame_count)
      return Fault(state, Result::kCompletionInconsistent, decision);
    state->phase = Phase::kCompletionPending;
    state->last_result = Result::kOk;
    decision->action = Action::kFreezeAndValidateCompletion;
    return Result::kOk;
  }

  // Controller selection occurs after the accumulator's positive write but
  // before its zero reset.  At the final zero reset the last frame can still
  // be kInFlight: the controller wrapper has returned and restored its source,
  // while the final writer and natural-action consumer have already had their
  // chance to publish receipts.  Waiting for the next UpdatePerTick would
  // create an extra outer tick and, historically, an extra fixed-delta write.
  const bool final_in_flight =
      evidence.last_status == payload::kRunning &&
      evidence.coordinator_phase ==
          static_cast<std::uint32_t>(coordinator::Phase::kInFlight) &&
      evidence.last_coordinator_result ==
          static_cast<std::int32_t>(coordinator::Result::kFrameSelected) &&
      evidence.selected_frames == config.frame_count &&
      evidence.completed_frames + 1u == config.frame_count &&
      evidence.next_frame + 1u == config.frame_count;
  if (lifecycle_state == 3u && observed_delta_us == 0 &&
      evidence.selected_frames == state->fixed_delta_requests &&
      state->closed_delta_requests < state->fixed_delta_requests)
    state->closed_delta_requests = state->fixed_delta_requests;
  if (state->phase == Phase::kCompletionPending) {
    if (!final_in_flight || lifecycle_state != 3u)
      return Fault(state, Result::kCompletionInconsistent, decision);
    if (observed_delta_us == 0) {
      ++state->zero_delta_hits;
      return state->last_result = Result::kOk;
    }
    ++state->positive_delta_hits;
    decision->action = Action::kFreezeAndValidateFinalInFlight;
    return state->last_result = Result::kOk;
  }
  if (observed_delta_us == 0 && final_in_flight) {
    state->phase = Phase::kCompletionPending;
    state->last_result = Result::kOk;
    ++state->zero_delta_hits;
    // ACC_ZERO is earlier than the certified F64/callback-close/world-commit
    // tail on this build.  Let the already-selected final frame finish, then
    // validate at the next positive accumulator access before modifying it.
    decision->action = Action::kNone;
    return Result::kOk;
  }

  state->phase = evidence.selected_frames == 0
                     ? Phase::kWaitingForGameplay
                     : Phase::kRunning;
  if (observed_delta_us == 0) {
    ++state->zero_delta_hits;
    return state->last_result = Result::kOk;
  }
  ++state->positive_delta_hits;
  // The outer accumulator also advances while the race is paused.  The
  // lifecycle object's certified 2 -> 3 transition is therefore mandatory:
  // countdown/paused cycles must remain completely write-neutral.
  if (lifecycle_state == 2u) {
    ++state->non_racing_delta_hits;
    return state->last_result = Result::kOk;
  }
  if (lifecycle_state != 3u)
    return Fault(state, Result::kEvidenceInvalid, decision);
  if (evidence.selected_frames > state->fixed_delta_requests ||
      state->fixed_delta_requests >
          static_cast<std::uint64_t>(evidence.selected_frames) + 1u ||
      state->closed_delta_requests > state->fixed_delta_requests)
    return Fault(state, Result::kCompletionInconsistent, decision);
  // One request may be waiting for the controller to select it, or a selected
  // request may still be waiting for its zero close.  Both states are strictly
  // observation-only.  In particular, reaching frame_count never causes a
  // sixth write while the final controller frame catches up.
  if (state->fixed_delta_requests == config.frame_count ||
      evidence.selected_frames != state->fixed_delta_requests ||
      state->closed_delta_requests != state->fixed_delta_requests)
    return state->last_result = Result::kOk;
  ++state->fixed_delta_requests;
  decision->action = Action::kWriteFixedDelta;
  decision->write_value = config.fixed_interval_us;
  return state->last_result = Result::kOk;
}

}  // namespace a9tas::m1_delta_only_executor_core_v1
