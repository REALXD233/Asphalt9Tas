#pragma once

// Pure preparation/rollback core for the finite final-writer transaction.
// It builds bytes for a future guarded host controller but performs no target
// access itself.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "final_writer_replay_protocol_v1.h"

namespace a9tas::final_writer_transaction_core_v1 {

using namespace a9tas::final_writer_replay_v1;

inline constexpr std::uint64_t kExpectedPrefix[11] = {
    0x17D8, 0x17A8, 0x1398, 0x1778, 0x13C8, 0x1748,
    0x1748, 0x13C8, 0x1398, 0,      0,
};
inline constexpr std::uint64_t kExpectedSlots[4] = {
    0x367AB34, 0x367AD84, kOriginalCallbackRva, 0x36818A0,
};

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kAddressOverflow = -2,
  kUnexpectedOriginalTable = -3,
  kUnexpectedOriginalVptr = -4,
  kPayloadNotFresh = -5,
  kUnexpectedTransactionPhase = -6,
  kUnsafeVptrForRollback = -7,
  kIncompleteEvidence = -8,
};

enum class Phase : std::uint32_t {
  kPreflight = 0,
  kPrepared = 1,
  kPublished = 2,
  kInstalled = 3,
  kComplete = 4,
  kRollbackRequired = 5,
  kRolledBack = 6,
  kFaulted = 7,
};

struct Prepared {
  alignas(64) std::array<std::uint8_t, kPrimaryShadowSize> shadow{};
  alignas(64) Control unpublished_control{};
  alignas(64) Control published_control{};
  alignas(64) Evidence fresh_evidence{};
  std::uintptr_t shadow_vptr{};
};

inline bool Add(std::uintptr_t base, std::uintptr_t rva,
                std::uintptr_t* output) {
  if (output == nullptr || rva > UINTPTR_MAX - base) return false;
  *output = base + rva;
  return true;
}

inline bool IsFresh(const Control& control, const Evidence& evidence) {
  return std::memcmp(control.magic, kControlMagic, 8) == 0 &&
         control.version == kProtocolVersion && control.size == sizeof(Control) &&
         control.flags == 0 && control.frame_count == 0 &&
         control.expected_object == 0 && control.original_vptr == 0 &&
         control.shadow_vptr == 0 && control.original_callback == 0 &&
         control.native_pose == 0 && control.native_linear == 0 &&
         control.reserved[0] == kFramePermitDisarmed &&
         control.reserved[1] == 0 &&
         std::memcmp(evidence.magic, kEvidenceMagic, 8) == 0 &&
         evidence.version == kProtocolVersion && evidence.size == sizeof(Evidence) &&
         evidence.wrapper_entries == 0 && evidence.original_calls == 0 &&
         evidence.clean_returns == 0 && evidence.equal_frames == 0 &&
         evidence.corrected_frames == 0 && evidence.correction_writes == 0 &&
         evidence.failures == 0 && evidence.recursive_entries == 0 &&
         evidence.processed_frames == 0 && evidence.last_status == kStatusPassive;
}

inline Result Prepare(const std::uint8_t original_table[kPrimaryShadowSize],
                      std::uintptr_t library_base,
                      std::uintptr_t object,
                      std::uintptr_t observed_original_vptr,
                      std::uintptr_t payload_shadow,
                      std::uintptr_t wrapper,
                      std::uintptr_t native_pose,
                      std::uintptr_t native_linear,
                      const std::uint8_t source_sha256[32],
                      std::uint32_t frame_count,
                      const Control& initial_control,
                      const Evidence& initial_evidence,
                      Prepared* output) {
  if (original_table == nullptr || source_sha256 == nullptr || output == nullptr ||
      object == 0 || payload_shadow == 0 || wrapper == 0 || native_pose == 0 ||
      native_linear == 0 || frame_count < 2 || frame_count > kMaximumFrames ||
      (payload_shadow & 63u) != 0 || (wrapper & 3u) != 0)
    return Result::kInvalidArgument;
  std::uintptr_t expected_vptr = 0, original_callback = 0;
  if (!Add(library_base, kPrimaryAddressPointRva, &expected_vptr) ||
      !Add(library_base, kOriginalCallbackRva, &original_callback) ||
      payload_shadow > UINTPTR_MAX - kPrimaryPrefixSize)
    return Result::kAddressOverflow;
  if (observed_original_vptr != expected_vptr)
    return Result::kUnexpectedOriginalVptr;
  if (std::memcmp(original_table, kExpectedPrefix, sizeof(kExpectedPrefix)) != 0)
    return Result::kUnexpectedOriginalTable;
  for (std::size_t index = 0; index < std::size(kExpectedSlots); ++index) {
    std::uintptr_t slot = 0, expected_slot = 0;
    std::memcpy(&slot, original_table + kPrimaryPrefixSize + index * 8, 8);
    if (!Add(library_base, kExpectedSlots[index], &expected_slot) ||
        slot != expected_slot)
      return Result::kUnexpectedOriginalTable;
  }
  if (!IsFresh(initial_control, initial_evidence))
    return Result::kPayloadNotFresh;

  // Aggregate value-initialization does not promise canonical bytes for
  // padding nested inside the alignas(64) Evidence object.  Those bytes are
  // staged into the remote payload and later serialized into A9FWR1, so make
  // the complete trivially-copyable transaction deterministic.
  Prepared result;
  std::memset(&result, 0, sizeof(result));
  std::memcpy(result.shadow.data(), original_table, result.shadow.size());
  std::memcpy(result.shadow.data() + kPrimaryPrefixSize + kCallbackSlotOffset,
              &wrapper, sizeof(wrapper));
  result.shadow_vptr = payload_shadow + kPrimaryPrefixSize;

  std::memcpy(result.unpublished_control.magic, kControlMagic, 8);
  result.unpublished_control.version = kProtocolVersion;
  result.unpublished_control.size = sizeof(Control);
  result.unpublished_control.flags = 0;
  result.unpublished_control.frame_count = frame_count;
  result.unpublished_control.expected_object = object;
  result.unpublished_control.original_vptr = expected_vptr;
  result.unpublished_control.shadow_vptr = result.shadow_vptr;
  result.unpublished_control.original_callback = original_callback;
  result.unpublished_control.native_pose = native_pose;
  result.unpublished_control.native_linear = native_linear;
  std::memcpy(result.unpublished_control.recording_sha256, source_sha256, 32);
  result.published_control = result.unpublished_control;
  result.published_control.flags = kControlConfigured | kControlTargetsLoaded;

  std::memcpy(result.fresh_evidence.magic, kEvidenceMagic, 8);
  result.fresh_evidence.version = kProtocolVersion;
  result.fresh_evidence.size = sizeof(Evidence);
  result.fresh_evidence.last_status = kStatusPassive;
  std::memcpy(output, &result, sizeof(result));
  return Result::kOk;
}

