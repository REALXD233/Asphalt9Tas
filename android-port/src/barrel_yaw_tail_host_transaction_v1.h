#pragma once

// Pure preparation and lifecycle model for the transient BarrelYaw shadow.
// Install publishes only immutable configuration while the object keeps its
// exact original vptr. Each arm has a unique monotonically increasing token;
// the shadow exists only between the stopped-owner arm and the payload's
// second certified natural return (or a stopped-owner absent cancellation).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "barrel_yaw_tail_payload_protocol_v1.h"

namespace a9tas::barrel_yaw_tail_host_v1 {

namespace protocol = a9tas::barrel_yaw_tail_payload_v1;

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kAddressOverflow = -2,
  kUnexpectedOriginalVptr = -3,
  kUnexpectedBoundarySlot = -4,
  kPayloadNotFresh = -5,
  kUnexpectedPhase = -6,
  kUnsafeRollback = -7,
  kIncompleteEvidence = -8,
  kFrameStillArmed = -9,
  kUnexpectedBuildIdentity = -10,
  kNonMonotonicSequence = -11,
  kPerFrameTransactionLimit = -12,
  kEvidenceDeltaMismatch = -13,
};

enum class Phase : std::uint32_t {
  kPreflight = 0,
  kPayloadStaged = 1,
  kConfigurationPublished = 2,
  kFrameArmed = 3,
  kReplayComplete = 4,
  kRollbackRequired = 5,
  kRolledBack = 6,
  kFaulted = 7,
};

struct Prepared {
  alignas(64) std::array<std::uint8_t, protocol::kShadowSize> original{};
  alignas(64) std::array<std::uint8_t, protocol::kShadowSize> shadow{};
  alignas(64) protocol::Control unpublished_control{};
  alignas(64) protocol::Control published_control{};
  alignas(64) protocol::Evidence fresh_evidence{};
  std::uintptr_t library_base{};
  std::uintptr_t shadow_vptr{};
  std::uintptr_t original_boundary_callback{};
};

inline bool AllZero(const void* data, std::size_t size) {
  if (data == nullptr) return false;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t index = 0; index < size; ++index)
    if (bytes[index] != 0) return false;
  return true;
}

inline bool IsFresh(const protocol::Control& control,
                    const protocol::Evidence& evidence) {
  return std::memcmp(control.magic, protocol::kControlMagic, 8) == 0 &&
         control.version == protocol::kProtocolVersion &&
         control.size == sizeof(protocol::Control) && control.flags == 0 &&
         control.frame_count == 0 && control.expected_object == 0 &&
         control.original_vptr == 0 && control.shadow_vptr == 0 &&
         control.original_boundary_callback == 0 &&
         control.native_angular == 0 && control.expected_tid == 0 &&
         control.session_generation == 0 && control.active_audit_index == 0 &&
         control.active_frame_index == 0 &&
         AllZero(control.recording_sha256, sizeof(control.recording_sha256)) &&
         control.active_token == protocol::kDisarmedToken &&
         control.expected_first_caller_return == 0 &&
         control.expected_second_caller_return == 0 &&
         std::memcmp(evidence.magic, protocol::kEvidenceMagic, 8) == 0 &&
         evidence.version == protocol::kProtocolVersion &&
         evidence.size == sizeof(protocol::Evidence) &&
         evidence.wrapper_entries == 0 && evidence.original_calls == 0 &&
         evidence.original_returns == 0 &&
         evidence.completed_transactions == 0 &&
         evidence.override_writes == 0 && evidence.skipped_transactions == 0 &&
         evidence.failures == 0 && evidence.active_call_count == 0 &&
         evidence.audit_count == 0 &&
         evidence.last_status == protocol::kStatusPassive &&
         evidence.last_tid == 0 &&
         evidence.last_token == protocol::kDisarmedToken &&
         evidence.last_object == 0 && evidence.final_vptr == 0 &&
         AllZero(evidence.reserved, sizeof(evidence.reserved));
}

