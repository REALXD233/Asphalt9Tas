#include "natural_action_replay_cursor_v1.h"

#include <cstdio>

namespace {

namespace cursor = a9tas::natural_action_replay_cursor_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;

recording::RecordingFrameV1 Frame(std::uint32_t index,
                                  std::uint32_t activations,
                                  std::uint32_t skip = 0) {
    recording::RecordingFrameV1 frame{};
    frame.tick = index;
    frame.flags = recording::kRequiredFrameFlags;
    frame.nitro_activation_count = activations;
    frame.skip_override_flags = skip;
    return frame;
}

bool Run() {
    constexpr std::uint32_t kSession = 17;
    cursor::Binding binding(5, kSession, 0x12340000, 222);
    mailbox::Mailbox box{};
    mailbox::Initialize(&box);
    mailbox::StoreRelease(&box.session_control,
                          (static_cast<std::uint64_t>(kSession) << 1) |
                              mailbox::kArmedBit);
    mailbox::RuntimeState consumer{};
    const std::uint32_t counts[5] = {0, 1, 0, 2, 1};
    for (std::uint32_t index = 0; index < 5; ++index) {
        mailbox::Command command{};
        if (binding.PrepareFrame(index, Frame(index, counts[index]),
                                 &command) != cursor::Result::kOk ||
            mailbox::Publish(&box, command) != mailbox::Result::kPublished ||
            binding.ObserveMailbox(index, box) != cursor::Result::kOk ||
            binding.phase() != cursor::Phase::kPublished)
            return false;
        mailbox::Command claimed{};
        if (mailbox::ClaimAtNaturalCallback(&box, &consumer, 222, &claimed) !=
                mailbox::Result::kClaimed ||
            claimed.nitro_activations != counts[index])
            return false;
        // Exercise both legal receipt orders.  The frame is never committable
        // until the action and writer receipts are both present.
        if ((index & 1u) == 0) {
            if (binding.AcknowledgeWriter(index, index + 1) !=
                    cursor::Result::kOk ||
                binding.CanCommit(index))
                return false;
        }
        if (mailbox::CompleteNaturalCallback(&box, &consumer,
                                             command.sequence, counts[index],
                                             true) !=
                mailbox::Result::kCompleted ||
            binding.ObserveMailbox(index, box) != cursor::Result::kOk)
            return false;
        if ((index & 1u) != 0 &&
            binding.AcknowledgeWriter(index, index + 1) != cursor::Result::kOk)
            return false;
        if (!binding.CanCommit(index) ||
            binding.CommitFrame(index) != cursor::Result::kOk)
            return false;
    }
    return binding.next_index() == 5 &&
           binding.phase() == cursor::Phase::kComplete;
}

}  // namespace

int main() {
    const bool passed = Run();
    std::printf("NATURAL_ACTION_REPLAY_CURSOR_SELFTEST passed=%u "
                "frames=5 counts_0_1_2=1 exact_cursor=1 dual_receipt=1 "
                "device_access=0\n",
                passed ? 1u : 0u);
    return passed ? 0 : 1;
}
