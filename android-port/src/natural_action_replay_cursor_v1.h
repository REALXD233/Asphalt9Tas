#pragma once

// Pure host-side cursor contract for AluTasV2-compatible natural actions.
//
// Every authoritative A9UTK1 frame publishes exactly one mailbox command,
// including frames whose activation count is zero or whose Nitro override is
// skipped.  A frame may not commit, and the next frame may not publish, until
// both the game-owned action receipt and the matching final-writer receipt are
// present.  No process access or live-game primitive belongs in this layer.

#include "natural_action_replay_transport_v1.h"

#include <cstdint>
#include <cstring>

namespace a9tas::natural_action_replay_cursor_v1 {

namespace mailbox = a9tas::natural_action_callback_v1;
namespace replay = a9tas::natural_action_replay_v1;
namespace recording = a9tas::unified_tick_v1;

enum class Phase : std::uint32_t {
    kReady = 0,
    kPrepared = 1,
    kPublished = 2,
    kActionCompleted = 3,
    kWriterAcknowledged = 4,
    kReadyToCommit = 5,
    kComplete = 6,
    kFaulted = 7,
};

enum class Result : std::int32_t {
    kOk = 0,
    kInvalidConfiguration = -1,
    kWrongPhase = -2,
    kExternalCursorMismatch = -3,
    kFrameCursorMismatch = -4,
    kCommandConstructionFailed = -5,
    kMailboxInvalid = -6,
    kMailboxSessionMismatch = -7,
    kMailboxSequenceMismatch = -8,
    kPublishedCommandMismatch = -9,
    kMailboxFault = -10,
    kCompletionMismatch = -11,
    kWriterReceiptMismatch = -12,
};

class Binding {
 public:
    Binding(std::uint32_t frame_count, std::uint32_t session_id,
            std::uintptr_t vehicle_owner, std::uint32_t producer_tid)
        : frame_count_(frame_count),
          session_id_(session_id),
          vehicle_owner_(vehicle_owner),
          producer_tid_(producer_tid) {
        if (frame_count_ == 0 ||
            frame_count_ > recording::kMaximumFrames || session_id_ == 0 ||
            vehicle_owner_ == 0 || producer_tid_ == 0) {
            Fault(Result::kInvalidConfiguration);
        }
    }

    Result PrepareFrame(std::uint32_t external_index,
                        const recording::RecordingFrameV1& frame,
                        mailbox::Command* command) {
        if (phase_ != Phase::kReady) return Fault(Result::kWrongPhase);
        if (external_index != next_index_)
            return Fault(Result::kExternalCursorMismatch);
        if (frame.tick != external_index)
            return Fault(Result::kFrameCursorMismatch);
        mailbox::Command prepared{};
        if (!replay::BuildFrameCommand(
                frame, static_cast<std::uint64_t>(external_index) + 1,
                session_id_, vehicle_owner_, producer_tid_, &prepared)) {
            return Fault(Result::kCommandConstructionFailed);
        }
        expected_ = prepared;
        action_completed_ = false;
        writer_acknowledged_ = false;
        phase_ = Phase::kPrepared;
        if (command != nullptr) *command = prepared;
        return last_result_ = Result::kOk;
    }

