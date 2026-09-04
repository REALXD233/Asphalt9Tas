#include "natural_action_lifecycle_transaction_v1.h"

#include <cassert>
#include <cstring>

namespace tx = a9tas::natural_action_lifecycle_transaction_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;

static tx::State StartRegistered(mailbox::Mailbox* box) {
    tx::State state{};
    assert(tx::AcceptPreflight(&state, 9, 7, 123, 0xABC, true, true, true,
                               true, true) == tx::Result::kOk);
    assert(tx::AcceptReadyNoAttach(&state, true, true, true, true, true) ==
           tx::Result::kOk);
    assert(tx::AcceptSingleResumeEdge(&state, 9, 1, true) ==
           tx::Result::kOk);
    assert(tx::AcceptShadowInstall(&state, true, true, 1, true) ==
           tx::Result::kOk);
    tx::RegistrationProof registration{};
    registration.bootstrap_entries = 1;
    registration.original_calls = 1;
    registration.original_returns = 1;
    registration.registration_attempts = 1;
    registration.registration_returns = 1;
    registration.list_end_before = 0x1000;
    registration.list_active_before = 0x1000;
    registration.list_end_after = 0x1010;
    registration.list_active_after = 0x1000;
    registration.protocol_state = 1;
    registration.last_status = 1;
    registration.deferred_add = true;
    registration.object_present_once = true;
    registration.original_vptr_restored = true;
    assert(tx::AcceptRegistration(&state, registration, 10) ==
           tx::Result::kOk);
    assert(tx::AcceptRegisteredDetach(&state, 9, true, true, true) ==
           tx::Result::kOk);
    mailbox::Initialize(box);
    assert(tx::ArmZeroCallSession(&state, box) == tx::Result::kOk);
    return state;
}

int main() {
    mailbox::Mailbox box{};
    tx::State state = StartRegistered(&box);
    recording::RecordingFrameV1 frame{};
    frame.flags = recording::kRequiredFrameFlags;
    frame.skip_override_flags = recording::kSkipNitroActivation;
    mailbox::Command command{};
    assert(tx::SelectZeroCallAtDelta(&state, frame, 0, &command) ==
           tx::Result::kOk);
    assert(mailbox::Publish(&box, command) == mailbox::Result::kPublished);
    assert(tx::MarkPublishedAtC98(&state, command) == tx::Result::kOk);
    mailbox::RuntimeState runtime{};
    mailbox::Command claimed{};
    assert(mailbox::ClaimAtNaturalCallback(&box, &runtime, 123, &claimed) ==
           mailbox::Result::kClaimed);
    assert(mailbox::CompleteNaturalCallback(&box, &runtime, 1, 0, true) ==
           mailbox::Result::kCompleted);
    assert(tx::AcceptZeroReceiptAndCommit(&state, box) == tx::Result::kOk);
    assert(state.committed_zero_call_frames == 1);
    std::uint32_t remove_requested = 0;
    assert(tx::PrepareCleanRemoval(&state, &box, &remove_requested) ==
           tx::Result::kOk);
    assert(remove_requested == 1);
    tx::RemovalProof removal{1, 1, 0, 3, 3, true, true};
    assert(tx::AcceptRemovalRequest(&state, removal) == tx::Result::kOk);
    tx::FinalProof final{true, true, true, true, true, true};
    assert(tx::AcceptFinalRemoval(&state, final, 11) == tx::Result::kOk);
    assert(tx::AcceptCleanEnd(&state, true, true) == tx::Result::kOk);
    assert(state.phase == tx::Phase::kClean);

    tx::State preflight_failure{};
    assert(tx::AcceptPreflight(&preflight_failure, 1, 1, 1, 1, false, true,
                               true, true, true) ==
           tx::Result::kIdentityMismatch);
    assert(preflight_failure.phase == tx::Phase::kFault);

    mailbox::Mailbox second_box{};
    tx::State post_mutation_failure = StartRegistered(&second_box);
    recording::RecordingFrameV1 nonzero = frame;
    nonzero.skip_override_flags = 0;
    nonzero.nitro_activation_count = 1;
    assert(tx::SelectZeroCallAtDelta(&post_mutation_failure, nonzero, 0,
                                     &command) == tx::Result::kActionMismatch);
    assert(post_mutation_failure.phase == tx::Phase::kForceStopRequired);
    return 0;
}