inline Result Prepare(
    const std::uint8_t original_table[protocol::kShadowSize],
    std::uintptr_t object, std::uintptr_t observed_original_vptr,
    std::uintptr_t payload_shadow, std::uintptr_t wrapper,
    std::uintptr_t native_angular, std::uint32_t expected_tid,
    std::uint32_t session_generation, const std::uint8_t recording_sha256[32],
    std::uint32_t frame_count, const protocol::Control& initial_control,
    const protocol::Evidence& initial_evidence, Prepared* output) {
  if (original_table == nullptr || recording_sha256 == nullptr ||
      output == nullptr || object == 0 || observed_original_vptr == 0 ||
      payload_shadow == 0 || wrapper == 0 || native_angular == 0 ||
      expected_tid == 0 || session_generation == 0 || frame_count < 2 ||
      frame_count > protocol::kMaximumFrames || (payload_shadow & 63u) != 0 ||
      (wrapper & 3u) != 0 || (native_angular & 3u) != 0 ||
      AllZero(recording_sha256, 32))
    return Result::kInvalidArgument;
  if (observed_original_vptr < protocol::kPhysicsBackendVptrRva)
    return Result::kUnexpectedOriginalVptr;
  const std::uintptr_t library_base =
      observed_original_vptr - protocol::kPhysicsBackendVptrRva;
  if ((library_base & 0xfffu) != 0 ||
      library_base > UINTPTR_MAX - protocol::kPhysicsBackendVptrRva ||
      library_base + protocol::kPhysicsBackendVptrRva !=
          observed_original_vptr)
    return Result::kUnexpectedBuildIdentity;
  if (payload_shadow > UINTPTR_MAX - protocol::kVptrPrefixSize ||
      library_base > UINTPTR_MAX - protocol::kOriginalBoundaryCallbackRva ||
      library_base > UINTPTR_MAX - protocol::kFirstBoundaryCallerReturnRva ||
      library_base > UINTPTR_MAX - protocol::kSecondBoundaryCallerReturnRva)
    return Result::kAddressOverflow;

  std::uintptr_t original_boundary = 0;
  std::memcpy(&original_boundary,
              original_table + protocol::kVptrPrefixSize +
                  protocol::kBoundarySlotOffset,
              sizeof(original_boundary));
  if (original_boundary !=
          library_base + protocol::kOriginalBoundaryCallbackRva ||
      original_boundary == wrapper)
    return Result::kUnexpectedBoundarySlot;
  if (!IsFresh(initial_control, initial_evidence))
    return Result::kPayloadNotFresh;

  Prepared result{};
  std::memcpy(result.original.data(), original_table, result.original.size());
  std::memcpy(result.shadow.data(), original_table, result.shadow.size());
  std::memcpy(result.shadow.data() + protocol::kVptrPrefixSize +
                  protocol::kBoundarySlotOffset,
              &wrapper, sizeof(wrapper));
  result.library_base = library_base;
  result.shadow_vptr = payload_shadow + protocol::kVptrPrefixSize;
  result.original_boundary_callback = original_boundary;

  std::memcpy(result.unpublished_control.magic, protocol::kControlMagic, 8);
  result.unpublished_control.version = protocol::kProtocolVersion;
  result.unpublished_control.size = sizeof(protocol::Control);
  result.unpublished_control.flags = 0;
  result.unpublished_control.frame_count = frame_count;
  result.unpublished_control.expected_object = object;
  result.unpublished_control.original_vptr = observed_original_vptr;
  result.unpublished_control.shadow_vptr = result.shadow_vptr;
  result.unpublished_control.original_boundary_callback = original_boundary;
  result.unpublished_control.native_angular = native_angular;
  result.unpublished_control.expected_tid = expected_tid;
  result.unpublished_control.session_generation = session_generation;
  result.unpublished_control.active_audit_index = 0;
  result.unpublished_control.active_frame_index = 0;
  std::memcpy(result.unpublished_control.recording_sha256, recording_sha256,
              32);
  result.unpublished_control.active_token = protocol::kDisarmedToken;
  result.unpublished_control.expected_first_caller_return =
      library_base + protocol::kFirstBoundaryCallerReturnRva;
  result.unpublished_control.expected_second_caller_return =
      library_base + protocol::kSecondBoundaryCallerReturnRva;
  result.published_control = result.unpublished_control;
  result.published_control.flags =
      protocol::kControlConfigured | protocol::kControlTargetsLoaded;

  std::memcpy(result.fresh_evidence.magic, protocol::kEvidenceMagic, 8);
  result.fresh_evidence.version = protocol::kProtocolVersion;
  result.fresh_evidence.size = sizeof(protocol::Evidence);
  result.fresh_evidence.last_status = protocol::kStatusPassive;
  *output = result;
  return Result::kOk;
}

inline bool EvidenceUnchanged(const protocol::Evidence& before,
                              const protocol::Evidence& after) {
  return std::memcmp(&before, &after, sizeof(before)) == 0;
}

