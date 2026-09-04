#pragma once

// Pure composed cursor for one authoritative A9UTK1 replay frame.
//
// The established final-writer callback receipt and the game-owned natural
// action receipt remain independently validated.  This layer permits the
// world-boundary commit only after both receipts identify the same frame.

#include "final_writer_cursor_binding_v1.h"
#include "natural_action_replay_cursor_v1.h"

#include <cstdint>

namespace a9tas::final_writer_natural_action_binding_v1 {

namespace writer = a9tas::final_writer_cursor_binding_v1;
namespace action = a9tas::natural_action_replay_cursor_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;
namespace writer_protocol = a9tas::final_writer_replay_v1;

enum class Result : std::int32_t {
    kOk = 0,
    kFaulted = -1,
    kWriterRejected = -2,
    kActionRejected = -3,
    kCommitNotReady = -4,
    kCursorDiverged = -5,
};

class Binding {
 public:
    Binding(std::uint32_t frame_count, std::uint32_t session_id,
            std::uintptr_t vehicle_owner, std::uint32_t producer_tid)
        : frame_count_(frame_count),
          writer_(frame_count),
          action_(frame_count, session_id, vehicle_owner, producer_tid) {
        if (frame_count < 2 || frame_count > recording::kMaximumFrames ||
            writer_.phase() == writer::Phase::kFaulted ||
            action_.phase() == action::Phase::kFaulted) {
            Fault(Result::kFaulted);
        }
    }

    Result BeginFrame(std::uint32_t external_index,
                      const recording::RecordingFrameV1& frame,
                      const writer_protocol::Evidence& writer_evidence,
                      mailbox::Command* command) {
        if (faulted_) return last_result_ = Result::kFaulted;
        if (writer_.next_index() != action_.next_index() ||
            external_index != writer_.next_index())
            return Fault(Result::kCursorDiverged);
        if (action_.PrepareFrame(external_index, frame, command) !=
            action::Result::kOk)
            return Fault(Result::kActionRejected);
        if (writer_.BeginTick(external_index, writer_evidence) !=
            writer::Result::kOk)
            return Fault(Result::kWriterRejected);
        return last_result_ = Result::kOk;
    }

    Result ObserveAction(std::uint32_t external_index,
                         const mailbox::Mailbox& snapshot) {
        if (faulted_) return last_result_ = Result::kFaulted;
        if (action_.ObserveMailbox(external_index, snapshot) !=
            action::Result::kOk)
            return Fault(Result::kActionRejected);
        return last_result_ = Result::kOk;
    }

    Result AcknowledgeWriter(
        std::uint32_t external_index,
        const writer_protocol::Evidence& writer_evidence) {
        if (faulted_) return last_result_ = Result::kFaulted;
        if (writer_.AcknowledgeWriter(external_index, writer_evidence) !=
            writer::Result::kOk)
            return Fault(Result::kWriterRejected);
        if (action_.AcknowledgeWriter(
                external_index, writer_evidence.processed_frames) !=
            action::Result::kOk)
            return Fault(Result::kActionRejected);
        return last_result_ = Result::kOk;
    }

    bool CanCommit(std::uint32_t external_index) const {
        return !faulted_ && writer_.next_index() == action_.next_index() &&
               writer_.next_index() == external_index &&
               writer_.phase() == writer::Phase::kWriterAcknowledged &&
               action_.CanCommit(external_index);
    }

    Result CommitFrame(std::uint32_t external_index,
                       const writer_protocol::Evidence& writer_evidence) {
        if (!CanCommit(external_index))
            return Fault(Result::kCommitNotReady);
        if (writer_.CommitTick(external_index, writer_evidence) !=
            writer::Result::kOk)
            return Fault(Result::kWriterRejected);
        // CanCommit proved the action transition.  CommitFrame cannot fail
        // without an internal cursor violation, which is latched permanently.
        if (action_.CommitFrame(external_index) != action::Result::kOk)
            return Fault(Result::kActionRejected);
        if (writer_.next_index() != action_.next_index())
            return Fault(Result::kCursorDiverged);
        return last_result_ = Result::kOk;
    }

    bool complete() const {
        return !faulted_ && writer_.next_index() == frame_count_ &&
               action_.next_index() == frame_count_ &&
               writer_.phase() == writer::Phase::kComplete &&
               action_.phase() == action::Phase::kComplete;
    }

    bool faulted() const { return faulted_; }
    Result last_result() const { return last_result_; }
    std::uint32_t next_index() const { return action_.next_index(); }
    const writer::Binding& writer_binding() const { return writer_; }
    const action::Binding& action_binding() const { return action_; }

 private:
    Result Fault(Result result) {
        faulted_ = true;
        return last_result_ = result;
    }

    std::uint32_t frame_count_{};
    writer::Binding writer_;
    action::Binding action_;
    bool faulted_{};
    Result last_result_{Result::kOk};
};

}  // namespace a9tas::final_writer_natural_action_binding_v1
