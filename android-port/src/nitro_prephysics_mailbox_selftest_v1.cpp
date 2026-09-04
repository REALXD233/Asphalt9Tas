#include "nitro_prephysics_mailbox_v1.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace np = a9tas::nitro_prephysics_v1;

namespace {

struct Fixture {
    np::Mailbox mailbox{};
    np::RuntimeState state{};

    Fixture() { np::InitializeMailbox(&mailbox); }

    void Arm(std::uint32_t session_id) {
        np::StoreRelease(&mailbox.session_control,
                         (static_cast<std::uint64_t>(session_id) << 1) |
                             np::kSessionArmedBit);
    }

    void Publish(std::uint64_t sequence, std::uint32_t session_id,
                 std::uint32_t frame, std::uint32_t tid,
                 std::uint32_t activations,
                 std::uint64_t owner = 0x10000000ULL) {
        np::Command command{};
        std::memcpy(command.magic, np::kCommandMagic,
                    sizeof(np::kCommandMagic));
        command.version = np::kVersion;
        command.size = sizeof(command);
        command.sequence = sequence;
        command.session_id = session_id;
        command.replay_frame = frame;
        command.vehicle_owner = owner;
        command.expected_frame_tid = tid;
        command.activations = activations;
        command.flags = np::kFlagReplayFrame;
        command.checksum = np::Checksum(command);
        const std::uint32_t slot = static_cast<std::uint32_t>(sequence & 1u);
        mailbox.slots[slot] = command;
        np::StoreRelease(&mailbox.published_selector,
                         (sequence << 1) | slot);
    }
};

void TestPassiveDefault() {
    Fixture fixture;
    assert(np::StageAtSubmit(&fixture.mailbox, &fixture.state) ==
           np::Result::kPassive);
    np::Command command{};
    assert(np::ClaimAtExecute(&fixture.mailbox, &fixture.state, 42, &command) ==
           np::Result::kIdle);
}

void TestExactCountsAndExactlyOnce() {
    Fixture fixture;
    fixture.Arm(7);
    for (std::uint32_t count = 0; count <= np::kMaxActivations; ++count) {
        const std::uint64_t sequence = count + 1;
        fixture.Publish(sequence, 7, count, 4100, count);
        assert(np::StageAtSubmit(&fixture.mailbox, &fixture.state) ==
               np::Result::kStaged);
        np::Command command{};
        assert(np::ClaimAtExecute(&fixture.mailbox, &fixture.state, 4100,
                                  &command) == np::Result::kClaimed);
        assert(command.sequence == sequence);
        assert(command.activations == count);
        np::Command second{};
        assert(np::ClaimAtExecute(&fixture.mailbox, &fixture.state, 4100,
                                  &second) == np::Result::kExecuteReentry);
        // Re-entry deliberately latches a fault, so complete this case on a
        // fresh fixture below.  The main loop tests normal completion without
        // intentionally racing the claimed command.
        break;
    }

    for (std::uint32_t count = 0; count <= np::kMaxActivations; ++count) {
        Fixture clean;
        clean.Arm(8);
        clean.Publish(count + 1, 8, count, 4200, count);
        assert(np::StageAtSubmit(&clean.mailbox, &clean.state) ==
               np::Result::kStaged);
        np::Command command{};
        assert(np::ClaimAtExecute(&clean.mailbox, &clean.state, 4200,
                                  &command) == np::Result::kClaimed);
        assert(np::CompleteExecution(&clean.mailbox, &clean.state,
                                     command.sequence, count, true) ==
               np::Result::kCompleted);
        assert(clean.mailbox.completed_sequence == command.sequence);
        assert(clean.mailbox.calls_completed == count);
        np::Command after{};
        assert(np::ClaimAtExecute(&clean.mailbox, &clean.state, 4200, &after) ==
               np::Result::kIdle);
    }
}

void TestStrictFailures() {
    {
        Fixture fixture;
        fixture.Arm(1);
        assert(np::StageAtSubmit(&fixture.mailbox, &fixture.state) ==
               np::Result::kNoPublishedCommand);
        assert((fixture.mailbox.fault_flags & np::kFaultMissingFrame) != 0);
    }
    {
        Fixture fixture;
        fixture.Arm(2);
        fixture.Publish(1, 3, 0, 5000, 1);
        assert(np::StageAtSubmit(&fixture.mailbox, &fixture.state) ==
               np::Result::kWrongSession);
    }
    {
        Fixture fixture;
        fixture.Arm(4);
        fixture.Publish(1, 4, 0, 5001, 1);
        const std::uint32_t slot = 1;
        fixture.mailbox.slots[slot].vehicle_owner ^= 8;
        assert(np::StageAtSubmit(&fixture.mailbox, &fixture.state) ==
               np::Result::kInvalidCommand);
    }
    {
        Fixture fixture;
        fixture.Arm(5);
        fixture.Publish(1, 5, 0, 5002, 1);
        assert(np::StageAtSubmit(&fixture.mailbox, &fixture.state) ==
               np::Result::kStaged);
        np::Command command{};
        assert(np::ClaimAtExecute(&fixture.mailbox, &fixture.state, 9999,
                                  &command) == np::Result::kWrongThread);
        assert(fixture.mailbox.calls_completed == 0);
        assert((fixture.mailbox.fault_flags & np::kFaultWrongThread) != 0);
    }
    {
        Fixture fixture;
        fixture.Arm(6);
        fixture.Publish(1, 6, 0, 5003, 1);
        assert(np::StageAtSubmit(&fixture.mailbox, &fixture.state) ==
               np::Result::kStaged);
        fixture.Publish(2, 6, 1, 5003, 1);
        assert(np::StageAtSubmit(&fixture.mailbox, &fixture.state) ==
               np::Result::kPreviousTokenPending);
    }
}

}  // namespace

int main() {
    TestPassiveDefault();
    TestExactCountsAndExactlyOnce();
    TestStrictFailures();
    return 0;
}
