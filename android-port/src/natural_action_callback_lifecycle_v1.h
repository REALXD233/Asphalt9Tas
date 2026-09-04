#pragma once

// Pure host lifecycle contract for a persistent game-owned action callback.
// No target access or game call is present here.

#include "natural_action_callback_mailbox_v1.h"

#include <cstdint>

namespace a9tas::natural_action_callback_lifecycle_v1 {

namespace mailbox = a9tas::natural_action_callback_v1;

enum class Phase : std::uint32_t {
    kCold = 0,
    kRegistered = 1,
    kSessionArmed = 2,
    kRemovalPrepared = 3,
    kRemovalRequested = 4,
    kRemoved = 5,
    kFault = 6,
};

enum class Result : std::int32_t {
    kOk = 0,
    kInvalidState = -1,
    kRegistrationEvidenceMismatch = -2,
    kPendingFrame = -3,
    kMailboxMismatch = -4,
    kRemovalEvidenceMismatch = -5,
    kFault = -6,
};

struct State {
    Phase phase{Phase::kCold};
    std::uint32_t session_id{};
    std::uint32_t expected_producer_tid{};
    std::uint64_t vehicle_owner{};
    std::uint64_t registration_receipt{};
    std::uint64_t removal_receipt{};
};

inline bool Initialize(State* state, std::uint32_t session_id,
                       std::uint32_t expected_producer_tid,
                       std::uint64_t vehicle_owner) {
    if (!state || session_id == 0 || expected_producer_tid == 0 ||
        vehicle_owner == 0)
        return false;
    *state = {};
    state->session_id = session_id;
    state->expected_producer_tid = expected_producer_tid;
    state->vehicle_owner = vehicle_owner;
    return true;
}

inline Result AcceptRegistration(State* state, bool bootstrap_once,
                                 bool original_once, bool add_once,
                                 bool deferred_add, bool object_present_once,
                                 std::uint64_t receipt) {
    if (!state || state->phase != Phase::kCold || receipt == 0) {
        if (state) state->phase = Phase::kFault;
        return Result::kInvalidState;
    }
    if (!bootstrap_once || !original_once || !add_once || !deferred_add ||
        !object_present_once) {
        state->phase = Phase::kFault;
        return Result::kRegistrationEvidenceMismatch;
    }
    state->registration_receipt = receipt;
    state->phase = Phase::kRegistered;
    return Result::kOk;
}

inline Result ArmSession(State* state, mailbox::Mailbox* target) {
    if (!state || !target || state->phase != Phase::kRegistered ||
        !mailbox::MailboxValid(*target)) {
        if (state) state->phase = Phase::kFault;
        return Result::kInvalidState;
    }
    mailbox::StoreRelease(
        &target->session_control,
        (static_cast<std::uint64_t>(state->session_id) << 1) |
            mailbox::kArmedBit);
    state->phase = Phase::kSessionArmed;
    return Result::kOk;
}

inline Result PrepareCleanRemoval(State* state, mailbox::Mailbox* target,
                                  bool action_phase_idle,
                                  bool host_frame_pending,
                                  std::uint32_t* remove_requested) {
    if (!state || !target || !remove_requested ||
        state->phase != Phase::kSessionArmed ||
        !mailbox::MailboxValid(*target)) {
        if (state) state->phase = Phase::kFault;
        return Result::kInvalidState;
    }
    if (!action_phase_idle || host_frame_pending ||
        mailbox::LoadAcquire(&target->claimed_sequence) !=
            mailbox::LoadAcquire(&target->completed_sequence))
        return Result::kPendingFrame;
    mailbox::StoreRelease(
        &target->session_control,
        static_cast<std::uint64_t>(state->session_id) << 1);
    *remove_requested = 1;
    state->phase = Phase::kRemovalPrepared;
    return Result::kOk;
}

inline Result MarkRemovalRequested(State* state, bool callback_natural,
                                   bool remove_once, bool deferred_remove) {
    if (!state || state->phase != Phase::kRemovalPrepared) {
        if (state) state->phase = Phase::kFault;
        return Result::kInvalidState;
    }
    if (!callback_natural || !remove_once || !deferred_remove) {
        state->phase = Phase::kFault;
        return Result::kRemovalEvidenceMismatch;
    }
    state->phase = Phase::kRemovalRequested;
    return Result::kOk;
}

inline Result AcceptRemoval(State* state, bool object_absent,
                            bool callback_count_stable,
                            std::uint64_t receipt) {
    if (!state || state->phase != Phase::kRemovalRequested || receipt == 0) {
        if (state) state->phase = Phase::kFault;
        return Result::kInvalidState;
    }
    if (!object_absent || !callback_count_stable) {
        state->phase = Phase::kFault;
        return Result::kRemovalEvidenceMismatch;
    }
    state->removal_receipt = receipt;
    state->phase = Phase::kRemoved;
    return Result::kOk;
}

}  // namespace a9tas::natural_action_callback_lifecycle_v1
