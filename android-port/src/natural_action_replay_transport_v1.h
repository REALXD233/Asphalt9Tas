#pragma once

// Pure AluTasV2-compatible Nitro event transport.
//
// Upstream records the number of real Nitro activations issued in each
// authoritative frame (0/1/2).  It does not record a colour or force a Nitro
// mode.  This contract preserves that representation and lets the game derive
// yellow/perfect/shockwave/red from its live Nitro state and the timing of the
// second activation.

#include "natural_action_callback_mailbox_v1.h"
#include "unified_tick_recording_v1.h"

#include <cstdint>
#include <cstring>
#include <limits>

namespace a9tas::natural_action_replay_v1 {

namespace mailbox = a9tas::natural_action_callback_v1;
namespace recording = a9tas::unified_tick_v1;

struct NitroSnapshot {
    std::uint8_t active{};
    std::uint8_t reserved[3]{};
    std::uint32_t mode{};
};

enum TransitionProof : std::uint32_t {
    kNoAction = 0,
    kActiveChanged = 1u << 0,
    kModeChanged = 1u << 1,
};

inline bool SnapshotValid(const NitroSnapshot& snapshot) noexcept {
    return snapshot.active <= 1 && snapshot.reserved[0] == 0 &&
           snapshot.reserved[1] == 0 && snapshot.reserved[2] == 0;
}

inline std::uint32_t ProveTransition(const NitroSnapshot& before,
                                     const NitroSnapshot& after,
                                     std::uint32_t activations) noexcept {
    if (!SnapshotValid(before) || !SnapshotValid(after) || activations > 2)
        return 0;
    if (activations == 0) return kNoAction;
    std::uint32_t proof = 0;
    if (before.active != after.active) proof |= kActiveChanged;
    if (before.mode != after.mode) proof |= kModeChanged;
    return proof;
}

inline bool ActionTransitionComplete(const NitroSnapshot& before,
                                     const NitroSnapshot& after,
                                     std::uint32_t activations) noexcept {
    return activations != 0 && ProveTransition(before, after, activations) != 0;
}

inline bool BuildFrameCommand(const recording::RecordingFrameV1& frame,
                              std::uint64_t sequence,
                              std::uint32_t session_id,
                              std::uintptr_t vehicle_owner,
                              std::uint32_t producer_tid,
                              mailbox::Command* output) noexcept {
    if (output == nullptr || frame.tick > std::numeric_limits<std::uint32_t>::max() ||
        sequence != frame.tick + 1 || session_id == 0 || vehicle_owner == 0 ||
        producer_tid == 0 || frame.flags != recording::kRequiredFrameFlags ||
        frame.reserved != 0 ||
        (frame.skip_override_flags & ~recording::kSupportedSkipMask) != 0 ||
        frame.nitro_activation_count > mailbox::kMaximumNitroActivations) {
        return false;
    }

    mailbox::Command command{};
    std::memcpy(command.magic, mailbox::kCommandMagic,
                sizeof(mailbox::kCommandMagic));
    command.version = mailbox::kVersion;
    command.size = sizeof(command);
    command.sequence = sequence;
    command.session_id = session_id;
    command.replay_frame = static_cast<std::uint32_t>(frame.tick);
    command.vehicle_owner = vehicle_owner;
    command.expected_producer_tid = producer_tid;
    command.flags = mailbox::kReplayFrame;

    const bool skip =
        (frame.skip_override_flags & recording::kSkipNitroActivation) != 0;
    if (!skip) {
        command.nitro_activations = frame.nitro_activation_count;
        command.flags |= mailbox::kNitroOverrideEnabled;
    } else {
        // Skip means this replay component does not synthesize Nitro.  Keep the
        // command in the exact per-frame sequence so the mailbox cursor cannot
        // drift relative to steering, brake or physics correction.
        command.nitro_activations = 0;
    }
    command.checksum = mailbox::Checksum(command);
    if (!mailbox::CommandValid(command)) return false;
    *output = command;
    return true;
}

}  // namespace a9tas::natural_action_replay_v1

