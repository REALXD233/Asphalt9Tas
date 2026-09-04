#include "natural_action_callback_mailbox_v1.h"

#include <cassert>
#include <cstdint>
#include <cstring>

namespace na = a9tas::natural_action_callback_v1;

namespace {

na::Command MakeCommand(std::uint64_t sequence, std::uint32_t session,
                        std::uint32_t tid, std::uint32_t count,
                        bool nitro_enabled = true) {
    na::Command command{};
    std::memcpy(command.magic, na::kCommandMagic, sizeof(na::kCommandMagic));
    command.version = na::kVersion;
    command.size = sizeof(command);
    command.sequence = sequence;
    command.session_id = session;
    command.replay_frame = static_cast<std::uint32_t>(sequence - 1);
    command.vehicle_owner = 0x10000000ULL;
    command.expected_producer_tid = tid;
    command.nitro_activations = count;
    command.flags = na::kReplayFrame |
                    (nitro_enabled ? na::kNitroOverrideEnabled : 0u);
    command.checksum = na::Checksum(command);
    return command;
}

struct Fixture {
    na::Mailbox mailbox{};
    na::RuntimeState state{};
    explicit Fixture(std::uint32_t session = 7) {
        na::Initialize(&mailbox);
        na::StoreRelease(&mailbox.session_control,
                         (static_cast<std::uint64_t>(session) << 1) |
                             na::kArmedBit);
    }
};

void TestExactZeroOneTwoAndIdleCallbacks() {
    Fixture fixture;
    for (std::uint32_t count = 0; count <= 2; ++count) {
        const std::uint64_t sequence = count + 1;
        const na::Command command = MakeCommand(sequence, 7, 4100, count);
        assert(na::Publish(&fixture.mailbox, command) ==
               na::Result::kPublished);
        na::Command selected{};
        assert(na::ClaimAtNaturalCallback(&fixture.mailbox, &fixture.state,
                                          4100, &selected) ==
               na::Result::kClaimed);
        assert(selected.nitro_activations == count);
        assert(na::CompleteNaturalCallback(&fixture.mailbox, &fixture.state,
                                           sequence, count, true) ==
               na::Result::kCompleted);
        assert(na::CompletionMatches(fixture.mailbox, sequence, count, count));
        assert(na::ClaimAtNaturalCallback(&fixture.mailbox, &fixture.state,
                                          4100, &selected) ==
               na::Result::kIdle);
    }
}

void TestSkipIsDifferentFromEnabledZero() {
    Fixture fixture(8);
    const na::Command skipped = MakeCommand(1, 8, 4200, 0, false);
    assert(na::CommandValid(skipped));
    na::Command invalid = MakeCommand(1, 8, 4200, 1, false);
    assert(!na::CommandValid(invalid));
    const na::Command enabled_zero = MakeCommand(1, 8, 4200, 0, true);
    assert(na::CommandValid(enabled_zero));
    assert(skipped.flags != enabled_zero.flags);
}

void TestFailClosedBoundaries() {
    {
        Fixture fixture(9);
        const na::Command command = MakeCommand(1, 9, 4300, 1);
        assert(na::Publish(&fixture.mailbox, command) ==
               na::Result::kPublished);
        na::Command selected{};
        assert(na::ClaimAtNaturalCallback(&fixture.mailbox, &fixture.state,
                                          4301, &selected) ==
               na::Result::kWrongProducerThread);
        assert(na::ClaimAtNaturalCallback(&fixture.mailbox, &fixture.state,
                                          4300, &selected) ==
               na::Result::kLatchedFault);
    }
    {
        Fixture fixture(10);
        const na::Command first = MakeCommand(1, 10, 4400, 1);
        assert(na::Publish(&fixture.mailbox, first) ==
               na::Result::kPublished);
        na::Command selected{};
        assert(na::ClaimAtNaturalCallback(&fixture.mailbox, &fixture.state,
                                          4400, &selected) ==
               na::Result::kClaimed);
        const na::Command second = MakeCommand(2, 10, 4400, 0);
        assert(na::Publish(&fixture.mailbox, second) ==
               na::Result::kPreviousFramePending);
        assert(na::CompleteNaturalCallback(&fixture.mailbox, &fixture.state,
                                           1, 0, true) ==
               na::Result::kCompletionMismatch);
    }
    {
        Fixture fixture(11);
        na::Command damaged = MakeCommand(1, 11, 4500, 2);
        damaged.vehicle_owner ^= 8;
        assert(na::Publish(&fixture.mailbox, damaged) ==
               na::Result::kInvalidCommand);
    }
}

}  // namespace

int main() {
    TestExactZeroOneTwoAndIdleCallbacks();
    TestSkipIsDifferentFromEnabledZero();
    TestFailClosedBoundaries();
    return 0;
}
