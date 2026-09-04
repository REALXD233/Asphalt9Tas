#include "barrel_post_c9c_slot_schedule_v1.h"

#include <cstdio>

namespace schedule = a9tas::barrel_post_c9c_schedule_v1;

bool Prefix(schedule::State* state, std::uint32_t* actions) {
  return schedule::Advance(state, schedule::Event::kCompletionStore, actions) &&
         schedule::Advance(state, schedule::Event::kCertifiedC9C, actions) &&
         *actions == schedule::kActionInstallParallelWatchSet;
}

bool Finish(schedule::State* state, std::uint32_t* actions) {
  return schedule::Advance(state,
                           schedule::Event::kAngularIdleReceiptAtF64,
                           actions) &&
         schedule::Advance(state, schedule::Event::kF64, actions) &&
         *actions == (schedule::kActionVerifyBarrelTerminal |
                      schedule::kActionRestorePostF64WatchSet) &&
         schedule::Advance(state, schedule::Event::kWorldCommit, actions) &&
         *actions == schedule::kActionCommitFrame && schedule::Terminal(*state);
}

int main() {
  std::uint32_t actions = 0;

  schedule::State neither{};
  const bool neither_path = Prefix(&neither, &actions) &&
                            Finish(&neither, &actions) &&
                            neither.rbx_pairs == 0 &&
                            neither.angular_transactions == 0;

  schedule::State rbx_only{};
  const bool rbx_path =
      Prefix(&rbx_only, &actions) &&
      schedule::Advance(&rbx_only, schedule::Event::kRbxFirstStore,
                        &actions) &&
      actions == schedule::kActionSwapRbxFirstForSecond &&
      schedule::Advance(&rbx_only, schedule::Event::kRbxSecondStore,
                        &actions) &&
      actions == (schedule::kActionApplyRbxTail |
                  schedule::kActionResetRbxFirstWatch) &&
      Finish(&rbx_only, &actions) && rbx_only.rbx_pairs == 1 &&
      rbx_only.angular_transactions == 0;

  schedule::State yaw_only{};
  const bool yaw_path =
      Prefix(&yaw_only, &actions) &&
      schedule::Advance(&yaw_only,
                        schedule::Event::kAngularSetterCertificate,
                        &actions) &&
      actions == schedule::kActionArmAngularTransientWindow &&
      schedule::Advance(&yaw_only, schedule::Event::kAngularPayloadComplete,
                        &actions) &&
      Finish(&yaw_only, &actions) && yaw_only.rbx_pairs == 0 &&
      yaw_only.angular_transactions == 1;

  schedule::State both{};
  const bool both_path =
      Prefix(&both, &actions) &&
      schedule::Advance(&both, schedule::Event::kRbxFirstStore, &actions) &&
      schedule::Advance(&both, schedule::Event::kRbxSecondStore, &actions) &&
      schedule::Advance(&both,
                        schedule::Event::kAngularSetterCertificate,
                        &actions) &&
      schedule::Advance(&both, schedule::Event::kAngularPayloadComplete,
                        &actions) &&
      Finish(&both, &actions) && both.rbx_pairs == 1 &&
      both.angular_transactions == 1;

  schedule::State broad_candidate_absent{};
  const bool absent_cancel_path =
      Prefix(&broad_candidate_absent, &actions) &&
      schedule::Advance(&broad_candidate_absent,
                        schedule::Event::kAngularSetterCertificate,
                        &actions) &&
      schedule::Advance(&broad_candidate_absent,
                        schedule::Event::kAngularAbsentCancelled,
                        &actions) &&
      Finish(&broad_candidate_absent, &actions) &&
      broad_candidate_absent.angular_transactions == 0;

  schedule::State repeated{};
  bool repeated_path = Prefix(&repeated, &actions);
  for (std::uint32_t index = 0;
       repeated_path && index < schedule::kMaximumTransactionsPerFrame;
       ++index) {
    repeated_path =
        schedule::Advance(&repeated, schedule::Event::kRbxFirstStore,
                          &actions) &&
        schedule::Advance(&repeated, schedule::Event::kRbxSecondStore,
                          &actions) &&
        schedule::Advance(&repeated,
                          schedule::Event::kAngularSetterCertificate,
                          &actions) &&
        schedule::Advance(&repeated,
                          schedule::Event::kAngularPayloadComplete,
                          &actions);
  }
  repeated_path = repeated_path && Finish(&repeated, &actions) &&
                  repeated.rbx_pairs ==
                      schedule::kMaximumTransactionsPerFrame &&
                  repeated.angular_transactions ==
                      schedule::kMaximumTransactionsPerFrame;

  schedule::State half_rbx{};
  const bool rejects_half_rbx =
      Prefix(&half_rbx, &actions) &&
      schedule::Advance(&half_rbx, schedule::Event::kRbxFirstStore,
                        &actions) &&
      !schedule::Advance(&half_rbx, schedule::Event::kF64, &actions) &&
      half_rbx.stage == schedule::Stage::kFaulted;

  schedule::State armed_at_f64{};
  const bool rejects_armed =
      Prefix(&armed_at_f64, &actions) &&
      schedule::Advance(&armed_at_f64,
                        schedule::Event::kAngularSetterCertificate,
                        &actions) &&
      !schedule::Advance(&armed_at_f64, schedule::Event::kF64, &actions) &&
      armed_at_f64.stage == schedule::Stage::kFaulted;

  schedule::State missing_completion{};
  const bool rejects_unproved_c9c =
      !schedule::Advance(&missing_completion,
                         schedule::Event::kCertifiedC9C, &actions);

  schedule::State missing_idle_receipt{};
  const bool rejects_unreceipted_f64 =
      Prefix(&missing_idle_receipt, &actions) &&
      !schedule::Advance(&missing_idle_receipt, schedule::Event::kF64,
                         &actions) &&
      missing_idle_receipt.stage == schedule::Stage::kFaulted;

  schedule::InterlockedState interlocked{};
  const bool interlocked_main_path =
      schedule::AdvanceInterlocked(
          &interlocked,
          schedule::InterlockedEvent::kCompletionStore, &actions) &&
      schedule::AdvanceInterlocked(
          &interlocked,
          schedule::InterlockedEvent::kCertifiedC9C, &actions) &&
      schedule::AdvanceInterlocked(
          &interlocked,
          schedule::InterlockedEvent::kAngularTerminalReceipt, &actions) &&
      schedule::AdvanceInterlocked(
          &interlocked, schedule::InterlockedEvent::kF64, &actions) &&
      schedule::AdvanceInterlocked(
          &interlocked, schedule::InterlockedEvent::kCallbackClose,
          &actions) &&
      schedule::AdvanceInterlocked(
          &interlocked,
          schedule::InterlockedEvent::kDeferredCallbackClear, &actions) &&
      schedule::AdvanceInterlocked(
          &interlocked, schedule::InterlockedEvent::kWorldCommit,
          &actions) &&
      schedule::InterlockedTerminal(interlocked);

  schedule::InterlockedState early_world{};
  const bool rejects_world_before_callback_close =
      schedule::AdvanceInterlocked(
          &early_world,
          schedule::InterlockedEvent::kCompletionStore, &actions) &&
      schedule::AdvanceInterlocked(
          &early_world,
          schedule::InterlockedEvent::kCertifiedC9C, &actions) &&
      schedule::AdvanceInterlocked(
          &early_world,
          schedule::InterlockedEvent::kAngularTerminalReceipt, &actions) &&
      schedule::AdvanceInterlocked(
          &early_world, schedule::InterlockedEvent::kF64, &actions) &&
      !schedule::AdvanceInterlocked(
          &early_world, schedule::InterlockedEvent::kWorldCommit,
          &actions) &&
      early_world.main == schedule::MainStage::kFaulted;

  schedule::InterlockedState missing_deferred{};
  const bool rejects_world_before_deferred_clear =
      schedule::AdvanceInterlocked(
          &missing_deferred,
          schedule::InterlockedEvent::kCompletionStore, &actions) &&
      schedule::AdvanceInterlocked(
          &missing_deferred,
          schedule::InterlockedEvent::kCertifiedC9C, &actions) &&
      schedule::AdvanceInterlocked(
          &missing_deferred,
          schedule::InterlockedEvent::kAngularTerminalReceipt, &actions) &&
      schedule::AdvanceInterlocked(
          &missing_deferred, schedule::InterlockedEvent::kF64,
          &actions) &&
      schedule::AdvanceInterlocked(
          &missing_deferred,
          schedule::InterlockedEvent::kCallbackClose, &actions) &&
      !schedule::AdvanceInterlocked(
          &missing_deferred,
          schedule::InterlockedEvent::kWorldCommit, &actions) &&
      missing_deferred.main == schedule::MainStage::kFaulted;

  const bool passed = neither_path && rbx_path && yaw_path &&
                       both_path && absent_cancel_path && repeated_path &&
                       rejects_half_rbx && rejects_armed &&
                       rejects_unproved_c9c && rejects_unreceipted_f64 &&
                       interlocked_main_path &&
                       rejects_world_before_callback_close &&
                       rejects_world_before_deferred_clear;
  std::printf(
      "BARREL_POST_C9C_SLOT_SCHEDULE_SELFTEST passed=%u "
      "rbx_yaw_orthogonal=1 exact_paths=1 broad_absent_cancel=1 repeated_calls=1 "
      "callback_restored_at_f64=1 half_rbx_rejected=1 "
      "armed_yaw_rejected=1 unreceipted_f64_rejected=1 "
      "main_machine_interlock=1 callback_close_deferred_world_order=1 "
      "runtime=disabled\n",
      passed ? 1u : 0u);
  return passed ? 0 : 1;
}