    // Accepts a coherent mailbox snapshot read after the host has published
    // the prepared command.  The consumer may already have claimed or even
    // completed it before this first read.
    Result ObserveMailbox(std::uint32_t external_index,
                          const mailbox::Mailbox& snapshot) {
        if (phase_ != Phase::kPrepared && phase_ != Phase::kPublished &&
            phase_ != Phase::kWriterAcknowledged &&
            phase_ != Phase::kActionCompleted &&
            phase_ != Phase::kReadyToCommit) {
            return Fault(Result::kWrongPhase);
        }
        if (external_index != next_index_)
            return Fault(Result::kExternalCursorMismatch);
        if (!mailbox::MailboxValid(snapshot))
            return Fault(Result::kMailboxInvalid);
        const std::uint64_t session = mailbox::LoadAcquire(
            &snapshot.session_control);
        if (!mailbox::Armed(session) ||
            mailbox::SessionId(session) != session_id_) {
            return Fault(Result::kMailboxSessionMismatch);
        }
        const std::uint64_t selector = mailbox::LoadAcquire(
            &snapshot.published_selector);
        const std::uint64_t expected_sequence = expected_.sequence;
        if ((selector >> 1) != expected_sequence)
            return Fault(Result::kMailboxSequenceMismatch);
        const std::uint32_t slot = static_cast<std::uint32_t>(selector & 1u);
        if (std::memcmp(&snapshot.slots[slot], &expected_,
                        sizeof(expected_)) != 0) {
            return Fault(Result::kPublishedCommandMismatch);
        }
        const std::int32_t raw_result = __atomic_load_n(
            &snapshot.result, __ATOMIC_ACQUIRE);
        if (raw_result < 0) return Fault(Result::kMailboxFault);
        const std::uint64_t claimed = mailbox::LoadAcquire(
            &snapshot.claimed_sequence);
        const std::uint64_t completed = mailbox::LoadAcquire(
            &snapshot.completed_sequence);
        if (claimed > expected_sequence || completed > expected_sequence ||
            completed > claimed || claimed + 1 < expected_sequence ||
            completed + 1 < expected_sequence) {
            return Fault(Result::kMailboxSequenceMismatch);
        }
        if (claimed == expected_sequence &&
            __atomic_load_n(&snapshot.claimed_frame, __ATOMIC_ACQUIRE) !=
                external_index) {
            return Fault(Result::kCompletionMismatch);
        }
        if (completed == expected_sequence) {
            if (!mailbox::CompletionMatches(
                    snapshot, expected_sequence, external_index,
                    expected_.nitro_activations)) {
                return Fault(Result::kCompletionMismatch);
            }
            action_completed_ = true;
        }
        UpdatePhase();
        return last_result_ = Result::kOk;
    }

    // The caller invokes this only after the existing final-writer binding
    // has accepted its per-frame callback-close evidence.
    Result AcknowledgeWriter(std::uint32_t external_index,
                             std::uint32_t writer_processed_frames) {
        if (phase_ != Phase::kPublished &&
            phase_ != Phase::kActionCompleted) {
            return Fault(Result::kWrongPhase);
        }
        if (external_index != next_index_)
            return Fault(Result::kExternalCursorMismatch);
        if (writer_processed_frames != external_index + 1)
            return Fault(Result::kWriterReceiptMismatch);
        writer_acknowledged_ = true;
        UpdatePhase();
        return last_result_ = Result::kOk;
    }

    bool CanCommit(std::uint32_t external_index) const {
        return phase_ == Phase::kReadyToCommit &&
               external_index == next_index_ && action_completed_ &&
               writer_acknowledged_;
    }

    Result CommitFrame(std::uint32_t external_index) {
        if (!CanCommit(external_index)) return Fault(Result::kWrongPhase);
        ++next_index_;
        action_completed_ = false;
        writer_acknowledged_ = false;
        std::memset(&expected_, 0, sizeof(expected_));
        phase_ = next_index_ == frame_count_ ? Phase::kComplete
                                             : Phase::kReady;
        return last_result_ = Result::kOk;
    }

    Phase phase() const { return phase_; }
    Result last_result() const { return last_result_; }
    std::uint32_t next_index() const { return next_index_; }
    std::uint32_t frame_count() const { return frame_count_; }
    const mailbox::Command& expected_command() const { return expected_; }

 private:
    void UpdatePhase() {
        if (action_completed_ && writer_acknowledged_)
            phase_ = Phase::kReadyToCommit;
        else if (action_completed_)
            phase_ = Phase::kActionCompleted;
        else if (writer_acknowledged_)
            phase_ = Phase::kWriterAcknowledged;
        else
            phase_ = Phase::kPublished;
    }

    Result Fault(Result result) {
        phase_ = Phase::kFaulted;
        return last_result_ = result;
    }

    std::uint32_t frame_count_{};
    std::uint32_t session_id_{};
    std::uintptr_t vehicle_owner_{};
    std::uint32_t producer_tid_{};
    std::uint32_t next_index_{};
    mailbox::Command expected_{};
    bool action_completed_{};
    bool writer_acknowledged_{};
    Phase phase_{Phase::kReady};
    Result last_result_{Result::kOk};
};

}  // namespace a9tas::natural_action_replay_cursor_v1
