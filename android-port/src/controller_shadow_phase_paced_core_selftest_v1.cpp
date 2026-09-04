#include "controller_shadow_phase_paced_core_v1.h"

#include <cstdint>
#include <cstring>

namespace core = a9tas::controller_shadow_phase_paced_v1;
namespace payload = a9tas::controller_shadow_coordinator_v1;
namespace coordinator = a9tas::in_process_tick_coordinator_v1;

namespace {

core::Receipts Receipts(std::uint64_t selected = 0,
                        std::uint64_t completed = 0,
                        std::uint64_t writer = 0,
                        std::uint64_t action = 0) {
  core::Receipts receipts{};
  std::memcpy(receipts.controller.magic, payload::kEvidenceMagic, 8);
  receipts.controller.version = payload::kProtocolVersion;
  receipts.controller.size = sizeof(receipts.controller);
  receipts.controller.wrapper_entries = selected;
  receipts.controller.original_calls = selected;
  receipts.controller.original_returns = completed;
  receipts.controller.source_swaps = selected;
  receipts.controller.source_restores = completed;
  receipts.controller.selected_frames = selected;
  receipts.controller.completed_frames = completed;
  receipts.controller.next_frame = static_cast<std::uint32_t>(completed);
  receipts.controller.last_status =
      selected == completed && selected != 0 ? payload::kRunning
                                             : payload::kRunning;
  receipts.controller.last_coordinator_result =
      static_cast<std::int32_t>(coordinator::Result::kFrameSelected);
  receipts.controller.coordinator_phase =
      selected == 0 ? static_cast<std::uint32_t>(coordinator::Phase::kReady)
                    : static_cast<std::uint32_t>(coordinator::Phase::kInFlight);
  receipts.writer_processed_frames = writer;
  receipts.action_completed_sequence = action;
  return receipts;
}

bool CountdownIsNeutral() {
  core::State state{};
  core::Decision decision{};
  const auto receipts = Receipts();
  return core::Observe({2, 16667}, core::Boundary::kDelta, 20000, 2, 10,
                       receipts, &state, &decision) == core::Result::kOk &&
         decision.action == core::Action::kNone &&
         state.fixed_delta_requests == 0 &&
         state.stage == core::Stage::kWaitingForGameplay;
}

bool IncompletePrefixBeforeDeltaIsIgnored() {
  core::State state{};
  core::Decision decision{};
  const auto receipts = Receipts();
  return core::Observe({2, 16667}, core::Boundary::kC9C, 0, 3, 10,
                       receipts, &state, &decision) == core::Result::kOk &&
         decision.action == core::Action::kNone &&
         state.fixed_delta_requests == 0 &&
         state.stage == core::Stage::kAwaitDelta;
}

bool ProvenBoundarySequencePacesFrames() {
  core::State state{};
  core::Decision decision{};
  auto receipts = Receipts();
  if (core::Observe({2, 16667}, core::Boundary::kDelta, 20000, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk ||
      decision.action != core::Action::kWriteFixedDelta ||
      state.fixed_delta_requests != 1 ||
      state.stage != core::Stage::kAwaitC98)
    return false;
  // Repeated accumulator accesses in the same cycle are observational.
  if (core::Observe({2, 16667}, core::Boundary::kDelta, 19000, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk ||
      decision.action != core::Action::kNone ||
      state.fixed_delta_requests != 1 || state.duplicate_delta_events != 1)
    return false;
  receipts = Receipts(1, 0, 0, 0);
  if (core::Observe({2, 16667}, core::Boundary::kC98, 0, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk ||
      state.stage != core::Stage::kAwaitC9C)
    return false;
  if (core::Observe({2, 16667}, core::Boundary::kC9C, 0, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk ||
      state.stage != core::Stage::kAwaitWorldCommit)
    return false;
  receipts = Receipts(1, 0, 1, 1);
  if (core::Observe({2, 16667}, core::Boundary::kWorldCommit, 0, 3, 20,
                    receipts, &state, &decision) != core::Result::kOk ||
      decision.action != core::Action::kNone ||
      state.committed_frames != 1 || state.stage != core::Stage::kAwaitDelta)
    return false;
  return core::Observe({2, 16667}, core::Boundary::kDelta, 18000, 3, 11,
                       receipts, &state, &decision) == core::Result::kOk &&
         decision.action == core::Action::kWriteFixedDelta &&
         state.fixed_delta_requests == 2;
}

bool FinalFrameCompletesAtWorldCommitWithoutSixthDelta() {
  core::State state{};
  core::Decision decision{};
  auto receipts = Receipts();
  if (core::Observe({1, 16667}, core::Boundary::kDelta, 20000, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk)
    return false;
  receipts = Receipts(1, 0, 0, 0);
  if (core::Observe({1, 16667}, core::Boundary::kC98, 0, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk ||
      core::Observe({1, 16667}, core::Boundary::kC9C, 0, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk)
    return false;
  receipts = Receipts(1, 0, 1, 1);
  return core::Observe({1, 16667}, core::Boundary::kWorldCommit, 0, 3, 20,
                       receipts, &state, &decision) == core::Result::kOk &&
         decision.action ==
             core::Action::kFreezeAndValidateFinalInFlight &&
         state.fixed_delta_requests == 1 && state.committed_frames == 1 &&
         state.stage == core::Stage::kComplete;
}

bool CrossThreadInputFailsClosed() {
  core::State state{};
  core::Decision decision{};
  auto receipts = Receipts();
  if (core::Observe({2, 16667}, core::Boundary::kDelta, 20000, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk)
    return false;
  receipts = Receipts(1, 0, 0, 0);
  return core::Observe({2, 16667}, core::Boundary::kC98, 0, 3, 11,
                       receipts, &state, &decision) ==
             core::Result::kBoundaryOwner &&
         decision.action == core::Action::kFreezeAndRollback;
}

bool WorldCommitRequiresAllGameOwnedReceipts() {
  core::State state{};
  core::Decision decision{};
  auto receipts = Receipts();
  if (core::Observe({2, 16667}, core::Boundary::kDelta, 20000, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk)
    return false;
  receipts = Receipts(1, 0, 0, 0);
  if (core::Observe({2, 16667}, core::Boundary::kC98, 0, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk ||
      core::Observe({2, 16667}, core::Boundary::kC9C, 0, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk)
    return false;
  receipts = Receipts(1, 0, 0, 1);
  return core::Observe({2, 16667}, core::Boundary::kWorldCommit, 0, 3, 20,
                       receipts, &state, &decision) ==
             core::Result::kReceiptLag &&
         decision.action == core::Action::kFreezeAndRollback;
}

bool OutOfOrderBoundaryFailsClosed() {
  core::State state{};
  core::Decision decision{};
  auto receipts = Receipts();
  if (core::Observe({2, 16667}, core::Boundary::kDelta, 20000, 3, 10,
                    receipts, &state, &decision) != core::Result::kOk)
    return false;
  receipts = Receipts(1, 0, 0, 0);
  return core::Observe({2, 16667}, core::Boundary::kC9C, 0, 3, 10,
                       receipts, &state, &decision) ==
             core::Result::kBoundaryOrder &&
         decision.action == core::Action::kFreezeAndRollback;
}

}  // namespace

int main() {
  return CountdownIsNeutral() && IncompletePrefixBeforeDeltaIsIgnored() &&
                 ProvenBoundarySequencePacesFrames() &&
                 FinalFrameCompletesAtWorldCommitWithoutSixthDelta() &&
                 CrossThreadInputFailsClosed() &&
                 WorldCommitRequiresAllGameOwnedReceipts() &&
                 OutOfOrderBoundaryFailsClosed()
             ? 0
             : 1;
}