inline bool EvidenceDeltaMatches(const protocol::Control& control,
                                 const protocol::Evidence& before,
                                 const protocol::Evidence& after,
                                 std::uint64_t token,
                                 std::uint32_t audit_index) {
  if (token == protocol::kDisarmedToken ||
      static_cast<std::uint32_t>(token >> 32) != control.session_generation ||
      static_cast<std::uint32_t>(token) == 0 ||
      audit_index != before.audit_count ||
      before.completed_transactions != before.audit_count)
    return false;
  const bool override_delta =
      after.override_writes == before.override_writes + 1u &&
      after.skipped_transactions == before.skipped_transactions;
  const bool skip_delta =
      after.override_writes == before.override_writes &&
      after.skipped_transactions == before.skipped_transactions + 1u;
  return std::memcmp(after.magic, before.magic, sizeof(after.magic)) == 0 &&
         after.version == before.version && after.size == before.size &&
         after.wrapper_entries == before.wrapper_entries + 2u &&
         after.original_calls == before.original_calls + 2u &&
         after.original_returns == before.original_returns + 2u &&
         after.completed_transactions ==
             before.completed_transactions + 1u &&
         (override_delta != skip_delta) && after.failures == before.failures &&
         after.active_call_count == 0 &&
         after.audit_count == audit_index + 1u &&
         after.last_status == protocol::kStatusComplete &&
         after.last_tid == control.expected_tid && after.last_token == token &&
         after.last_object == control.expected_object &&
         after.final_vptr == control.original_vptr &&
         std::memcmp(after.reserved, before.reserved,
                     sizeof(after.reserved)) == 0;
}

inline bool FrameTerminal(const protocol::Control& control,
                          const protocol::Evidence& evidence,
                          std::uint32_t expected_audits) {
  const std::uint64_t expected_calls =
      static_cast<std::uint64_t>(expected_audits) * 2u;
  return expected_audits <= protocol::kMaximumTransactions &&
         control.active_token == protocol::kDisarmedToken &&
         evidence.active_call_count == 0 && evidence.failures == 0 &&
         evidence.audit_count == expected_audits &&
         evidence.completed_transactions == expected_audits &&
         evidence.wrapper_entries == expected_calls &&
         evidence.original_calls == expected_calls &&
         evidence.original_returns == expected_calls &&
         evidence.override_writes + evidence.skipped_transactions ==
             expected_audits &&
         (expected_audits == 0 ||
          (evidence.last_status == protocol::kStatusComplete &&
           evidence.last_tid == control.expected_tid &&
           evidence.last_object == control.expected_object &&
           evidence.final_vptr == control.original_vptr &&
           evidence.last_token != protocol::kDisarmedToken &&
           static_cast<std::uint32_t>(evidence.last_token >> 32) ==
               control.session_generation)) &&
         AllZero(evidence.reserved, sizeof(evidence.reserved));
}

inline bool SessionEvidenceComplete(const protocol::Control& control,
                                    const protocol::Evidence& evidence,
                                    std::uint32_t expected_transactions) {
  return std::memcmp(control.magic, protocol::kControlMagic, 8) == 0 &&
         control.version == protocol::kProtocolVersion &&
         control.size == sizeof(protocol::Control) &&
         control.flags ==
             (protocol::kControlConfigured |
              protocol::kControlTargetsLoaded) &&
         control.frame_count >= 2 &&
         control.frame_count <= protocol::kMaximumFrames &&
         expected_transactions != 0 &&
         FrameTerminal(control, evidence, expected_transactions);
}

class Transaction {
 public:
  Result MarkPayloadStaged() {
    if (phase_ != Phase::kPreflight) return Fault(Result::kUnexpectedPhase);
    phase_ = Phase::kPayloadStaged;
    return last_ = Result::kOk;
  }

  Result MarkConfigurationPublished(const protocol::Control& published) {
    if (phase_ != Phase::kPayloadStaged)
      return Fault(Result::kUnexpectedPhase);
    if (published.active_token != protocol::kDisarmedToken ||
        published.flags !=
            (protocol::kControlConfigured |
             protocol::kControlTargetsLoaded) ||
        published.active_audit_index != 0 ||
        published.active_frame_index != 0)
      return RequireRollback(Result::kFrameStillArmed);
    phase_ = Phase::kConfigurationPublished;
    return last_ = Result::kOk;
  }