inline bool EvidenceComplete(const Evidence& evidence,
                             const Control& published_control) {
  const std::uint32_t count = published_control.frame_count;
  return count >= 2 && count <= kMaximumFrames &&
         std::memcmp(evidence.magic, kEvidenceMagic, 8) == 0 &&
         evidence.version == kProtocolVersion && evidence.size == sizeof(Evidence) &&
         evidence.wrapper_entries == count && evidence.original_calls == count &&
         evidence.clean_returns == count && evidence.processed_frames == count &&
         evidence.equal_frames + evidence.corrected_frames == count &&
         evidence.correction_writes == evidence.corrected_frames * 2u &&
         evidence.failures == 0 && evidence.recursive_entries == 0 &&
         evidence.last_object == published_control.expected_object &&
         evidence.observed_vptr == published_control.shadow_vptr &&
         evidence.final_vptr == published_control.original_vptr &&
         evidence.last_status == kStatusComplete &&
         published_control.reserved[0] == kFramePermitDisarmed;
}

class Transaction {
 public:
  Result MarkPrepared() {
    if (phase_ != Phase::kPreflight) return Fault(Result::kUnexpectedTransactionPhase);
    phase_ = Phase::kPrepared;
    return last_ = Result::kOk;
  }
  Result MarkPublished() {
    if (phase_ != Phase::kPrepared) return Fault(Result::kUnexpectedTransactionPhase);
    phase_ = Phase::kPublished;
    return last_ = Result::kOk;
  }
  Result MarkInstalled() {
    if (phase_ != Phase::kPublished) return Fault(Result::kUnexpectedTransactionPhase);
    phase_ = Phase::kInstalled;
    return last_ = Result::kOk;
  }
  Result MarkComplete(const Evidence& evidence, const Control& control,
                      std::uintptr_t observed_vptr) {
    if (phase_ != Phase::kInstalled) return Fault(Result::kUnexpectedTransactionPhase);
    if (!EvidenceComplete(evidence, control))
      return RequireRollback(Result::kIncompleteEvidence);
    if (observed_vptr != control.original_vptr)
      return RequireRollback(Result::kUnsafeVptrForRollback);
    phase_ = Phase::kComplete;
    return last_ = Result::kOk;
  }
  Result RequireRollback(Result cause) {
    if (phase_ == Phase::kComplete || phase_ == Phase::kRolledBack ||
        phase_ == Phase::kFaulted)
      return Fault(Result::kUnexpectedTransactionPhase);
    phase_ = Phase::kRollbackRequired;
    return last_ = cause;
  }
  Result MarkRolledBack(std::uintptr_t observed_vptr,
                        std::uintptr_t original_vptr) {
    if (phase_ != Phase::kRollbackRequired)
      return Fault(Result::kUnexpectedTransactionPhase);
    if (observed_vptr != original_vptr)
      return Fault(Result::kUnsafeVptrForRollback);
    phase_ = Phase::kRolledBack;
    return last_ = Result::kOk;
  }
  Phase phase() const { return phase_; }
  Result last_result() const { return last_; }

 private:
  Result Fault(Result result) {
    phase_ = Phase::kFaulted;
    return last_ = result;
  }
  Phase phase_{Phase::kPreflight};
  Result last_{Result::kOk};
};

}  // namespace a9tas::final_writer_transaction_core_v1
