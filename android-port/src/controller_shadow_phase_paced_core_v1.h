#pragma once

// Pure decision core for the controller-shadow replay successor.
//
// Unlike the superseded single-address M1 core, this core does not infer a
// gameplay frame from repeated accumulator accesses.  It reuses the exact
// boundary topology proven by the 900-frame unified executor:
//
//   positive delta -> C98 -> C9C -> world commit
//
// The external executor may write only the fixed-delta value.  Controller
// input, natural actions, and final 64+12 correction remain game-owned.

#include "controller_shadow_coordinator_protocol_v1.h"
#include "in_process_tick_coordinator_v1.h"

#include <cstdint>
#include <cstring>

namespace a9tas::controller_shadow_phase_paced_v1 {

namespace payload = a9tas::controller_shadow_coordinator_v1;
namespace coordinator = a9tas::in_process_tick_coordinator_v1;

enum class Stage : std::uint32_t {
  kWaitingForGameplay = 0,
  kAwaitDelta = 1,
  kAwaitC98 = 2,
  kAwaitC9C = 3,
  kAwaitWorldCommit = 4,
  kComplete = 5,
  kFaulted = 6,
};

enum class Boundary : std::uint32_t {
  kDelta = 0,
  kC98 = 1,
  kC9C = 2,
  kWorldCommit = 3,
};

enum class Action : std::uint32_t {
  kNone = 0,
  kWriteFixedDelta = 1,
  // The game-owned controller commits frame N-1 only when frame N begins.
  // At the final world commit its cursor is therefore intentionally
  // selected=N/completed=N-1.  Validate that final-in-flight state without
  // creating a synthetic N+1 controller call.
  kFreezeAndValidateFinalInFlight = 2,
  kFreezeAndRollback = 3,
};

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kInvalidDelta = -2,
  kEvidenceInvalid = -3,
  kEvidenceRegressed = -4,
  kPayloadFault = -5,
  kBoundaryOrder = -6,
  kBoundaryOwner = -7,
  kReceiptLag = -8,
};

struct Config {
  std::uint32_t frame_count{};
  std::uint32_t fixed_interval_us{};
};

struct Receipts {
  payload::Evidence controller{};
  std::uint64_t writer_processed_frames{};
  std::uint64_t action_completed_sequence{};
};

struct State {
  Stage stage{Stage::kWaitingForGameplay};
  Result last_result{Result::kOk};
  std::uint64_t events{};
  std::uint64_t delta_events{};
  std::uint64_t c98_events{};
  std::uint64_t c9c_events{};
  std::uint64_t world_commit_events{};
  std::uint64_t duplicate_delta_events{};
  std::uint64_t fixed_delta_requests{};
  std::uint64_t committed_frames{};
  std::uint64_t last_wrapper_entries{};
  std::uint64_t last_selected_frames{};
  std::uint64_t last_completed_frames{};
  std::uint64_t last_writer_processed{};
  std::uint64_t last_action_completed{};
  std::int32_t cycle_owner_tid{};
};

struct Decision {
  Action action{Action::kNone};
  std::int64_t write_value{};
  std::uint32_t frame_index{};
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
    state->stage = Stage::kFaulted;
    state->last_result = result;
  }
  if (decision != nullptr) {
    decision->action = Action::kFreezeAndRollback;
    decision->write_value = 0;
  }
  return result;
}

inline Result ValidateReceipts(const Config& config, const Receipts& receipts,
                               State* state, Decision* decision) noexcept {
  const auto& evidence = receipts.controller;
  if (!EvidenceHeaderValid(evidence) ||
      evidence.selected_frames > config.frame_count ||
      evidence.completed_frames > config.frame_count ||
      evidence.next_frame > config.frame_count ||
      evidence.original_returns > evidence.original_calls ||
      evidence.source_restores > evidence.source_swaps ||
      evidence.selected_frames > evidence.source_swaps ||
      evidence.completed_frames > evidence.selected_frames ||
      receipts.writer_processed_frames > config.frame_count ||
      receipts.action_completed_sequence > config.frame_count)
    return Fault(state, Result::kEvidenceInvalid, decision);
  if (evidence.wrapper_entries < state->last_wrapper_entries ||
      evidence.selected_frames < state->last_selected_frames ||
      evidence.completed_frames < state->last_completed_frames ||
      receipts.writer_processed_frames < state->last_writer_processed ||
      receipts.action_completed_sequence < state->last_action_completed)
    return Fault(state, Result::kEvidenceRegressed, decision);
  if (evidence.failures != 0 || evidence.recursive_entries != 0 ||
      evidence.last_status < 0 || evidence.last_coordinator_result < 0)
    return Fault(state, Result::kPayloadFault, decision);
  state->last_wrapper_entries = evidence.wrapper_entries;
  state->last_selected_frames = evidence.selected_frames;
  state->last_completed_frames = evidence.completed_frames;
  state->last_writer_processed = receipts.writer_processed_frames;
  state->last_action_completed = receipts.action_completed_sequence;
  return Result::kOk;
}