  Result MarkShadowInstalled(std::uintptr_t observed_vptr,
                             const protocol::Control& armed) {
    if (phase_ != Phase::kConfigurationPublished)
      return Fault(Result::kUnexpectedPhase);
    const std::uint32_t sequence =
        static_cast<std::uint32_t>(armed.active_token);
    if (observed_vptr != armed.shadow_vptr ||
        armed.active_token == protocol::kDisarmedToken || sequence == 0 ||
        static_cast<std::uint32_t>(armed.active_token >> 32) !=
            armed.session_generation ||
        armed.active_frame_index >= armed.frame_count ||
        armed.active_audit_index != completed_transactions_ ||
        armed.active_audit_index >= protocol::kMaximumTransactions)
      return RequireRollback(Result::kFrameStillArmed);
    if (last_arm_sequence_ == UINT32_MAX ||
        sequence != last_arm_sequence_ + 1u)
      return RequireRollback(Result::kNonMonotonicSequence);
    if (frame_arm_counts_[armed.active_frame_index] >=
        protocol::kMaximumTransactionsPerFrame)
      return RequireRollback(Result::kPerFrameTransactionLimit);
    ++frame_arm_counts_[armed.active_frame_index];
    last_arm_sequence_ = sequence;
    armed_token_ = armed.active_token;
    armed_audit_index_ = armed.active_audit_index;
    phase_ = Phase::kFrameArmed;
    return last_ = Result::kOk;
  }

  Result MarkFrameTerminal(std::uintptr_t observed_vptr,
                           const protocol::Control& control,
                           const protocol::Evidence& before,
                           const protocol::Evidence& after) {
    if (phase_ != Phase::kFrameArmed)
      return Fault(Result::kUnexpectedPhase);
    if (observed_vptr != control.original_vptr ||
        control.active_token != protocol::kDisarmedToken)
      return RequireRollback(Result::kUnsafeRollback);
    if (!EvidenceDeltaMatches(control, before, after, armed_token_,
                              armed_audit_index_))
      return RequireRollback(Result::kEvidenceDeltaMismatch);
    ++completed_transactions_;
    phase_ = Phase::kConfigurationPublished;
    return last_ = Result::kOk;
  }

  Result MarkFrameCancelled(std::uintptr_t observed_vptr,
                            const protocol::Control& control,
                            const protocol::Evidence& before,
                            const protocol::Evidence& after) {
    if (phase_ != Phase::kFrameArmed)
      return Fault(Result::kUnexpectedPhase);
    if (observed_vptr != control.original_vptr ||
        control.active_token != protocol::kDisarmedToken)
      return RequireRollback(Result::kUnsafeRollback);
    if (!EvidenceUnchanged(before, after))
      return RequireRollback(Result::kEvidenceDeltaMismatch);
    phase_ = Phase::kConfigurationPublished;
    return last_ = Result::kOk;
  }

  Result MarkReplayComplete(const protocol::Control& control,
                            const protocol::Evidence& evidence,
                            std::uint32_t expected_transactions) {
    if (phase_ != Phase::kConfigurationPublished)
      return Fault(Result::kUnexpectedPhase);
    if (control.active_token != protocol::kDisarmedToken ||
        evidence.active_call_count != 0)
      return RequireRollback(Result::kFrameStillArmed);
    if (expected_transactions != completed_transactions_ ||
        !SessionEvidenceComplete(control, evidence, expected_transactions))
      return RequireRollback(Result::kIncompleteEvidence);
    phase_ = Phase::kReplayComplete;
    return last_ = Result::kOk;
  }

  Result RequireRollback(Result cause) {
    if (phase_ == Phase::kRolledBack || phase_ == Phase::kFaulted)
      return Fault(Result::kUnexpectedPhase);
    phase_ = Phase::kRollbackRequired;
    return last_ = cause;
  }

  Result MarkRolledBack(std::uintptr_t observed_vptr,
                        std::uintptr_t original_vptr,
                        const protocol::Control& control,
                        const protocol::Evidence& evidence) {
    if (phase_ != Phase::kRollbackRequired &&
        phase_ != Phase::kReplayComplete)
      return Fault(Result::kUnexpectedPhase);
    if (control.active_token != protocol::kDisarmedToken ||
        evidence.active_call_count != 0)
      return Fault(Result::kFrameStillArmed);
    if (observed_vptr != original_vptr)
      return Fault(Result::kUnsafeRollback);
    phase_ = Phase::kRolledBack;
    return last_ = Result::kOk;
  }

  Phase phase() const { return phase_; }
  Result last_result() const { return last_; }
  std::uint32_t last_arm_sequence() const { return last_arm_sequence_; }
  std::uint32_t completed_transactions() const {
    return completed_transactions_;
  }

 private:
  Result Fault(Result result) {
    phase_ = Phase::kFaulted;
    return last_ = result;
  }

  std::array<std::uint8_t, protocol::kMaximumFrames> frame_arm_counts_{};
  Phase phase_{Phase::kPreflight};
  Result last_{Result::kOk};
  std::uint64_t armed_token_{};
  std::uint32_t armed_audit_index_{};
  std::uint32_t last_arm_sequence_{};
  std::uint32_t completed_transactions_{};
};

}  // namespace a9tas::barrel_yaw_tail_host_v1
