#pragma once

// Runtime-primitive-independent transport for the transient BarrelYaw payload.
// Install stages immutable payload state but leaves the object original.  On a
// stopped authoritative owner, ArmFrame proves every source/storage identity,
// publishes unique indices and token, then publishes the shadow vptr last.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "barrel_yaw_tail_host_transaction_v1.h"
#include "barrel_yaw_tail_payload_elf_resolver_v1.h"
#include "unified_tick_recording_v1.h"

namespace a9tas::barrel_yaw_tail_transport_v1 {

namespace elf = a9tas::barrel_yaw_tail_elf_v1;
namespace host = a9tas::barrel_yaw_tail_host_v1;
namespace protocol = a9tas::barrel_yaw_tail_payload_v1;

using ReadFn = bool (*)(void*, std::uintptr_t, void*, std::size_t);
using WriteFn = bool (*)(void*, std::uintptr_t, const void*, std::size_t);

struct Io {
  void* context{};
  ReadFn read{};
  WriteFn write{};
};

struct Session {
  elf::Layout payload{};
  host::Prepared prepared{};
  host::Transaction transaction{};
  std::uintptr_t object{};
  std::uintptr_t original_vptr{};
  std::uint32_t generation{};
  std::uint32_t frame_count{};
  std::uint32_t completed_transactions{};
  std::uint32_t issued_arm_sequences{};
  std::uint32_t last_control_audit_index{};
  std::uint32_t last_control_frame_index{};
  std::uint32_t armed_audit_index{};
  std::uint32_t armed_frame_index{};
  std::uint32_t armed_sequence{};
  std::uint64_t armed_token{};
  protocol::FrameTarget armed_target{};
  protocol::TransactionAudit audit_before_arm{};
  protocol::Evidence evidence_before_arm{};
  protocol::Evidence last_evidence{};
  protocol::Evidence frame_start_evidence{};
  std::array<std::uint8_t, protocol::kMaximumFrames> frame_arm_counts{};
  std::uint32_t active_window_frame_index{};
  std::uintptr_t restore_observed_before{};
  std::uintptr_t restore_observed_after{};
  bool installed{};
  bool frame_armed{};
  bool frame_window_active{};
  bool frame_window_ever_armed{};
  bool faulted{};
};

inline bool Read(const Io& io, std::uintptr_t address, void* output,
                 std::size_t size) {
  return io.read != nullptr && output != nullptr && size != 0 &&
         io.read(io.context, address, output, size);
}

inline bool Write(const Io& io, std::uintptr_t address, const void* input,
                  std::size_t size) {
  return io.write != nullptr && input != nullptr && size != 0 &&
         io.write(io.context, address, input, size);
}

inline bool IndexedAddress(std::uintptr_t base, std::size_t item_size,
                           std::uint32_t index, std::uintptr_t* output) {
  if (output == nullptr || item_size == 0 ||
      index > UINTPTR_MAX / item_size)
    return false;
  const std::uintptr_t offset =
      static_cast<std::uintptr_t>(item_size) * index;
  if (base > UINTPTR_MAX - offset) return false;
  *output = base + offset;
  return true;
}

inline bool StaticControlMatches(const Session& session,
                                 const protocol::Control& control,
                                 bool require_published) {
  const protocol::Control& expected = require_published
      ? session.prepared.published_control
      : session.prepared.unpublished_control;
  return std::memcmp(control.magic, expected.magic, sizeof(control.magic)) == 0 &&
         control.version == expected.version && control.size == expected.size &&
         control.flags == expected.flags &&
         control.frame_count == expected.frame_count &&
         control.expected_object == expected.expected_object &&
         control.original_vptr == expected.original_vptr &&
         control.shadow_vptr == expected.shadow_vptr &&
         control.original_boundary_callback ==
             expected.original_boundary_callback &&
         control.native_angular == expected.native_angular &&
         control.expected_tid == expected.expected_tid &&
         control.session_generation == expected.session_generation &&
         std::memcmp(control.recording_sha256, expected.recording_sha256,
                     sizeof(control.recording_sha256)) == 0 &&
         control.expected_first_caller_return ==
             expected.expected_first_caller_return &&
         control.expected_second_caller_return ==
             expected.expected_second_caller_return;
}

inline protocol::Control ExpectedControl(const Session& session,
                                         std::uint32_t audit_index,
                                         std::uint32_t frame_index,
                                         std::uint64_t token) {
  protocol::Control expected = session.prepared.published_control;
  expected.active_audit_index = audit_index;
  expected.active_frame_index = frame_index;
  expected.active_token = token;
  return expected;
}

inline bool ControlMatchesExact(const Session& session,
                                const protocol::Control& control,
                                std::uint32_t audit_index,
                                std::uint32_t frame_index,
                                std::uint64_t token) {
  const protocol::Control expected =
      ExpectedControl(session, audit_index, frame_index, token);
  return StaticControlMatches(session, control, true) &&
         std::memcmp(&control, &expected, sizeof(control)) == 0;
}

inline bool ReadKnownObjectVptr(const Io& io, Session* session,
                                std::uintptr_t* observed) {
  if (session == nullptr || observed == nullptr ||
      !Read(io, session->object, observed, sizeof(*observed)))
    return false;
  return *observed == session->original_vptr ||
         *observed == session->prepared.shadow_vptr;
}

inline bool ReadOriginalTableExact(const Io& io, const Session& session) {
  if (session.original_vptr < protocol::kVptrPrefixSize) return false;
  std::array<std::uint8_t, protocol::kShadowSize> live{};
  return Read(io, session.original_vptr - protocol::kVptrPrefixSize,
              live.data(), live.size()) &&
         std::memcmp(live.data(), session.prepared.original.data(),
                     live.size()) == 0;
}

inline bool ReadShadowExact(const Io& io, const Session& session) {
  std::array<std::uint8_t, protocol::kShadowSize> live{};
  return Read(io, session.payload.shadow, live.data(), live.size()) &&
         std::memcmp(live.data(), session.prepared.shadow.data(),
                     live.size()) == 0;
}

inline bool ReadNativeBindingExact(const Io& io, const Session& session) {
  if (session.object > UINTPTR_MAX - protocol::kNativeBodyPointerOffset)
    return false;
  std::uintptr_t native_body = 0;
  if (!Read(io, session.object + protocol::kNativeBodyPointerOffset,
            &native_body, sizeof(native_body)) || native_body == 0 ||
      native_body > UINTPTR_MAX - protocol::kNativeAngularOffset)
    return false;
  return native_body + protocol::kNativeAngularOffset ==
         session.prepared.published_control.native_angular;
}

inline bool StoppedOwnerMatches(const Session& session,
                                std::uint32_t stopped_tid) {
  return stopped_tid != 0 &&
         stopped_tid == session.prepared.published_control.expected_tid;
}

inline bool AuditEmpty(const protocol::TransactionAudit& audit) {
  const protocol::TransactionAudit empty{};
  return std::memcmp(&audit, &empty, sizeof(audit)) == 0;
}

inline bool FailClosed(Session* session,
                       host::Result cause = host::Result::kIncompleteEvidence) {
  if (session != nullptr) {
    session->faulted = true;
    if (session->transaction.phase() != host::Phase::kRollbackRequired &&
        session->transaction.phase() != host::Phase::kFaulted &&
        session->transaction.phase() != host::Phase::kRolledBack)
      session->transaction.RequireRollback(cause);
  }
  return false;
}

inline void ClearArmedIntent(Session* session) {
  session->frame_armed = false;
  session->armed_audit_index = 0;
  session->armed_frame_index = 0;
  session->armed_sequence = 0;
  session->armed_token = protocol::kDisarmedToken;
  session->armed_target = {};
  session->audit_before_arm = {};
  session->evidence_before_arm = {};
}

inline bool RestoreVptr(const Io& io, Session* session) {
  if (session == nullptr || !session->installed || !session->frame_armed)
    return false;
  std::uintptr_t observed_vptr = 0;
  if (!ReadKnownObjectVptr(io, session, &observed_vptr) ||
      observed_vptr != session->prepared.shadow_vptr)
    return false;
  session->restore_observed_before = observed_vptr;
  if (!Write(io, session->object, &session->original_vptr,
             sizeof(session->original_vptr)))
    return false;
  if (!Read(io, session->object, &observed_vptr, sizeof(observed_vptr)) ||
      observed_vptr != session->original_vptr)
    return false;
  session->restore_observed_after = observed_vptr;
  return true;
}

inline bool Install(const Io& io, const elf::Layout& payload,
                    std::uintptr_t object, std::uintptr_t native_angular,
                    std::uint32_t expected_tid, std::uint32_t generation,
                    const std::uint8_t recording_sha256[32],
                    const protocol::FrameTarget* targets,
                    std::uint32_t frame_count, Session* output) {
  if (output == nullptr || recording_sha256 == nullptr || targets == nullptr ||
      object == 0 || native_angular == 0 || expected_tid == 0 ||
      generation == 0 || frame_count < 2 ||
      frame_count > protocol::kMaximumFrames)
    return false;
  Session result{};
  result.payload = payload;
  result.object = object;
  result.generation = generation;
  result.frame_count = frame_count;

  protocol::Control initial_control{};
  protocol::Evidence initial_evidence{};
  if (!Read(io, payload.control, &initial_control, sizeof(initial_control)) ||
      !Read(io, payload.evidence, &initial_evidence,
            sizeof(initial_evidence)) ||
      !Read(io, object, &result.original_vptr, sizeof(result.original_vptr)) ||
      result.original_vptr < protocol::kVptrPrefixSize)
    return false;
  std::array<std::uint8_t, protocol::kShadowSize> original_table{};
  if (!Read(io, result.original_vptr - protocol::kVptrPrefixSize,
            original_table.data(), original_table.size()) ||
      host::Prepare(original_table.data(), object, result.original_vptr,
                    payload.shadow, payload.boundary, native_angular,
                    expected_tid, generation, recording_sha256, frame_count,
                    initial_control, initial_evidence,
                    &result.prepared) != host::Result::kOk)
    return false;

  // Install only configures payload-owned storage.  The object must remain on
  // its original table until a stopped-owner ArmFrame publishes the shadow.
  if (!Write(io, payload.targets, targets,
             sizeof(protocol::FrameTarget) * frame_count) ||
      !Write(io, payload.shadow, result.prepared.shadow.data(),
             result.prepared.shadow.size()) ||
      !Write(io, payload.control, &result.prepared.unpublished_control,
             sizeof(result.prepared.unpublished_control)) ||
      result.transaction.MarkPayloadStaged() != host::Result::kOk)
    return false;
  const std::uint32_t published_flags =
      result.prepared.published_control.flags;
  if (!Write(io, payload.control + offsetof(protocol::Control, flags),
             &published_flags, sizeof(published_flags)))
    return false;

  result.installed = true;
  result.last_evidence = result.prepared.fresh_evidence;
  protocol::Control published{};
  protocol::Evidence evidence{};
  std::uintptr_t observed_vptr = 0;
  protocol::TransactionAudit first_audit{};
  if (!Read(io, payload.control, &published, sizeof(published)) ||
      !Read(io, payload.evidence, &evidence, sizeof(evidence)) ||
      !Read(io, object, &observed_vptr, sizeof(observed_vptr)) ||
      !Read(io, payload.audits, &first_audit, sizeof(first_audit)) ||
      observed_vptr != result.original_vptr ||
      !ControlMatchesExact(result, published, 0, 0,
                           protocol::kDisarmedToken) ||
      !host::EvidenceUnchanged(result.prepared.fresh_evidence, evidence) ||
      !AuditEmpty(first_audit) || !ReadOriginalTableExact(io, result) ||
      !ReadShadowExact(io, result))
    return false;
  for (std::uint32_t index = 0; index < frame_count; ++index) {
    std::uintptr_t target_address = 0;
    protocol::FrameTarget observed{};
    if (!IndexedAddress(payload.targets, sizeof(protocol::FrameTarget), index,
                        &target_address) ||
        !Read(io, target_address, &observed, sizeof(observed)) ||
        std::memcmp(&observed, &targets[index], sizeof(observed)) != 0)
      return false;
  }
  if (result.transaction.MarkConfigurationPublished(published) !=
      host::Result::kOk)
    return false;
  *output = result;
  return true;
}

inline bool BeginFrame(const Io& io, std::uint32_t frame_index,
                       std::uint32_t stopped_tid, Session* session) {
  if (session == nullptr || !StoppedOwnerMatches(*session, stopped_tid) ||
      !session->installed ||
      session->frame_armed || session->frame_window_active ||
      session->faulted || frame_index >= session->frame_count)
    return false;
  protocol::Control control{};
  protocol::Evidence evidence{};
  std::uintptr_t observed_vptr = 0;
  if (!Read(io, session->payload.control, &control, sizeof(control)) ||
      !Read(io, session->payload.evidence, &evidence, sizeof(evidence)) ||
      !Read(io, session->object, &observed_vptr, sizeof(observed_vptr)) ||
      observed_vptr != session->original_vptr ||
      !ControlMatchesExact(*session, control,
                           session->last_control_audit_index,
                           session->last_control_frame_index,
                           protocol::kDisarmedToken) ||
      !host::EvidenceUnchanged(session->last_evidence, evidence) ||
      !ReadOriginalTableExact(io, *session) ||
      !ReadShadowExact(io, *session) ||
      !ReadNativeBindingExact(io, *session))
    return FailClosed(session, host::Result::kUnexpectedBuildIdentity);
  session->active_window_frame_index = frame_index;
  session->frame_start_evidence = evidence;
  session->frame_window_ever_armed = false;
  session->frame_window_active = true;
  return true;
}

inline bool ArmFrame(const Io& io, std::uint32_t frame_index,
                     const protocol::FrameTarget& expected_target,
                     std::uint32_t stopped_tid, Session* session) {
  if (session == nullptr || !StoppedOwnerMatches(*session, stopped_tid) ||
      !session->installed ||
      !session->frame_window_active || session->frame_armed ||
      session->faulted || frame_index != session->active_window_frame_index ||
      frame_index >= session->frame_count ||
      session->completed_transactions >= protocol::kMaximumTransactions ||
      session->issued_arm_sequences >= protocol::kMaximumTransactions ||
      session->frame_arm_counts[frame_index] >=
          protocol::kMaximumTransactionsPerFrame)
    return false;

  const std::uint32_t audit_index = session->completed_transactions;
  std::uintptr_t target_address = 0;
  std::uintptr_t audit_address = 0;
  if (!IndexedAddress(session->payload.targets,
                      sizeof(protocol::FrameTarget), frame_index,
                      &target_address) ||
      !IndexedAddress(session->payload.audits,
                      sizeof(protocol::TransactionAudit), audit_index,
                      &audit_address))
    return FailClosed(session, host::Result::kAddressOverflow);

  protocol::Control control{};
  protocol::Evidence evidence{};
  protocol::FrameTarget observed_target{};
  protocol::TransactionAudit audit{};
  std::uintptr_t observed_vptr = 0;
  if (!Read(io, session->payload.control, &control, sizeof(control)) ||
      !Read(io, session->payload.evidence, &evidence, sizeof(evidence)) ||
      !Read(io, target_address, &observed_target, sizeof(observed_target)) ||
      !Read(io, audit_address, &audit, sizeof(audit)) ||
      !Read(io, session->object, &observed_vptr, sizeof(observed_vptr)) ||
      observed_vptr != session->original_vptr ||
      !ControlMatchesExact(*session, control,
                           session->last_control_audit_index,
                           session->last_control_frame_index,
                           protocol::kDisarmedToken) ||
      !host::EvidenceUnchanged(session->last_evidence, evidence) ||
      !AuditEmpty(audit) ||
      std::memcmp(&observed_target, &expected_target,
                  sizeof(observed_target)) != 0 ||
      !ReadOriginalTableExact(io, *session) ||
      !ReadShadowExact(io, *session) ||
      !ReadNativeBindingExact(io, *session))
    return FailClosed(session, host::Result::kUnexpectedBuildIdentity);

  if (!Write(io, session->payload.control +
                     offsetof(protocol::Control, active_audit_index),
             &audit_index, sizeof(audit_index)) ||
      !Read(io, session->payload.control, &control, sizeof(control)) ||
      !ControlMatchesExact(*session, control, audit_index,
                           session->last_control_frame_index,
                           protocol::kDisarmedToken) ||
      !Write(io, session->payload.control +
                     offsetof(protocol::Control, active_frame_index),
             &frame_index, sizeof(frame_index)) ||
      !Read(io, session->payload.control, &control, sizeof(control)) ||
      !ControlMatchesExact(*session, control, audit_index, frame_index,
                           protocol::kDisarmedToken))
    return FailClosed(session);

  // Host armed intent is published locally before the guest token.  From this
  // point every failed write/readback is mutation-uncertain and terminal.
  const std::uint32_t sequence = session->issued_arm_sequences + 1u;
  const std::uint64_t token = protocol::ArmToken(session->generation, sequence);
  session->frame_armed = true;
  session->armed_audit_index = audit_index;
  session->armed_frame_index = frame_index;
  session->armed_sequence = sequence;
  session->armed_token = token;
  session->armed_target = expected_target;
  session->audit_before_arm = audit;
  session->evidence_before_arm = evidence;
  ++session->issued_arm_sequences;
  ++session->frame_arm_counts[frame_index];
  session->frame_window_ever_armed = true;
  session->last_control_audit_index = audit_index;
  session->last_control_frame_index = frame_index;

  if (!Write(io, session->payload.control +
                     offsetof(protocol::Control, active_token),
             &token, sizeof(token)) ||
      !Read(io, session->payload.control, &control, sizeof(control)) ||
      !ControlMatchesExact(*session, control, audit_index, frame_index, token) ||
      !Read(io, session->object, &observed_vptr, sizeof(observed_vptr)) ||
      observed_vptr != session->original_vptr ||
      !Read(io, session->payload.evidence, &evidence, sizeof(evidence)) ||
      !host::EvidenceUnchanged(session->evidence_before_arm, evidence) ||
      !Read(io, audit_address, &audit, sizeof(audit)) ||
      std::memcmp(&audit, &session->audit_before_arm, sizeof(audit)) != 0 ||
      !ReadOriginalTableExact(io, *session))
    return FailClosed(session);

  // The shadow vptr is the final guest publish write.
  if (!Write(io, session->object, &session->prepared.shadow_vptr,
             sizeof(session->prepared.shadow_vptr)) ||
      !Read(io, session->object, &observed_vptr, sizeof(observed_vptr)) ||
      observed_vptr != session->prepared.shadow_vptr ||
      !Read(io, session->payload.control, &control, sizeof(control)) ||
      !ControlMatchesExact(*session, control, audit_index, frame_index, token) ||
      !Read(io, session->payload.evidence, &evidence, sizeof(evidence)) ||
      !host::EvidenceUnchanged(session->evidence_before_arm, evidence) ||
      !Read(io, audit_address, &audit, sizeof(audit)) ||
      std::memcmp(&audit, &session->audit_before_arm, sizeof(audit)) != 0 ||
      !ReadShadowExact(io, *session) ||
      session->transaction.MarkShadowInstalled(observed_vptr, control) !=
          host::Result::kOk)
    return FailClosed(session);
  return true;
}

inline bool AuditMatchesArmedFrame(
    const Session& session, const protocol::TransactionAudit& audit,
    const protocol::Evidence& evidence) {
  const std::uint32_t common_flags =
      protocol::kAuditFirstNaturalCallReturned |
      protocol::kAuditSecondNaturalCallReturned |
      protocol::kAuditImmediateExact | protocol::kAuditTokenDisarmed |
      protocol::kAuditVptrRestored;
  const bool skipped =
      (session.armed_target.skip_override_flags &
       a9tas::unified_tick_v1::kSkipBarrelAngular) != 0;
  const std::uint32_t expected_flags =
      common_flags |
      (skipped ? protocol::kAuditSkipped : protocol::kAuditOverridden);
  const std::uint32_t* expected_bits =
      skipped ? audit.before_bits : session.armed_target.angular_bits;
  return audit.token == session.armed_token &&
         audit.frame_index == session.armed_frame_index &&
         audit.flags == expected_flags &&
         audit.tid == session.prepared.published_control.expected_tid &&
         audit.natural_calls == 2 && audit.reserved == 0 &&
         std::memcmp(audit.immediate_bits, expected_bits,
                     sizeof(audit.immediate_bits)) == 0 &&
         std::memcmp(audit.captured_bits, expected_bits,
                     sizeof(audit.captured_bits)) == 0 &&
         evidence.last_token == session.armed_token &&
         evidence.last_tid == session.prepared.published_control.expected_tid &&
         evidence.last_object == session.object &&
         evidence.final_vptr == session.original_vptr;
}

inline bool ReconcileCompletedWindow(const Io& io,
                                     std::uint32_t stopped_tid,
                                     bool frame_terminal, Session* session) {
  if (session == nullptr || !StoppedOwnerMatches(*session, stopped_tid) ||
      !session->installed ||
      !session->frame_window_active || !session->frame_armed ||
      session->faulted ||
      session->armed_frame_index != session->active_window_frame_index)
    return false;
  std::uintptr_t audit_address = 0;
  std::uintptr_t target_address = 0;
  if (!IndexedAddress(session->payload.audits,
                      sizeof(protocol::TransactionAudit),
                      session->armed_audit_index, &audit_address) ||
      !IndexedAddress(session->payload.targets, sizeof(protocol::FrameTarget),
                      session->armed_frame_index, &target_address))
    return FailClosed(session, host::Result::kAddressOverflow);
  protocol::Control control{};
  protocol::Evidence evidence{};
  protocol::TransactionAudit audit{};
  protocol::FrameTarget target{};
  std::uintptr_t observed_vptr = 0;
  const std::uint32_t expected = session->completed_transactions + 1u;
  if (!Read(io, session->payload.control, &control, sizeof(control)) ||
      !Read(io, session->payload.evidence, &evidence, sizeof(evidence)) ||
      !Read(io, session->object, &observed_vptr, sizeof(observed_vptr)) ||
      !Read(io, audit_address, &audit, sizeof(audit)) ||
      !Read(io, target_address, &target, sizeof(target)) ||
      observed_vptr != session->original_vptr ||
      !StaticControlMatches(*session, control, true) ||
      !ControlMatchesExact(*session, control, session->armed_audit_index,
                           session->armed_frame_index,
                           protocol::kDisarmedToken) ||
      std::memcmp(&target, &session->armed_target, sizeof(target)) != 0 ||
      !ReadOriginalTableExact(io, *session) ||
      !ReadShadowExact(io, *session) ||
      !ReadNativeBindingExact(io, *session) ||
      !host::FrameTerminal(control, evidence, expected) ||
      !host::EvidenceDeltaMatches(control, session->evidence_before_arm,
                                  evidence, session->armed_token,
                                  session->armed_audit_index) ||
      !AuditMatchesArmedFrame(*session, audit, evidence) ||
      session->transaction.MarkFrameTerminal(
          observed_vptr, control, session->evidence_before_arm, evidence) !=
          host::Result::kOk)
    return FailClosed(session, host::Result::kEvidenceDeltaMismatch);
  session->completed_transactions = expected;
  session->last_evidence = evidence;
  ClearArmedIntent(session);
  if (frame_terminal) {
    session->frame_window_active = false;
    session->frame_window_ever_armed = false;
  }
  return true;
}

inline bool ObserveFrameTerminalAtF64(const Io& io,
                                      std::uint32_t stopped_tid,
                                      Session* session) {
  return ReconcileCompletedWindow(io, stopped_tid, true, session);
}

// Close a frame whose last completed payload transaction was already
// reconciled at an earlier stopped auxiliary event.  This is distinct from a
// truly idle frame: the window was armed, but is now provably original,
// disarmed, and evidence-stable at F64.
inline bool ObserveSettledAtF64(const Io& io, std::uint32_t stopped_tid,
                                Session* session) {
  if (session == nullptr || !StoppedOwnerMatches(*session, stopped_tid) ||
      !session->installed || !session->frame_window_active ||
      !session->frame_window_ever_armed || session->frame_armed ||
      session->completed_transactions == 0 || session->faulted)
    return false;
  protocol::Control control{};
  protocol::Evidence evidence{};
  std::uintptr_t observed_vptr = 0;
  if (!Read(io, session->payload.control, &control, sizeof(control)) ||
      !Read(io, session->payload.evidence, &evidence, sizeof(evidence)) ||
      !Read(io, session->object, &observed_vptr, sizeof(observed_vptr)) ||
      observed_vptr != session->original_vptr ||
      !ControlMatchesExact(*session, control,
                           session->last_control_audit_index,
                           session->last_control_frame_index,
                           protocol::kDisarmedToken) ||
      !host::EvidenceUnchanged(session->last_evidence, evidence) ||
      !ReadOriginalTableExact(io, *session) ||
      !ReadShadowExact(io, *session) ||
      !ReadNativeBindingExact(io, *session))
    return FailClosed(session, host::Result::kEvidenceDeltaMismatch);
  session->frame_window_active = false;
  session->frame_window_ever_armed = false;
  return true;
}

inline bool CancelAbsentFrameAtF64(const Io& io,
                                   std::uint32_t stopped_tid,
                                   Session* session) {
  if (session == nullptr || !StoppedOwnerMatches(*session, stopped_tid) ||
      !session->installed ||
      !session->frame_window_active || !session->frame_armed ||
      session->faulted ||
      session->armed_frame_index != session->active_window_frame_index)
    return false;
  std::uintptr_t audit_address = 0;
  std::uintptr_t target_address = 0;
  if (!IndexedAddress(session->payload.audits,
                      sizeof(protocol::TransactionAudit),
                      session->armed_audit_index, &audit_address) ||
      !IndexedAddress(session->payload.targets, sizeof(protocol::FrameTarget),
                      session->armed_frame_index, &target_address))
    return FailClosed(session, host::Result::kAddressOverflow);
  protocol::Control control{};
  protocol::Evidence evidence{};
  protocol::TransactionAudit audit{};
  protocol::FrameTarget target{};
  std::uintptr_t observed_vptr = 0;
  if (!Read(io, session->payload.control, &control, sizeof(control)) ||
      !Read(io, session->payload.evidence, &evidence, sizeof(evidence)) ||
      !Read(io, session->object, &observed_vptr, sizeof(observed_vptr)) ||
      !Read(io, audit_address, &audit, sizeof(audit)) ||
      !Read(io, target_address, &target, sizeof(target)) ||
      observed_vptr != session->prepared.shadow_vptr ||
      !ControlMatchesExact(*session, control, session->armed_audit_index,
                           session->armed_frame_index,
                           session->armed_token) ||
      !host::EvidenceUnchanged(session->evidence_before_arm, evidence) ||
      std::memcmp(&audit, &session->audit_before_arm, sizeof(audit)) != 0 ||
      std::memcmp(&target, &session->armed_target, sizeof(target)) != 0 ||
      !ReadOriginalTableExact(io, *session) ||
      !ReadShadowExact(io, *session) ||
      !ReadNativeBindingExact(io, *session))
    return FailClosed(session);

  // Make the object safe first.  If clearing the token subsequently becomes
  // mutation-uncertain, an armed token can no longer reach the wrapper through
  // this object.  The inverse ordering could leave a disarmed shadow installed.
  if (!RestoreVptr(io, session) ||
      !Read(io, session->object, &observed_vptr, sizeof(observed_vptr)) ||
      observed_vptr != session->original_vptr ||
      !Read(io, session->payload.control, &control, sizeof(control)) ||
      !ControlMatchesExact(*session, control, session->armed_audit_index,
                           session->armed_frame_index,
                           session->armed_token) ||
      !Read(io, session->payload.evidence, &evidence, sizeof(evidence)) ||
      !host::EvidenceUnchanged(session->evidence_before_arm, evidence) ||
      !Read(io, audit_address, &audit, sizeof(audit)) ||
      std::memcmp(&audit, &session->audit_before_arm, sizeof(audit)) != 0)
    return FailClosed(session);

  const std::uint64_t disarmed = protocol::kDisarmedToken;
  if (!Write(io, session->payload.control +
                     offsetof(protocol::Control, active_token),
             &disarmed, sizeof(disarmed)) ||
      !Read(io, session->object, &observed_vptr, sizeof(observed_vptr)) ||
      observed_vptr != session->original_vptr ||
      !Read(io, session->payload.control, &control, sizeof(control)) ||
      !ControlMatchesExact(*session, control, session->armed_audit_index,
                           session->armed_frame_index,
                           protocol::kDisarmedToken) ||
      !Read(io, session->payload.evidence, &evidence, sizeof(evidence)) ||
      !host::EvidenceUnchanged(session->evidence_before_arm, evidence) ||
      !Read(io, audit_address, &audit, sizeof(audit)) ||
      std::memcmp(&audit, &session->audit_before_arm, sizeof(audit)) != 0 ||
      !ReadOriginalTableExact(io, *session) ||
      session->transaction.MarkFrameCancelled(
          observed_vptr, control, session->evidence_before_arm, evidence) !=
          host::Result::kOk)
    return FailClosed(session);
  session->last_evidence = evidence;
  ClearArmedIntent(session);
  session->frame_window_active = false;
  session->frame_window_ever_armed = false;
  return true;
}

inline bool ObserveIdleAtF64(const Io& io, std::uint32_t stopped_tid,
                             Session* session) {
  if (session == nullptr || !StoppedOwnerMatches(*session, stopped_tid) ||
      !session->installed ||
      !session->frame_window_active || session->frame_window_ever_armed ||
      session->frame_armed || session->faulted)
    return false;
  protocol::Control control{};
  protocol::Evidence evidence{};
  std::uintptr_t observed_vptr = 0;
  if (!Read(io, session->payload.control, &control, sizeof(control)) ||
      !Read(io, session->payload.evidence, &evidence, sizeof(evidence)) ||
      !Read(io, session->object, &observed_vptr, sizeof(observed_vptr)) ||
      observed_vptr != session->original_vptr ||
      !StaticControlMatches(*session, control, true) ||
      !ControlMatchesExact(*session, control,
                           session->last_control_audit_index,
                           session->last_control_frame_index,
                           protocol::kDisarmedToken) ||
      !host::EvidenceUnchanged(session->frame_start_evidence, evidence) ||
      !host::EvidenceUnchanged(session->last_evidence, evidence) ||
      !ReadOriginalTableExact(io, *session) ||
      !ReadShadowExact(io, *session) ||
      !ReadNativeBindingExact(io, *session))
    return FailClosed(session, host::Result::kEvidenceDeltaMismatch);
  session->frame_window_active = false;
  return true;
}

inline bool Finish(const Io& io, std::uint32_t stopped_tid,
                   bool require_completed_transaction,
                   Session* session) {
  if (session == nullptr || !StoppedOwnerMatches(*session, stopped_tid) ||
      !session->installed || session->frame_armed ||
      session->frame_window_active || session->faulted)
    return false;
  protocol::Control control{};
  protocol::Evidence evidence{};
  std::uintptr_t observed_vptr = 0;
  if (!Read(io, session->payload.control, &control, sizeof(control)) ||
      !Read(io, session->payload.evidence, &evidence, sizeof(evidence)) ||
      !ReadKnownObjectVptr(io, session, &observed_vptr) ||
      observed_vptr != session->original_vptr ||
      !ControlMatchesExact(*session, control,
                           session->last_control_audit_index,
                           session->last_control_frame_index,
                           protocol::kDisarmedToken) ||
      !host::EvidenceUnchanged(session->last_evidence, evidence) ||
      !ReadOriginalTableExact(io, *session) ||
      !ReadShadowExact(io, *session) ||
      !ReadNativeBindingExact(io, *session))
    return FailClosed(session);
  if (require_completed_transaction) {
    if (session->completed_transactions == 0 ||
        session->transaction.MarkReplayComplete(
            control, evidence, session->completed_transactions) !=
            host::Result::kOk)
      return FailClosed(session, host::Result::kIncompleteEvidence);
  } else if (session->transaction.RequireRollback(
                 host::Result::kIncompleteEvidence) !=
             host::Result::kIncompleteEvidence) {
    return FailClosed(session);
  }
  if (session->transaction.MarkRolledBack(
          observed_vptr, session->original_vptr, control, evidence) !=
      host::Result::kOk)
    return FailClosed(session, host::Result::kUnsafeRollback);
  session->restore_observed_before = observed_vptr;
  session->restore_observed_after = observed_vptr;
  session->installed = false;
  return true;
}

}  // namespace a9tas::barrel_yaw_tail_transport_v1
