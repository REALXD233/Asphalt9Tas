#pragma once

// Pure host-side planner/receipt gate for the natural callback action mailbox.
// It performs no target-memory access.  A separately reviewed runtime writer
// must copy the planned command into the payload-owned mailbox using the
// mailbox publication order.

#include "natural_action_callback_mailbox_v1.h"
#include "unified_tick_recording_v1.h"

#include <cstdint>
#include <cstring>

namespace a9tas::natural_action_host_gate_v1 {

namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;

enum class GateResult : std::int32_t {
    kOk = 0,
    kInvalidConfiguration = -1,
    kInvalidFrame = -2,
    kPreviousFramePending = -3,
    kSequenceMismatch = -4,
    kReceiptMismatch = -5,
};

struct State {
    std::uint32_t session_id{};
    std::uint32_t expected_producer_tid{};
    std::uint64_t vehicle_owner{};
    std::uint64_t next_sequence{1};
    std::uint32_t next_frame{};
    bool pending{};
    bool published{};
    mailbox::Command pending_command{};
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
    state->next_sequence = 1;
    return true;
}

inline GateResult PlanFrame(State* state,
                            const recording::RecordingFrameV1& frame,
                            std::uint32_t replay_frame,
                            mailbox::Command* output) {
    if (!state || !output || state->session_id == 0 ||
        state->expected_producer_tid == 0 || state->vehicle_owner == 0)
        return GateResult::kInvalidConfiguration;
    if (state->pending) return GateResult::kPreviousFramePending;
    if (replay_frame != state->next_frame ||
        state->next_sequence != static_cast<std::uint64_t>(replay_frame) + 1)
        return GateResult::kSequenceMismatch;
    if (frame.flags != recording::kRequiredFrameFlags ||
        frame.nitro_activation_count > mailbox::kMaximumNitroActivations)
        return GateResult::kInvalidFrame;

    const bool nitro_enabled =
        (frame.skip_override_flags & recording::kSkipNitroActivation) == 0;
    mailbox::Command command{};
    std::memcpy(command.magic, mailbox::kCommandMagic,
                sizeof(mailbox::kCommandMagic));
    command.version = mailbox::kVersion;
    command.size = sizeof(command);
    command.sequence = state->next_sequence;
    command.session_id = state->session_id;
    command.replay_frame = replay_frame;
    command.vehicle_owner = state->vehicle_owner;
    command.expected_producer_tid = state->expected_producer_tid;
    command.nitro_activations =
        nitro_enabled ? frame.nitro_activation_count : 0;
    command.flags = mailbox::kReplayFrame |
                    (nitro_enabled ? mailbox::kNitroOverrideEnabled : 0u);
    command.checksum = mailbox::Checksum(command);
    if (!mailbox::CommandValid(command)) return GateResult::kInvalidFrame;
    state->pending = true;
    state->published = false;
    state->pending_command = command;
    *output = command;
    return GateResult::kOk;
}

inline GateResult CancelBeforePublication(State* state) {
    if (!state || !state->pending || state->published)
        return GateResult::kInvalidConfiguration;
    state->pending = false;
    state->pending_command = {};
    return GateResult::kOk;
}

inline GateResult MarkPublished(State* state,
                                const mailbox::Command& command) {
    if (!state || !state->pending || state->published)
        return GateResult::kInvalidConfiguration;
    if (std::memcmp(&state->pending_command, &command, sizeof(command)) != 0)
        return GateResult::kSequenceMismatch;
    state->published = true;
    return GateResult::kOk;
}

inline GateResult AcceptReceipt(State* state, const mailbox::Mailbox& mailbox) {
    if (!state || !state->pending || !state->published)
        return GateResult::kInvalidConfiguration;
    const auto& command = state->pending_command;
    if (!mailbox::CompletionMatches(mailbox, command.sequence,
                                    command.replay_frame,
                                    command.nitro_activations))
        return GateResult::kReceiptMismatch;
    state->pending = false;
    state->published = false;
    ++state->next_sequence;
    ++state->next_frame;
    state->pending_command = {};
    return GateResult::kOk;
}

}  // namespace a9tas::natural_action_host_gate_v1
