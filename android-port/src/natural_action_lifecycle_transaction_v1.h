#pragma once

// Pure guarded transaction contract for one persistent, zero-call natural
// action lifecycle. Runtime process access is intentionally outside this file.

#include "authoritative_natural_action_phase_gate_v1.h"
#include "natural_action_callback_lifecycle_v1.h"

#include <cstdint>

namespace a9tas::natural_action_lifecycle_transaction_v1 {

namespace action = a9tas::authoritative_natural_action_phase_v1;
namespace lifecycle = a9tas::natural_action_callback_lifecycle_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;

enum class Phase : std::uint32_t {
    kCold = 0,
    kPreflightAccepted = 1,
    kReadyNoAttach = 2,
    kResumeObserved = 3,
    kShadowInstalled = 4,
    kRegistered = 5,
    kDetachedRegistered = 6,
    kArmed = 7,
    kFrameInFlight = 8,
    kRemovalPrepared = 9,
    kRemovalRequested = 10,
    kRemoved = 11,
    kClean = 12,
    kForceStopRequired = 13,
    kFault = 14,
};

enum class Result : std::int32_t {
    kOk = 0,
    kInvalidState = -1,
    kIdentityMismatch = -2,
    kStartlineMismatch = -3,
    kMutationMismatch = -4,
    kRegistrationMismatch = -5,
    kActionMismatch = -6,
    kRemovalMismatch = -7,
};

struct RegistrationProof {
    std::uint64_t bootstrap_entries{};
    std::uint64_t original_calls{};
    std::uint64_t original_returns{};
    std::uint64_t registration_attempts{};
    std::uint64_t registration_returns{};
    std::uint64_t failures{};
    std::uintptr_t list_end_before{};
    std::uintptr_t list_active_before{};
    std::uintptr_t list_end_after{};
    std::uintptr_t list_active_after{};
    std::uint32_t protocol_state{};
    std::int32_t last_status{};
    bool deferred_add{};
    bool object_present_once{};
    bool original_vptr_restored{};
};

struct RemovalProof {
    std::uint64_t removal_attempts{};
    std::uint64_t removal_returns{};
    std::uint64_t failures{};
    std::uint32_t protocol_state{};
    std::int32_t last_status{};
    bool callback_natural{};
    bool deferred_remove{};
};

struct FinalProof {
    bool same_process_generation{};
    bool process_alive{};
    bool tracer_clear{};
    bool original_vptr_final{};
    bool object_absent{};
    bool callback_count_stable{};
};

struct State {
    Phase phase{Phase::kCold};
    lifecycle::State lifecycle_state{};
    action::State action_state{};
    std::uint64_t process_generation{};
    std::uint64_t registration_receipt{};
    std::uint64_t removal_receipt{};
    std::uint32_t committed_zero_call_frames{};
    bool mutation_started{};
};

inline Result Reject(State* state, Result reason) {
    if (state) {
        state->phase = state->mutation_started ? Phase::kForceStopRequired
                                               : Phase::kFault;
    }
    return reason;
}

inline Result AcceptPreflight(State* state, std::uint64_t process_generation,
                              std::uint32_t session_id,
                              std::uint32_t expected_producer_tid,
                              std::uint64_t vehicle_owner,
                              bool game_build_identity,
                              bool payload_elf_identity,
                              bool fixed_environment,
                              bool process_alive, bool tracer_clear) {
    if (!state || state->phase != Phase::kCold || process_generation == 0 ||
        !lifecycle::Initialize(&state->lifecycle_state, session_id,
                               expected_producer_tid, vehicle_owner))
        return Reject(state, Result::kInvalidState);
    if (!game_build_identity || !payload_elf_identity || !fixed_environment ||
        !process_alive || !tracer_clear)
        return Reject(state, Result::kIdentityMismatch);
    state->process_generation = process_generation;
    state->phase = Phase::kPreflightAccepted;
    return Result::kOk;
}

inline Result AcceptReadyNoAttach(State* state, bool payload_storage_clean,
                                  bool control_prepared,
                                  bool shadow_prepared,
                                  bool host_tracer_clear,
                                  bool guest_tracer_clear) {
    if (!state || state->phase != Phase::kPreflightAccepted)
        return Reject(state, Result::kInvalidState);
    if (!payload_storage_clean || !control_prepared || !shadow_prepared ||
        !host_tracer_clear || !guest_tracer_clear)
        return Reject(state, Result::kStartlineMismatch);
    state->phase = Phase::kReadyNoAttach;
    return Result::kOk;
}

inline Result AcceptSingleResumeEdge(State* state,
                                     std::uint64_t process_generation,
                                     std::uint32_t esc_count,
                                     bool first_positive_delta) {
    if (!state || state->phase != Phase::kReadyNoAttach)
        return Reject(state, Result::kInvalidState);
    if (process_generation != state->process_generation || esc_count != 1 ||
        !first_positive_delta)
        return Reject(state, Result::kStartlineMismatch);
    state->phase = Phase::kResumeObserved;
    return Result::kOk;
}

inline Result AcceptShadowInstall(State* state, bool exact_owner_thread,
                                  bool other_threads_frozen,
                                  std::uint32_t game_write_count,
                                  bool exact_vptr_exchange) {
    if (!state || state->phase != Phase::kResumeObserved)
        return Reject(state, Result::kInvalidState);
    if (!exact_owner_thread || !other_threads_frozen ||
        game_write_count != 1 || !exact_vptr_exchange)
        return Reject(state, Result::kMutationMismatch);
    state->mutation_started = true;
    state->phase = Phase::kShadowInstalled;
    return Result::kOk;
}

inline Result AcceptRegistration(State* state, const RegistrationProof& proof,
                                 std::uint64_t receipt) {
    if (!state || state->phase != Phase::kShadowInstalled || receipt == 0)
        return Reject(state, Result::kInvalidState);
    const bool exact =
        proof.bootstrap_entries == 1 && proof.original_calls == 1 &&
        proof.original_returns == 1 && proof.registration_attempts == 1 &&
        proof.registration_returns == 1 && proof.failures == 0 &&
        proof.list_end_before != 0 &&
        proof.list_end_after == proof.list_end_before + 16u &&
        proof.list_active_after == proof.list_active_before &&
        proof.protocol_state == 1 && proof.last_status == 1 &&
        proof.deferred_add && proof.object_present_once &&
        proof.original_vptr_restored;
    if (!exact || lifecycle::AcceptRegistration(
                      &state->lifecycle_state, true, true, true, true, true,
                      receipt) != lifecycle::Result::kOk)
        return Reject(state, Result::kRegistrationMismatch);
    state->registration_receipt = receipt;
    state->phase = Phase::kRegistered;
    return Result::kOk;
}

inline Result AcceptRegisteredDetach(State* state,
                                     std::uint64_t process_generation,
                                     bool process_alive, bool tracer_clear,
                                     bool original_vptr_final) {
    if (!state || state->phase != Phase::kRegistered)
        return Reject(state, Result::kInvalidState);
    if (process_generation != state->process_generation || !process_alive ||
        !tracer_clear || !original_vptr_final)
        return Reject(state, Result::kRegistrationMismatch);
    state->phase = Phase::kDetachedRegistered;
    return Result::kOk;
}

inline Result ArmZeroCallSession(State* state, mailbox::Mailbox* target) {
    if (!state || state->phase != Phase::kDetachedRegistered || !target ||
        lifecycle::ArmSession(&state->lifecycle_state, target) !=
            lifecycle::Result::kOk ||
        !action::Initialize(
            &state->action_state, state->lifecycle_state.session_id,
            state->lifecycle_state.expected_producer_tid,
            state->lifecycle_state.vehicle_owner))
        return Reject(state, Result::kActionMismatch);
    state->phase = Phase::kArmed;
    return Result::kOk;
}

inline Result SelectZeroCallAtDelta(State* state,
                                    const recording::RecordingFrameV1& frame,
                                    std::uint32_t replay_frame,
                                    mailbox::Command* command) {
    if (!state || state->phase != Phase::kArmed || !command)
        return Reject(state, Result::kInvalidState);
    if (action::SelectAtDelta(&state->action_state, frame, replay_frame,
                              command) != action::Result::kOk ||
        command->nitro_activations != 0)
        return Reject(state, Result::kActionMismatch);
    state->phase = Phase::kFrameInFlight;
    return Result::kOk;
}

inline Result CancelBeforeC98(State* state) {
    if (!state || state->phase != Phase::kFrameInFlight ||
        action::RollbackAtDeltaZeroBeforeC98(&state->action_state) !=
            action::Result::kOk)
        return Reject(state, Result::kActionMismatch);
    state->phase = Phase::kArmed;
    return Result::kOk;
}

inline Result MarkPublishedAtC98(State* state,
                                 const mailbox::Command& command) {
    if (!state || state->phase != Phase::kFrameInFlight ||
        command.nitro_activations != 0 ||
        action::MarkPublishedAtC98(&state->action_state, command) !=
            action::Result::kOk)
        return Reject(state, Result::kActionMismatch);
    return Result::kOk;
}

inline Result AcceptZeroReceiptAndCommit(State* state,
                                         const mailbox::Mailbox& target) {
    if (!state || state->phase != Phase::kFrameInFlight ||
        action::RequireReceiptAtCallbackClose(&state->action_state, target) !=
            action::Result::kOk ||
        action::CommitAtWorld(&state->action_state) != action::Result::kOk)
        return Reject(state, Result::kActionMismatch);
    ++state->committed_zero_call_frames;
    state->phase = Phase::kArmed;
    return Result::kOk;
}

inline Result PrepareCleanRemoval(State* state, mailbox::Mailbox* target,
                                  std::uint32_t* remove_requested) {
    if (!state || state->phase != Phase::kArmed ||
        state->action_state.phase != action::Phase::kIdle ||
        state->action_state.host_gate.pending ||
        lifecycle::PrepareCleanRemoval(
            &state->lifecycle_state, target, true, false,
            remove_requested) != lifecycle::Result::kOk)
        return Reject(state, Result::kRemovalMismatch);
    state->phase = Phase::kRemovalPrepared;
    return Result::kOk;
}

inline Result AcceptRemovalRequest(State* state, const RemovalProof& proof) {
    if (!state || state->phase != Phase::kRemovalPrepared)
        return Reject(state, Result::kInvalidState);
    const bool exact = proof.removal_attempts == 1 &&
                       proof.removal_returns == 1 && proof.failures == 0 &&
                       proof.protocol_state == 3 && proof.last_status == 3 &&
                       proof.callback_natural && proof.deferred_remove;
    if (!exact || lifecycle::MarkRemovalRequested(
                      &state->lifecycle_state, true, true, true) !=
                      lifecycle::Result::kOk)
        return Reject(state, Result::kRemovalMismatch);
    state->phase = Phase::kRemovalRequested;
    return Result::kOk;
}

inline Result AcceptFinalRemoval(State* state, const FinalProof& proof,
                                 std::uint64_t receipt) {
    if (!state || state->phase != Phase::kRemovalRequested || receipt == 0)
        return Reject(state, Result::kInvalidState);
    if (!proof.same_process_generation || !proof.process_alive ||
        !proof.tracer_clear || !proof.original_vptr_final ||
        !proof.object_absent || !proof.callback_count_stable ||
        lifecycle::AcceptRemoval(&state->lifecycle_state, true, true,
                                 receipt) != lifecycle::Result::kOk)
        return Reject(state, Result::kRemovalMismatch);
    state->removal_receipt = receipt;
    state->phase = Phase::kRemoved;
    return Result::kOk;
}

inline Result AcceptCleanEnd(State* state, bool report_durable,
                             bool no_pending_writes) {
    if (!state || state->phase != Phase::kRemoved || !report_durable ||
        !no_pending_writes)
        return Reject(state, Result::kRemovalMismatch);
    state->phase = Phase::kClean;
    return Result::kOk;
}

}  // namespace a9tas::natural_action_lifecycle_transaction_v1
