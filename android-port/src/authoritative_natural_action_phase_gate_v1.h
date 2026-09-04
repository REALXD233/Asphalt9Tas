#pragma once

// Transactional binding between the authoritative replay phases and the
// natural callback action mailbox.  This is a pure state machine: the caller
// owns the actual mailbox publication and all process/runtime operations.

#include "natural_action_host_gate_v1.h"

#include <cstdint>

namespace a9tas::authoritative_natural_action_phase_v1 {

namespace host = a9tas::natural_action_host_gate_v1;
namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;

enum class Phase : std::uint32_t {
    kIdle = 0,
    kPlannedBeforeC98 = 1,
    kPublishedAfterC98 = 2,
    kReceiptAcceptedAtCallbackClose = 3,
    kPoisoned = 4,
};

enum class Result : std::int32_t {
    kOk = 0,
    kInvalidState = -1,
    kHostGateFailure = -2,
    kReceiptMissing = -3,
    kPoisoned = -4,
};

struct State {
    host::State host_gate{};
    Phase phase{Phase::kIdle};
    std::uint32_t committed_frames{};
    std::uint32_t pre_c98_rollbacks{};
};

inline bool Initialize(State* state, std::uint32_t session_id,
                       std::uint32_t expected_producer_tid,
                       std::uint64_t vehicle_owner) {
    if (!state) return false;
    *state = {};
    return host::Initialize(&state->host_gate, session_id,
                            expected_producer_tid, vehicle_owner);
}

inline Result SelectAtDelta(State* state,
                            const recording::RecordingFrameV1& frame,
                            std::uint32_t replay_frame,
                            mailbox::Command* command) {
    if (!state || state->phase == Phase::kPoisoned) return Result::kPoisoned;
    if (state->phase != Phase::kIdle) {
        state->phase = Phase::kPoisoned;
        return Result::kInvalidState;
    }
    if (host::PlanFrame(&state->host_gate, frame, replay_frame, command) !=
        host::GateResult::kOk) {
        state->phase = Phase::kPoisoned;
        return Result::kHostGateFailure;
    }
    state->phase = Phase::kPlannedBeforeC98;
    return Result::kOk;
}

inline Result RollbackAtDeltaZeroBeforeC98(State* state) {
    if (!state || state->phase == Phase::kPoisoned) return Result::kPoisoned;
    if (state->phase != Phase::kPlannedBeforeC98 ||
        host::CancelBeforePublication(&state->host_gate) !=
            host::GateResult::kOk) {
        state->phase = Phase::kPoisoned;
        return Result::kInvalidState;
    }
    ++state->pre_c98_rollbacks;
    state->phase = Phase::kIdle;
    return Result::kOk;
}

// Call only after the external mailbox Publish succeeded at the certified C98
// control-commit boundary.
inline Result MarkPublishedAtC98(State* state,
                                 const mailbox::Command& command) {
    if (!state || state->phase == Phase::kPoisoned) return Result::kPoisoned;
    if (state->phase != Phase::kPlannedBeforeC98 ||
        host::MarkPublished(&state->host_gate, command) !=
            host::GateResult::kOk) {
        state->phase = Phase::kPoisoned;
        return Result::kInvalidState;
    }
    state->phase = Phase::kPublishedAfterC98;
    return Result::kOk;
}

// The dedicated natural consumer has returned before global callback-close.
// Missing/mismatched completion poisons the tick before world commit.
inline Result RequireReceiptAtCallbackClose(State* state,
                                            const mailbox::Mailbox& mailbox) {
    if (!state || state->phase == Phase::kPoisoned) return Result::kPoisoned;
    if (state->phase != Phase::kPublishedAfterC98) {
        state->phase = Phase::kPoisoned;
        return Result::kInvalidState;
    }
    if (host::AcceptReceipt(&state->host_gate, mailbox) !=
        host::GateResult::kOk) {
        state->phase = Phase::kPoisoned;
        return Result::kReceiptMissing;
    }
    state->phase = Phase::kReceiptAcceptedAtCallbackClose;
    return Result::kOk;
}

inline Result CommitAtWorld(State* state) {
    if (!state || state->phase == Phase::kPoisoned) return Result::kPoisoned;
    if (state->phase != Phase::kReceiptAcceptedAtCallbackClose) {
        state->phase = Phase::kPoisoned;
        return Result::kReceiptMissing;
    }
    ++state->committed_frames;
    state->phase = Phase::kIdle;
    return Result::kOk;
}

}  // namespace a9tas::authoritative_natural_action_phase_v1
