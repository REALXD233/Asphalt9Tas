#pragma once

// Pure host-side synchronization contract between the external fixed-delta
// executor and the in-callback final-writer payload.  No process access lives
// here; callers provide coherent Evidence snapshots from the target.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "final_writer_replay_protocol_v1.h"

namespace a9tas::final_writer_cursor_binding_v1 {

using namespace a9tas::final_writer_replay_v1;

enum class Phase : std::uint32_t {
  kReady = 0,
  kAwaitingWriter = 1,
  kWriterAcknowledged = 2,
  kComplete = 3,
  kFaulted = 4,
};

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidConfiguration = -1,
  kSourceIdentityMismatch = -2,
  kWrongPhase = -3,
  kExternalCursorMismatch = -4,
  kPayloadCursorMismatch = -5,
  kCallbackCountMismatch = -6,
  kPayloadFailure = -7,
  kCorrectionAccountingMismatch = -8,
  kPayloadStatusMismatch = -9,
};

inline bool SourceIdentityMatches(const Control& control,
                                  const std::uint8_t source_sha256[32],
                                  std::uint32_t frame_count) {
  return source_sha256 != nullptr && frame_count >= 2 &&
         frame_count <= kMaximumFrames &&
         std::memcmp(control.magic, kControlMagic, sizeof(kControlMagic)) == 0 &&
         control.version == kProtocolVersion && control.size == sizeof(Control) &&
         control.flags == (kControlConfigured | kControlTargetsLoaded) &&
         control.frame_count == frame_count &&
         control.reserved[0] == kFramePermitDisarmed &&
         std::memcmp(control.recording_sha256, source_sha256, 32) == 0;
}

class Binding {
 public:
  explicit Binding(std::uint32_t frame_count) : frame_count_(frame_count) {
    if (frame_count_ < 2 || frame_count_ > kMaximumFrames) {
      phase_ = Phase::kFaulted;
      last_result_ = Result::kInvalidConfiguration;
    }
  }

  Result BeginTick(std::uint32_t external_index,
                   const Evidence& evidence) {
    if (phase_ != Phase::kReady) return Fault(Result::kWrongPhase);
    if (external_index != next_index_)
      return Fault(Result::kExternalCursorMismatch);
    if (!ValidateBefore(evidence)) return last_result_;
    phase_ = Phase::kAwaitingWriter;
    return last_result_ = Result::kOk;
  }

  Result AcknowledgeWriter(std::uint32_t external_index,
                           const Evidence& evidence) {
    if (phase_ != Phase::kAwaitingWriter)
      return Fault(Result::kWrongPhase);
    if (external_index != next_index_)
      return Fault(Result::kExternalCursorMismatch);
    const std::uint32_t expected = next_index_ + 1;
    if (evidence.processed_frames != expected)
      return Fault(Result::kPayloadCursorMismatch);
    if (evidence.wrapper_entries != expected ||
        evidence.original_calls != expected ||
        evidence.clean_returns != expected)
      return Fault(Result::kCallbackCountMismatch);
    if (evidence.failures != 0 || evidence.recursive_entries != 0)
      return Fault(Result::kPayloadFailure);
    if (evidence.equal_frames + evidence.corrected_frames != expected ||
        evidence.correction_writes != evidence.corrected_frames * 2u)
      return Fault(Result::kCorrectionAccountingMismatch);
    const std::int32_t expected_status =
        expected == frame_count_ ? kStatusComplete : kStatusPassive;
    if (evidence.last_status != expected_status)
      return Fault(Result::kPayloadStatusMismatch);
    phase_ = Phase::kWriterAcknowledged;
    return last_result_ = Result::kOk;
  }

  Result CommitTick(std::uint32_t external_index,
                    const Evidence& evidence) {
    if (phase_ != Phase::kWriterAcknowledged)
      return Fault(Result::kWrongPhase);
    if (external_index != next_index_)
      return Fault(Result::kExternalCursorMismatch);
    const std::uint32_t expected = next_index_ + 1;
    if (evidence.processed_frames != expected)
      return Fault(Result::kPayloadCursorMismatch);
    ++next_index_;
    phase_ = next_index_ == frame_count_ ? Phase::kComplete : Phase::kReady;
    return last_result_ = Result::kOk;
  }

  Phase phase() const { return phase_; }
  Result last_result() const { return last_result_; }
  std::uint32_t next_index() const { return next_index_; }
  std::uint32_t frame_count() const { return frame_count_; }

 private:
  bool ValidateBefore(const Evidence& evidence) {
    if (evidence.processed_frames != next_index_) {
      Fault(Result::kPayloadCursorMismatch);
      return false;
    }
    if (evidence.wrapper_entries != next_index_ ||
        evidence.original_calls != next_index_ ||
        evidence.clean_returns != next_index_) {
      Fault(Result::kCallbackCountMismatch);
      return false;
    }
    if (evidence.failures != 0 || evidence.recursive_entries != 0) {
      Fault(Result::kPayloadFailure);
      return false;
    }
    if (evidence.equal_frames + evidence.corrected_frames != next_index_ ||
        evidence.correction_writes != evidence.corrected_frames * 2u) {
      Fault(Result::kCorrectionAccountingMismatch);
      return false;
    }
    if (evidence.last_status != kStatusPassive) {
      Fault(Result::kPayloadStatusMismatch);
      return false;
    }
    return true;
  }

  Result Fault(Result result) {
    phase_ = Phase::kFaulted;
    return last_result_ = result;
  }

  std::uint32_t frame_count_{};
  std::uint32_t next_index_{};
  Phase phase_{Phase::kReady};
  Result last_result_{Result::kOk};
};

}  // namespace a9tas::final_writer_cursor_binding_v1