inline Result Observe(const Config& config, Boundary boundary,
                      std::int64_t observed_delta_us,
                      std::uint32_t lifecycle_state, std::int32_t tid,
                      const Receipts& receipts, State* state,
                      Decision* decision) noexcept {
  if (!ConfigValid(config) || state == nullptr || decision == nullptr ||
      tid <= 0 || state->stage == Stage::kFaulted ||
      state->stage == Stage::kComplete)
    return Result::kInvalidArgument;
  *decision = {};
  decision->frame_index =
      static_cast<std::uint32_t>(state->fixed_delta_requests);
  ++state->events;
  if (boundary == Boundary::kC98)
    ++state->c98_events;
  else if (boundary == Boundary::kC9C)
    ++state->c9c_events;
  else if (boundary == Boundary::kWorldCommit)
    ++state->world_commit_events;

  const Result receipt_result =
      ValidateReceipts(config, receipts, state, decision);
  if (receipt_result != Result::kOk) return receipt_result;

  if (boundary == Boundary::kDelta) {
    ++state->delta_events;
    if (observed_delta_us < 0 || observed_delta_us > 1000000)
      return Fault(state, Result::kInvalidDelta, decision);
  }

  // The countdown/paused state generates the same accumulator traffic as the
  // race.  It is completely write-neutral and cannot open a phase cycle.
  if (lifecycle_state == 2u) {
    if (state->stage != Stage::kWaitingForGameplay &&
        state->stage != Stage::kAwaitDelta)
      return Fault(state, Result::kBoundaryOrder, decision);
    state->stage = Stage::kWaitingForGameplay;
    return state->last_result = Result::kOk;
  }
  if (lifecycle_state != 3u)
    return Fault(state, Result::kEvidenceInvalid, decision);
  if (state->stage == Stage::kWaitingForGameplay)
    state->stage = Stage::kAwaitDelta;

  // Match the live-proven unified executor's incomplete-prefix rule: an
  // initial attach or a newly discovered thread may expose C98/C9C/world tail
  // events before this executor has opened a delta-owned frame.  They consume
  // no replay state and are ignored only while no request is active.
  if (state->stage == Stage::kAwaitDelta &&
      boundary != Boundary::kDelta)
    return state->last_result = Result::kOk;

  if (boundary == Boundary::kDelta) {
    if (observed_delta_us == 0) return state->last_result = Result::kOk;
    if (state->stage != Stage::kAwaitDelta) {
      ++state->duplicate_delta_events;
      return state->last_result = Result::kOk;
    }
    const bool controller_cursor_ready =
        state->committed_frames == 0
            ? receipts.controller.selected_frames == 0 &&
                  receipts.controller.completed_frames == 0
            : receipts.controller.selected_frames == state->committed_frames &&
                  receipts.controller.completed_frames + 1u ==
                      state->committed_frames;
    if (state->fixed_delta_requests != state->committed_frames ||
        state->fixed_delta_requests >= config.frame_count ||
        !controller_cursor_ready ||
        receipts.writer_processed_frames != state->committed_frames ||
        receipts.action_completed_sequence != state->committed_frames)
      return Fault(state, Result::kReceiptLag, decision);
    ++state->fixed_delta_requests;
    state->cycle_owner_tid = tid;
    state->stage = Stage::kAwaitC98;
    decision->action = Action::kWriteFixedDelta;
    decision->write_value = config.fixed_interval_us;
    decision->frame_index =
        static_cast<std::uint32_t>(state->fixed_delta_requests - 1u);
    return state->last_result = Result::kOk;
  }

  if (boundary == Boundary::kC98) {
    if (state->stage != Stage::kAwaitC98)
      return Fault(state, Result::kBoundaryOrder, decision);
    if (tid != state->cycle_owner_tid)
      return Fault(state, Result::kBoundaryOwner, decision);
    if (receipts.controller.selected_frames != state->fixed_delta_requests ||
        receipts.controller.completed_frames >=
            receipts.controller.selected_frames)
      return Fault(state, Result::kReceiptLag, decision);
    state->stage = Stage::kAwaitC9C;
    return state->last_result = Result::kOk;
  }

  if (boundary == Boundary::kC9C) {
    if (state->stage != Stage::kAwaitC9C)
      return Fault(state, Result::kBoundaryOrder, decision);
    if (tid != state->cycle_owner_tid)
      return Fault(state, Result::kBoundaryOwner, decision);
    if (receipts.controller.selected_frames != state->fixed_delta_requests)
      return Fault(state, Result::kReceiptLag, decision);
    state->stage = Stage::kAwaitWorldCommit;
    return state->last_result = Result::kOk;
  }

  if (boundary != Boundary::kWorldCommit ||
      state->stage != Stage::kAwaitWorldCommit)
    return Fault(state, Result::kBoundaryOrder, decision);
  if (tid == state->cycle_owner_tid)
    return Fault(state, Result::kBoundaryOwner, decision);
  if (receipts.controller.selected_frames != state->fixed_delta_requests ||
      receipts.controller.completed_frames + 1u !=
          state->fixed_delta_requests ||
      receipts.writer_processed_frames != state->fixed_delta_requests ||
      receipts.action_completed_sequence != state->fixed_delta_requests)
    return Fault(state, Result::kReceiptLag, decision);

  state->committed_frames = state->fixed_delta_requests;
  state->cycle_owner_tid = 0;
  if (state->committed_frames == config.frame_count) {
    state->stage = Stage::kComplete;
    decision->action = Action::kFreezeAndValidateFinalInFlight;
  } else {
    state->stage = Stage::kAwaitDelta;
  }
  decision->frame_index = static_cast<std::uint32_t>(state->committed_frames);
  return state->last_result = Result::kOk;
}

}  // namespace a9tas::controller_shadow_phase_paced_v1
