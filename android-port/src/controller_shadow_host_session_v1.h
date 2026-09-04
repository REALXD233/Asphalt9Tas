#pragma once

// Host-side orchestration for one controller-shadow lifetime.  The caller
// supplies a frozen-thread guard and verified process-memory backend.  This
// layer binds the hash-pinned ELF layout to the transaction core and makes
// cleanup mandatory after every Stage/Commit failure.

#include "controller_shadow_coordinator_elf_resolver_v1.h"
#include "controller_shadow_transaction_core_v1.h"
#include "in_process_tick_coordinator_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace a9tas::controller_shadow_host_session_v1 {

namespace elf = a9tas::controller_shadow_coordinator_elf_v1;
namespace payload = a9tas::controller_shadow_coordinator_v1;
namespace transaction = a9tas::controller_shadow_transaction_core_v1;
namespace coordinator = a9tas::in_process_tick_coordinator_v1;
namespace recording = a9tas::unified_tick_v1;
namespace writer = a9tas::final_writer_replay_v1;
namespace mailbox = a9tas::natural_action_callback_v1;

enum class Phase : std::uint32_t {
  kEmpty = 0,
  kPrepared = 1,
  kStaged = 2,
  kInstalled = 3,
  kClean = 4,
  kFaulted = 5,
};

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kPayloadBindingRejected = -2,
  kPrepareRejected = -3,
  kStageRejectedClean = -4,
  kCommitRejectedClean = -5,
  kCleanupFailed = -6,
  kNotInstalled = -7,
  kCompletionReadFailed = -8,
  kCompletionRejected = -9,
};

struct Session {
  Phase phase{Phase::kEmpty};
  transaction::PayloadLayout payload_layout{};
  transaction::RuntimeLayout runtime_layout{};
  transaction::Prepared prepared{};
  transaction::Result prepare_result{transaction::Result::kInvalidArgument};
  transaction::Result stage_result{transaction::Result::kInvalidArgument};
  transaction::Result commit_result{transaction::Result::kInvalidArgument};
  transaction::Result rollback_result{transaction::Result::kInvalidArgument};
};

struct Completion {
  payload::Evidence controller{};
  writer::Evidence final_writer{};
  mailbox::Mailbox action_mailbox{};
  std::uintptr_t controller_vptr{};
  std::uintptr_t source{};
  std::uintptr_t source_vptr{};
};

inline bool ElfLayoutValid(const elf::Layout& layout) noexcept {
  return std::memcmp(layout.file_sha256, elf::kExpectedSha256,
                     sizeof(elf::kExpectedSha256)) == 0 &&
         layout.load_bias != 0 && layout.wrapper != 0 && layout.shadow != 0 &&
         layout.control != 0 && layout.evidence != 0 && layout.frames != 0 &&
         layout.shadow_size == payload::kControllerShadowSize &&
         layout.control_size == sizeof(payload::Control) &&
         layout.evidence_size == sizeof(payload::Evidence) &&
         layout.frame_size == sizeof(recording::RecordingFrameV1) &&
         layout.frame_capacity == payload::kMaximumFrames &&
         layout.prefix_size == payload::kControllerPrefixSize &&
         layout.update_slot == payload::kControllerUpdateSlotOffset;
}

inline transaction::PayloadLayout BindPayload(
    const elf::Layout& layout) noexcept {
  return {
      layout.wrapper,       layout.shadow,       layout.control,
      layout.evidence,      layout.frames,       layout.shadow_size,
      layout.control_size,  layout.evidence_size, layout.frame_size,
      layout.frame_capacity,
  };
}

inline transaction::Result Cleanup(const transaction::Backend& backend,
                                   const transaction::Guard& guard,
                                   Session* session) noexcept {
  if (session == nullptr) return transaction::Result::kInvalidArgument;
  session->rollback_result = transaction::Rollback(
      backend, guard, session->payload_layout, session->runtime_layout,
      session->prepared);
  session->phase = session->rollback_result == transaction::Result::kOk
                       ? Phase::kClean
                       : Phase::kFaulted;
  return session->rollback_result;
}

