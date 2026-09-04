#pragma once

// Host-side runtime composition for one authoritative replay frame.
//
// This layer joins the already reviewed final-writer/action cursor with the
// verified external-memory mailbox transport.  It deliberately owns no
// process discovery, ptrace, callback installation, lifecycle transition, or
// Nitro-state write.  A caller may publish frame N+1 only after frame N has
// both its natural-action receipt and final-writer receipt and has committed
// at the world boundary.

#include "final_writer_natural_action_binding_v1.h"
#include "natural_action_replay_host_v1.h"

#include <cstdint>

namespace a9tas::final_writer_natural_action_runtime_v1 {

namespace combined = a9tas::final_writer_natural_action_binding_v1;
namespace host = a9tas::natural_action_replay_host_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;
namespace writer_protocol = a9tas::final_writer_replay_v1;

enum class Result : std::int32_t {
    kOk = 0,
    kFaulted = -1,
    kBeginRejected = -2,
    kPublishRejected = -3,
    kActionReceiptRejected = -4,
    kWriterReceiptRejected = -5,
    kCommitRejected = -6,
};

class Runtime {
 public:
    Runtime(std::uint32_t frame_count, std::uint32_t session_id,
            std::uintptr_t vehicle_owner, std::uint32_t producer_tid,
            host::Layout mailbox_layout)
        : binding_(frame_count, session_id, vehicle_owner, producer_tid),
          mailbox_layout_(mailbox_layout) {
        if (binding_.faulted() || mailbox_layout_.mailbox_address == 0)
            Fault(Result::kFaulted);
    }

    Result BeginAndPublish(
        const host::Backend& backend, std::uint32_t external_index,
        const recording::RecordingFrameV1& frame,
        const writer_protocol::Evidence& writer_evidence) {
        if (faulted_) return last_result_ = Result::kFaulted;
        mailbox::Command command{};
        if (binding_.BeginFrame(external_index, frame, writer_evidence,
                                &command) != combined::Result::kOk)
            return Fault(Result::kBeginRejected);
        mailbox::Mailbox snapshot{};
        if (host::PublishFrame(backend, mailbox_layout_, command, &snapshot) !=
            host::Result::kOk)
            return Fault(Result::kPublishRejected);
        if (binding_.ObserveAction(external_index, snapshot) !=
            combined::Result::kOk)
            return Fault(Result::kActionReceiptRejected);
        return last_result_ = Result::kOk;
    }

    // Invoke at the certified callback-close boundary.  At that point the
    // game-owned persistent consumer must have completed the same frame, and
    // the final-writer payload must expose its matching processed-frame
    // receipt.  Receipt order inside the callback dispatch is irrelevant.
    Result ObserveCallbackClose(
        const host::Backend& backend, std::uint32_t external_index,
        const writer_protocol::Evidence& writer_evidence) {
        if (faulted_) return last_result_ = Result::kFaulted;
        mailbox::Mailbox snapshot{};
        if (host::ReadSnapshot(backend, mailbox_layout_, &snapshot) !=
                host::Result::kOk ||
            binding_.ObserveAction(external_index, snapshot) !=
                combined::Result::kOk)
            return Fault(Result::kActionReceiptRejected);
        if (binding_.AcknowledgeWriter(external_index, writer_evidence) !=
            combined::Result::kOk)
            return Fault(Result::kWriterReceiptRejected);
        return last_result_ = Result::kOk;
    }

    Result CommitWorld(std::uint32_t external_index,
                       const writer_protocol::Evidence& writer_evidence) {
        if (faulted_) return last_result_ = Result::kFaulted;
        if (!binding_.CanCommit(external_index) ||
            binding_.CommitFrame(external_index, writer_evidence) !=
                combined::Result::kOk)
            return Fault(Result::kCommitRejected);
        return last_result_ = Result::kOk;
    }

    bool complete() const { return !faulted_ && binding_.complete(); }
    bool faulted() const { return faulted_; }
    Result last_result() const { return last_result_; }
    std::uint32_t next_index() const { return binding_.next_index(); }
    const combined::Binding& binding() const { return binding_; }
    const host::Layout& mailbox_layout() const { return mailbox_layout_; }

 private:
    Result Fault(Result result) {
        faulted_ = true;
        return last_result_ = result;
    }

    combined::Binding binding_;
    host::Layout mailbox_layout_{};
    bool faulted_{};
    Result last_result_{Result::kOk};
};

}  // namespace a9tas::final_writer_natural_action_runtime_v1
