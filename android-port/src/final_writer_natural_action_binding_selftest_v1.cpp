#include "final_writer_natural_action_binding_v1.h"

#include <cstdio>

namespace {

namespace combined = a9tas::final_writer_natural_action_binding_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;
namespace writer_protocol = a9tas::final_writer_replay_v1;

recording::RecordingFrameV1 Frame(std::uint32_t index,
                                  std::uint32_t activations) {
    recording::RecordingFrameV1 frame{};
    frame.tick = index;
    frame.flags = recording::kRequiredFrameFlags;
    frame.nitro_activation_count = activations;
    return frame;
}

void AdvanceWriterEvidence(std::uint32_t processed,
                           std::uint32_t total,
                           writer_protocol::Evidence* evidence) {
    evidence->wrapper_entries = processed;
    evidence->original_calls = processed;
    evidence->clean_returns = processed;
    evidence->equal_frames = processed;
    evidence->processed_frames = processed;
    evidence->last_status = processed == total
                                ? writer_protocol::kStatusComplete
                                : writer_protocol::kStatusPassive;
}

bool Run() {
    constexpr std::uint32_t kFrames = 5;
    constexpr std::uint32_t kSession = 71;
    constexpr std::uint32_t kTid = 919;
    combined::Binding binding(kFrames, kSession, 0x45670000, kTid);
    mailbox::Mailbox box{};
    mailbox::Initialize(&box);
    mailbox::StoreRelease(&box.session_control,
                          (static_cast<std::uint64_t>(kSession) << 1) |
                              mailbox::kArmedBit);
    mailbox::RuntimeState consumer{};
    writer_protocol::Evidence writer_evidence{};
    writer_evidence.last_status = writer_protocol::kStatusPassive;
    const std::uint32_t counts[kFrames] = {0, 1, 0, 2, 1};
    for (std::uint32_t index = 0; index < kFrames; ++index) {
        mailbox::Command command{};
        if (binding.BeginFrame(index, Frame(index, counts[index]),
                               writer_evidence, &command) !=
                combined::Result::kOk ||
            mailbox::Publish(&box, command) != mailbox::Result::kPublished ||
            binding.ObserveAction(index, box) != combined::Result::kOk)
            return false;
        mailbox::Command claimed{};
        if (mailbox::ClaimAtNaturalCallback(&box, &consumer, kTid,
                                             &claimed) !=
                mailbox::Result::kClaimed)
            return false;
        AdvanceWriterEvidence(index + 1, kFrames, &writer_evidence);
        if (binding.AcknowledgeWriter(index, writer_evidence) !=
                combined::Result::kOk ||
            binding.CanCommit(index))
            return false;
        if (mailbox::CompleteNaturalCallback(&box, &consumer,
                                             command.sequence, counts[index],
                                             true) !=
                mailbox::Result::kCompleted ||
            binding.ObserveAction(index, box) != combined::Result::kOk ||
            !binding.CanCommit(index) ||
            binding.CommitFrame(index, writer_evidence) !=
                combined::Result::kOk)
            return false;
    }
    return binding.complete() && binding.next_index() == kFrames;
}

}  // namespace

int main() {
    const bool passed = Run();
    std::printf("FINAL_WRITER_NATURAL_ACTION_BINDING_SELFTEST passed=%u "
                "frames=5 action_before_commit=1 writer_bound=1 "
                "colour_state_forced=0 device_access=0\n",
                passed ? 1u : 0u);
    return passed ? 0 : 1;
}
