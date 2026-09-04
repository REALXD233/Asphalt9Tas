#include "natural_action_host_gate_v1.h"
#include "authoritative_natural_action_phase_gate_v1.h"
#include "natural_action_callback_lifecycle_v1.h"

#include <cassert>
#include <cstdint>

namespace gate = a9tas::natural_action_host_gate_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;
namespace phase = a9tas::authoritative_natural_action_phase_v1;
namespace lifecycle = a9tas::natural_action_callback_lifecycle_v1;

namespace {

recording::RecordingFrameV1 Frame(std::uint32_t count, bool skip) {
    recording::RecordingFrameV1 frame{};
    frame.nitro_activation_count = count;
    frame.skip_override_flags =
        skip ? recording::kSkipNitroActivation : 0;
    frame.flags = recording::kRequiredFrameFlags;
    return frame;
}

void Arm(mailbox::Mailbox* target, std::uint32_t session) {
    mailbox::Initialize(target);
    mailbox::StoreRelease(&target->session_control,
                          (static_cast<std::uint64_t>(session) << 1) |
                              mailbox::kArmedBit);
}

void TestSkipAndZeroOneTwoMapping() {
    gate::State state{};
    assert(gate::Initialize(&state, 7, 4100, 0x10000000ULL));
    mailbox::Mailbox target{};
    mailbox::RuntimeState runtime{};
    Arm(&target, 7);
    const recording::RecordingFrameV1 inputs[] = {
        Frame(2, true), Frame(0, false), Frame(1, false), Frame(2, false)};
    const std::uint32_t expected[] = {0, 0, 1, 2};
    const bool enabled[] = {false, true, true, true};
    for (std::uint32_t index = 0; index < 4; ++index) {
        mailbox::Command command{};
        assert(gate::PlanFrame(&state, inputs[index], index, &command) ==
               gate::GateResult::kOk);
        assert(command.nitro_activations == expected[index]);
        assert(((command.flags & mailbox::kNitroOverrideEnabled) != 0) ==
               enabled[index]);
        assert(mailbox::Publish(&target, command) ==
               mailbox::Result::kPublished);
        assert(gate::MarkPublished(&state, command) == gate::GateResult::kOk);
        mailbox::Command selected{};
        assert(mailbox::ClaimAtNaturalCallback(&target, &runtime, 4100,
                                               &selected) ==
               mailbox::Result::kClaimed);
        assert(mailbox::CompleteNaturalCallback(
                   &target, &runtime, selected.sequence,
                   selected.nitro_activations, true) ==
               mailbox::Result::kCompleted);
        assert(gate::AcceptReceipt(&state, target) == gate::GateResult::kOk);
    }
}

void TestPendingAndReceiptMismatch() {
    gate::State state{};
    assert(gate::Initialize(&state, 8, 4200, 0x20000000ULL));
    mailbox::Command command{};
    assert(gate::PlanFrame(&state, Frame(1, false), 0, &command) ==
           gate::GateResult::kOk);
    mailbox::Command duplicate{};
    assert(gate::PlanFrame(&state, Frame(0, false), 1, &duplicate) ==
           gate::GateResult::kPreviousFramePending);
    mailbox::Mailbox target{};
    Arm(&target, 8);
    assert(gate::AcceptReceipt(&state, target) ==
           gate::GateResult::kInvalidConfiguration);
    assert(mailbox::Publish(&target, command) == mailbox::Result::kPublished);
    assert(gate::MarkPublished(&state, command) == gate::GateResult::kOk);
    assert(gate::AcceptReceipt(&state, target) ==
           gate::GateResult::kReceiptMismatch);
    assert(state.pending && state.published && state.next_sequence == 1 &&
           state.next_frame == 0);
}

void TestPreC98CancellationReusesSequence() {
    gate::State state{};
    assert(gate::Initialize(&state, 9, 4300, 0x30000000ULL));
    mailbox::Command first{};
    assert(gate::PlanFrame(&state, Frame(2, false), 0, &first) ==
           gate::GateResult::kOk);
    assert(gate::CancelBeforePublication(&state) == gate::GateResult::kOk);
    assert(!state.pending && !state.published && state.next_sequence == 1 &&
           state.next_frame == 0);
    mailbox::Command retry{};
    assert(gate::PlanFrame(&state, Frame(1, false), 0, &retry) ==
           gate::GateResult::kOk);
    assert(retry.sequence == 1 && retry.replay_frame == 0 &&
           retry.nitro_activations == 1);
}

void TestAuthoritativePhaseBinding() {
    phase::State state{};
    assert(phase::Initialize(&state, 10, 4400, 0x40000000ULL));
    mailbox::Mailbox target{};
    mailbox::RuntimeState runtime{};
    Arm(&target, 10);
    mailbox::Command command{};
    assert(phase::SelectAtDelta(&state, Frame(0, false), 0, &command) ==
           phase::Result::kOk);
    assert(mailbox::Publish(&target, command) == mailbox::Result::kPublished);
    assert(phase::MarkPublishedAtC98(&state, command) == phase::Result::kOk);
    mailbox::Command selected{};
    assert(mailbox::ClaimAtNaturalCallback(&target, &runtime, 4400,
                                           &selected) ==
           mailbox::Result::kClaimed);
    assert(mailbox::CompleteNaturalCallback(&target, &runtime, 1, 0, true) ==
           mailbox::Result::kCompleted);
    assert(phase::RequireReceiptAtCallbackClose(&state, target) ==
           phase::Result::kOk);
    assert(phase::CommitAtWorld(&state) == phase::Result::kOk);
    assert(state.committed_frames == 1 &&
           state.phase == phase::Phase::kIdle);
}

void TestAuthoritativeRollbackAndMissingReceiptPoison() {
    phase::State rollback{};
    assert(phase::Initialize(&rollback, 11, 4500, 0x50000000ULL));
    mailbox::Command command{};
    assert(phase::SelectAtDelta(&rollback, Frame(2, false), 0, &command) ==
           phase::Result::kOk);
    assert(phase::RollbackAtDeltaZeroBeforeC98(&rollback) ==
           phase::Result::kOk);
    assert(rollback.pre_c98_rollbacks == 1 &&
           rollback.phase == phase::Phase::kIdle);

    phase::State missing{};
    assert(phase::Initialize(&missing, 12, 4600, 0x60000000ULL));
    mailbox::Mailbox target{};
    Arm(&target, 12);
    assert(phase::SelectAtDelta(&missing, Frame(0, false), 0, &command) ==
           phase::Result::kOk);
    assert(mailbox::Publish(&target, command) == mailbox::Result::kPublished);
    assert(phase::MarkPublishedAtC98(&missing, command) == phase::Result::kOk);
    assert(phase::RequireReceiptAtCallbackClose(&missing, target) ==
           phase::Result::kReceiptMissing);
    assert(missing.phase == phase::Phase::kPoisoned);
    assert(phase::CommitAtWorld(&missing) == phase::Result::kPoisoned);
}

void TestPersistentLifecycleCleanRemoval() {
    lifecycle::State state{};
    assert(lifecycle::Initialize(&state, 13, 4700, 0x70000000ULL));
    assert(lifecycle::AcceptRegistration(&state, true, true, true, true,
                                         true, 0x1111) ==
           lifecycle::Result::kOk);
    mailbox::Mailbox target{};
    Arm(&target, 13);
    assert(lifecycle::ArmSession(&state, &target) == lifecycle::Result::kOk);
    std::uint32_t remove_requested = 0;
    mailbox::StoreRelease(&target.claimed_sequence, 1);
    assert(lifecycle::PrepareCleanRemoval(&state, &target, true, false,
                                          &remove_requested) ==
           lifecycle::Result::kPendingFrame);
    mailbox::StoreRelease(&target.completed_sequence, 1);
    assert(lifecycle::PrepareCleanRemoval(&state, &target, true, false,
                                          &remove_requested) ==
           lifecycle::Result::kOk);
    assert(remove_requested == 1 &&
           !mailbox::Armed(mailbox::LoadAcquire(&target.session_control)));
    assert(lifecycle::MarkRemovalRequested(&state, true, true, true) ==
           lifecycle::Result::kOk);
    assert(lifecycle::AcceptRemoval(&state, true, true, 0x2222) ==
           lifecycle::Result::kOk);
    assert(state.phase == lifecycle::Phase::kRemoved &&
           state.registration_receipt == 0x1111 &&
           state.removal_receipt == 0x2222);
}

void TestPersistentLifecycleRejectsAmbiguousEvidence() {
    lifecycle::State state{};
    assert(lifecycle::Initialize(&state, 14, 4800, 0x80000000ULL));
    assert(lifecycle::AcceptRegistration(&state, true, true, true, false,
                                         true, 1) ==
           lifecycle::Result::kRegistrationEvidenceMismatch);
    assert(state.phase == lifecycle::Phase::kFault);
}

}  // namespace

int main() {
    TestSkipAndZeroOneTwoMapping();
    TestPendingAndReceiptMismatch();
    TestPreC98CancellationReusesSequence();
    TestAuthoritativePhaseBinding();
    TestAuthoritativeRollbackAndMissingReceiptPoison();
    TestPersistentLifecycleCleanRemoval();
    TestPersistentLifecycleRejectsAmbiguousEvidence();
    return 0;
}