inline Result Install(const transaction::Backend& backend,
                      const transaction::Guard& guard,
                      const elf::Layout& elf_layout,
                      const transaction::RuntimeLayout& runtime_layout,
                      const recording::RecordingFrameV1* frames,
                      std::uint32_t frame_count,
                      const std::uint8_t recording_sha256[32],
                      Session* output) noexcept {
  if (output == nullptr || output->phase != Phase::kEmpty)
    return Result::kInvalidArgument;
  Session session{};
  session.runtime_layout = runtime_layout;
  if (!ElfLayoutValid(elf_layout)) {
    *output = session;
    return Result::kPayloadBindingRejected;
  }
  session.payload_layout = BindPayload(elf_layout);
  session.prepare_result = transaction::Prepare(
      backend, guard, session.payload_layout, session.runtime_layout, frames,
      frame_count, recording_sha256, &session.prepared);
  if (session.prepare_result != transaction::Result::kOk) {
    *output = session;
    return Result::kPrepareRejected;
  }
  session.phase = Phase::kPrepared;
  session.stage_result = transaction::Stage(
      backend, guard, session.payload_layout, session.prepared,
      session.runtime_layout, frames);
  if (session.stage_result != transaction::Result::kOk) {
    const transaction::Result cleanup = Cleanup(backend, guard, &session);
    *output = session;
    return cleanup == transaction::Result::kOk ? Result::kStageRejectedClean
                                                : Result::kCleanupFailed;
  }
  session.phase = Phase::kStaged;
  session.commit_result = transaction::Commit(
      backend, guard, session.payload_layout, session.runtime_layout,
      session.prepared, frames);
  if (session.commit_result != transaction::Result::kOk) {
    const transaction::Result cleanup = Cleanup(backend, guard, &session);
    *output = session;
    return cleanup == transaction::Result::kOk ? Result::kCommitRejectedClean
                                                : Result::kCleanupFailed;
  }
  session.phase = Phase::kInstalled;
  *output = session;
  return Result::kOk;
}

inline Result Uninstall(const transaction::Backend& backend,
                        const transaction::Guard& guard,
                        Session* session) noexcept {
  if (session == nullptr || session->phase != Phase::kInstalled)
    return Result::kNotInstalled;
  return Cleanup(backend, guard, session) == transaction::Result::kOk
             ? Result::kOk
             : Result::kCleanupFailed;
}

