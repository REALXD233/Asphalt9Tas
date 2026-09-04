#include "m1_delta_only_executor_core_v1.h"

#include <cstdint>
#include <cstring>

namespace core = a9tas::m1_delta_only_executor_core_v1;
namespace payload = a9tas::controller_shadow_coordinator_v1;
namespace coordinator = a9tas::in_process_tick_coordinator_v1;

namespace {

payload::Evidence Evidence() {
  payload::Evidence evidence{};
  std::memcpy(evidence.magic, payload::kEvidenceMagic, 8);
  evidence.version = payload::kProtocolVersion;
  evidence.size = sizeof(evidence);
  return evidence;
}

bool DeltaOnlyOwnsOneWrite() {
  core::State state{};
  core::Decision decision{};
  payload::Evidence evidence = Evidence();
  return core::ObserveDelta({5, 16667}, 20000, 3, evidence, &state, &decision) ==
             core::Result::kOk &&
         decision.action == core::Action::kWriteFixedDelta &&
         decision.write_value == 16667 && state.fixed_delta_requests == 1;
}

bool ZeroDeltaNeverWrites() {
  core::State state{};
  core::Decision decision{};
  payload::Evidence evidence = Evidence();
  return core::ObserveDelta({5, 16667}, 0, 3, evidence, &state, &decision) ==
             core::Result::kOk &&
         decision.action == core::Action::kNone &&
         state.fixed_delta_requests == 0;
}

bool CompleteRequestsFrozenValidation() {
  core::State state{};
  core::Decision decision{};
  payload::Evidence evidence = Evidence();
  evidence.wrapper_entries = 6;
  evidence.original_calls = 6;
  evidence.original_returns = 6;
  evidence.source_swaps = 5;
  evidence.source_restores = 5;
  evidence.selected_frames = 5;
  evidence.completed_frames = 5;
  evidence.next_frame = 5;
  evidence.last_status = payload::kComplete;
  evidence.last_coordinator_result =
      static_cast<std::int32_t>(coordinator::Result::kComplete);
  evidence.coordinator_phase =
      static_cast<std::uint32_t>(coordinator::Phase::kComplete);
  return core::ObserveDelta({5, 16667}, 20000, 3, evidence, &state, &decision) ==
             core::Result::kOk &&
         decision.action == core::Action::kFreezeAndValidateCompletion &&
         state.fixed_delta_requests == 0;
}

bool FaultNeverRequestsDelta() {
  core::State state{};
  core::Decision decision{};
  payload::Evidence evidence = Evidence();
  evidence.failures = 1;
  evidence.last_status = payload::kCoordinatorFailure;
  return core::ObserveDelta({5, 16667}, 20000, 3, evidence, &state, &decision) ==
             core::Result::kPayloadFault &&
         decision.action == core::Action::kFreezeAndRollback &&
         state.fixed_delta_requests == 0;
}

bool RegressionFailsClosed() {
  core::State state{};
  state.fixed_delta_requests = 2;
  state.closed_delta_requests = 2;
  core::Decision decision{};
  payload::Evidence evidence = Evidence();
  evidence.wrapper_entries = 2;
  evidence.selected_frames = 2;
  evidence.completed_frames = 1;
  evidence.source_swaps = 2;
  evidence.source_restores = 2;
  if (core::ObserveDelta({5, 16667}, 20000, 3, evidence, &state, &decision) !=
      core::Result::kOk)
    return false;
  evidence.selected_frames = 1;
  return core::ObserveDelta({5, 16667}, 20000, 3, evidence, &state, &decision) ==
             core::Result::kEvidenceRegressed &&
         decision.action == core::Action::kFreezeAndRollback;
}

bool CountdownPositiveDeltaIsWriteNeutral() {
  core::State state{};
  core::Decision decision{};
  payload::Evidence evidence = Evidence();
  return core::ObserveDelta({5, 16667}, 20000, 2, evidence, &state,
                            &decision) == core::Result::kOk &&
         decision.action == core::Action::kNone &&
         state.fixed_delta_requests == 0 &&
         state.non_racing_delta_hits == 1;
}

bool RepeatedPositiveCannotOutrunControllerCycle() {
  core::State state{};
  core::Decision decision{};
  payload::Evidence evidence = Evidence();
  if (core::ObserveDelta({5, 16667}, 20000, 3, evidence, &state,
                         &decision) != core::Result::kOk ||
      decision.action != core::Action::kWriteFixedDelta ||
      state.fixed_delta_requests != 1)
    return false;
  // A duplicate positive access before controller selection is write-neutral.
  if (core::ObserveDelta({5, 16667}, 19000, 3, evidence, &state,
                         &decision) != core::Result::kOk ||
      decision.action != core::Action::kNone ||
      state.fixed_delta_requests != 1)
    return false;
  evidence.wrapper_entries = 1;
  evidence.original_calls = 1;
  evidence.original_returns = 1;
  evidence.source_swaps = 1;
  evidence.source_restores = 1;
  evidence.selected_frames = 1;
  evidence.last_status = payload::kRunning;
  evidence.last_coordinator_result =
      static_cast<std::int32_t>(coordinator::Result::kFrameSelected);
  evidence.coordinator_phase =
      static_cast<std::uint32_t>(coordinator::Phase::kInFlight);
  // Selection alone is insufficient; the cycle must reach its zero close.
  if (core::ObserveDelta({5, 16667}, 18000, 3, evidence, &state,
                         &decision) != core::Result::kOk ||
      decision.action != core::Action::kNone ||
      state.fixed_delta_requests != 1)
    return false;
  if (core::ObserveDelta({5, 16667}, 0, 3, evidence, &state, &decision) !=
          core::Result::kOk ||
      decision.action != core::Action::kNone ||
      state.closed_delta_requests != 1)
    return false;
  return core::ObserveDelta({5, 16667}, 17000, 3, evidence, &state,
                            &decision) == core::Result::kOk &&
         decision.action == core::Action::kWriteFixedDelta &&
         state.fixed_delta_requests == 2;
}

bool FinalInFlightWaitsForNextPositiveWithoutSixthWrite() {
  core::State state{};
  state.fixed_delta_requests = 5;
  state.closed_delta_requests = 4;
  core::Decision decision{};
  payload::Evidence evidence = Evidence();
  evidence.wrapper_entries = 5;
  evidence.original_calls = 5;
  evidence.original_returns = 5;
  evidence.source_swaps = 5;
  evidence.source_restores = 5;
  evidence.selected_frames = 5;
  evidence.completed_frames = 4;
  evidence.next_frame = 4;
  evidence.last_status = payload::kRunning;
  evidence.last_coordinator_result =
      static_cast<std::int32_t>(coordinator::Result::kFrameSelected);
  evidence.coordinator_phase =
      static_cast<std::uint32_t>(coordinator::Phase::kInFlight);
  if (core::ObserveDelta({5, 16667}, 0, 3, evidence, &state, &decision) !=
          core::Result::kOk ||
      decision.action != core::Action::kNone ||
      state.phase != core::Phase::kCompletionPending ||
      state.fixed_delta_requests != 5 || state.closed_delta_requests != 5)
    return false;
  // Extra zero observations cannot close the frame prematurely.
  if (core::ObserveDelta({5, 16667}, 0, 3, evidence, &state, &decision) !=
          core::Result::kOk ||
      decision.action != core::Action::kNone ||
      state.fixed_delta_requests != 5)
    return false;
  // The next positive boundary is observed but never overwritten.
  return core::ObserveDelta({5, 16667}, 20000, 3, evidence, &state,
                            &decision) == core::Result::kOk &&
         decision.action ==
             core::Action::kFreezeAndValidateFinalInFlight &&
         state.fixed_delta_requests == 5;
}

}  // namespace

int main() {
  return DeltaOnlyOwnsOneWrite() && ZeroDeltaNeverWrites() &&
                 CompleteRequestsFrozenValidation() &&
                  FaultNeverRequestsDelta() && RegressionFailsClosed() &&
                  CountdownPositiveDeltaIsWriteNeutral() &&
                  RepeatedPositiveCannotOutrunControllerCycle() &&
                  FinalInFlightWaitsForNextPositiveWithoutSixthWrite()
             ? 0
             : 1;
}