inline Result ValidateComplete(const transaction::Backend& backend,
                               const recording::RecordingFrameV1* frames,
                               const Session& session,
                               Completion* output) noexcept {
  if (output == nullptr || frames == nullptr ||
      session.phase != Phase::kInstalled ||
      session.prepared.frame_count == 0)
    return Result::kInvalidArgument;
  Completion completion{};
  payload::Control live_control{};
  writer::Control writer_control{};
  std::uintptr_t source_slot = 0;
  if (!transaction::Add(session.runtime_layout.controller,
                        payload::kControllerSourceOffset, &source_slot) ||
      !backend.read(backend.context, session.payload_layout.control,
                    &live_control, sizeof(live_control)) ||
      !backend.read(backend.context, session.payload_layout.evidence,
                    &completion.controller, sizeof(completion.controller)) ||
      !backend.read(backend.context, session.runtime_layout.writer_control,
                    &writer_control, sizeof(writer_control)) ||
      !backend.read(backend.context, session.runtime_layout.writer_evidence,
                    &completion.final_writer,
                    sizeof(completion.final_writer)) ||
      !backend.read(backend.context, session.runtime_layout.action_mailbox,
                    &completion.action_mailbox,
                    sizeof(completion.action_mailbox)) ||
      !backend.read(backend.context, session.runtime_layout.controller,
                    &completion.controller_vptr,
                    sizeof(completion.controller_vptr)) ||
      !backend.read(backend.context, source_slot, &completion.source,
                    sizeof(completion.source)) ||
      !backend.read(backend.context, completion.source,
                    &completion.source_vptr,
                    sizeof(completion.source_vptr)))
    return Result::kCompletionReadFailed;

  const std::uint32_t count = session.prepared.frame_count;
  const payload::Evidence& evidence = completion.controller;
  const writer::Evidence& writer_evidence = completion.final_writer;
  const std::uint32_t expected_calls =
      coordinator::ExpectedActionCalls(frames[count - 1u]);
  const bool controller_ok =
      std::memcmp(&live_control, &session.prepared.published_control,
                  sizeof(live_control)) == 0 &&
      completion.controller_vptr == session.prepared.original_controller_vptr &&
      completion.source == session.prepared.original_source &&
      completion.source_vptr == session.prepared.original_source_vptr &&
      std::memcmp(evidence.magic, payload::kEvidenceMagic,
                  sizeof(payload::kEvidenceMagic)) == 0 &&
      evidence.version == payload::kProtocolVersion &&
      evidence.size == sizeof(payload::Evidence) &&
      evidence.wrapper_entries == static_cast<std::uint64_t>(count) + 1u &&
      evidence.original_calls == evidence.wrapper_entries &&
      evidence.original_returns == evidence.wrapper_entries &&
      evidence.source_swaps == count && evidence.source_restores == count &&
      evidence.selected_frames == count && evidence.completed_frames == count &&
      evidence.failures == 0 && evidence.recursive_entries == 0 &&
      evidence.last_controller == session.runtime_layout.controller &&
      evidence.observed_source_vptr == session.prepared.original_source_vptr &&
      evidence.last_selected_frame == count - 1u &&
      evidence.last_tid == session.runtime_layout.producer_tid &&
      evidence.last_status == payload::kComplete &&
      evidence.last_coordinator_result ==
          static_cast<std::int32_t>(coordinator::Result::kComplete) &&
      evidence.coordinator_phase ==
          static_cast<std::uint32_t>(coordinator::Phase::kComplete) &&
      evidence.next_frame == count;
  const bool writer_ok =
      std::memcmp(writer_control.magic, writer::kControlMagic,
                  sizeof(writer::kControlMagic)) == 0 &&
      writer_control.version == writer::kProtocolVersion &&
      writer_control.size == sizeof(writer::Control) &&
      writer_control.frame_count == count &&
      writer_control.expected_object == session.runtime_layout.vehicle_owner &&
      writer_control.reserved[0] == writer::kFramePermitDisarmed &&
      std::memcmp(writer_evidence.magic, writer::kEvidenceMagic,
                  sizeof(writer::kEvidenceMagic)) == 0 &&
      writer_evidence.version == writer::kProtocolVersion &&
      writer_evidence.size == sizeof(writer::Evidence) &&
      writer_evidence.processed_frames == count &&
      writer_evidence.failures == 0 && writer_evidence.recursive_entries == 0;
  const bool action_ok = mailbox::CompletionMatches(
      completion.action_mailbox, count, count - 1u, expected_calls);
  // Preserve the complete read-only snapshot even when one semantic receipt is
  // rejected.  The caller must still treat the Result as authoritative; the
  // snapshot exists only so a failed live gate can identify the lagging
  // controller, writer, or action consumer without another mutation attempt.
  *output = completion;
  if (!controller_ok || !writer_ok || !action_ok)
    return Result::kCompletionRejected;
  return Result::kOk;
}

// Validate the semantically complete final frame at the following positive
// accumulator boundary, before a GameplayInputController::UpdatePerTick for a
// new logical frame exists.
// The controller coordinator is intentionally still kInFlight for frame N-1;
// writer/action receipts prove that both downstream consumers completed it.
// Cleanup may then restore the controller vptr without executing an extra
// gameplay tick solely to advance bookkeeping.
inline Result ValidateFinalInFlight(
    const transaction::Backend& backend,
    const recording::RecordingFrameV1* frames, const Session& session,
    Completion* output) noexcept {
  if (output == nullptr || frames == nullptr ||
      session.phase != Phase::kInstalled ||
      session.prepared.frame_count == 0)
    return Result::kInvalidArgument;
  Completion completion{};
  payload::Control live_control{};
  writer::Control writer_control{};
  std::uintptr_t source_slot = 0;
  if (!transaction::Add(session.runtime_layout.controller,
                        payload::kControllerSourceOffset, &source_slot) ||
      !backend.read(backend.context, session.payload_layout.control,
                    &live_control, sizeof(live_control)) ||
      !backend.read(backend.context, session.payload_layout.evidence,
                    &completion.controller, sizeof(completion.controller)) ||
      !backend.read(backend.context, session.runtime_layout.writer_control,
                    &writer_control, sizeof(writer_control)) ||
      !backend.read(backend.context, session.runtime_layout.writer_evidence,
                    &completion.final_writer,
                    sizeof(completion.final_writer)) ||
      !backend.read(backend.context, session.runtime_layout.action_mailbox,
                    &completion.action_mailbox,
                    sizeof(completion.action_mailbox)) ||
      !backend.read(backend.context, session.runtime_layout.controller,
                    &completion.controller_vptr,
                    sizeof(completion.controller_vptr)) ||
      !backend.read(backend.context, source_slot, &completion.source,
                    sizeof(completion.source)) ||
      !backend.read(backend.context, completion.source,
                    &completion.source_vptr,
                    sizeof(completion.source_vptr)))
    return Result::kCompletionReadFailed;

  const std::uint32_t count = session.prepared.frame_count;
  const payload::Evidence& evidence = completion.controller;
  const writer::Evidence& writer_evidence = completion.final_writer;
  const std::uint32_t expected_calls =
      coordinator::ExpectedActionCalls(frames[count - 1u]);
  const bool controller_ok =
      std::memcmp(&live_control, &session.prepared.published_control,
                  sizeof(live_control)) == 0 &&
      completion.controller_vptr == session.prepared.shadow_controller_vptr &&
      completion.source == session.prepared.original_source &&
      completion.source_vptr == session.prepared.original_source_vptr &&
      std::memcmp(evidence.magic, payload::kEvidenceMagic,
                  sizeof(payload::kEvidenceMagic)) == 0 &&
      evidence.version == payload::kProtocolVersion &&
      evidence.size == sizeof(payload::Evidence) &&
      evidence.wrapper_entries == count &&
      evidence.original_calls == evidence.wrapper_entries &&
      evidence.original_returns == evidence.wrapper_entries &&
      evidence.source_swaps == count && evidence.source_restores == count &&
      evidence.selected_frames == count &&
      evidence.completed_frames + 1u == count && evidence.failures == 0 &&
      evidence.recursive_entries == 0 &&
      evidence.last_controller == session.runtime_layout.controller &&
      evidence.observed_source_vptr == session.prepared.original_source_vptr &&
      evidence.last_selected_frame == count - 1u &&
      evidence.last_tid == session.runtime_layout.producer_tid &&
      evidence.last_status == payload::kRunning &&
      evidence.last_coordinator_result ==
          static_cast<std::int32_t>(coordinator::Result::kFrameSelected) &&
      evidence.coordinator_phase ==
          static_cast<std::uint32_t>(coordinator::Phase::kInFlight) &&
      evidence.next_frame + 1u == count;
  const bool writer_ok =
      std::memcmp(writer_control.magic, writer::kControlMagic,
                  sizeof(writer::kControlMagic)) == 0 &&
      writer_control.version == writer::kProtocolVersion &&
      writer_control.size == sizeof(writer::Control) &&
      writer_control.frame_count == count &&
      writer_control.expected_object == session.runtime_layout.vehicle_owner &&
      writer_control.reserved[0] == writer::kFramePermitDisarmed &&
      std::memcmp(writer_evidence.magic, writer::kEvidenceMagic,
                  sizeof(writer::kEvidenceMagic)) == 0 &&
      writer_evidence.version == writer::kProtocolVersion &&
      writer_evidence.size == sizeof(writer::Evidence) &&
      writer_evidence.processed_frames == count &&
      writer_evidence.failures == 0 && writer_evidence.recursive_entries == 0;
  const bool action_ok = mailbox::CompletionMatches(
      completion.action_mailbox, count, count - 1u, expected_calls);
  // As above, expose the observed receipt counters on semantic rejection while
  // keeping the non-kOk return fail-closed.
  *output = completion;
  if (!controller_ok || !writer_ok || !action_ok)
    return Result::kCompletionRejected;
  return Result::kOk;
}

}  // namespace a9tas::controller_shadow_host_session_v1
