// BUILD-ONLY/OFFLINE unified-tick executor core.
//
// This binary validates the exact A9UTK1 wire ABI and the two-phase tick state
// machine. It deliberately has no ptrace loop and cannot write game memory.
// Runtime integration remains fail-closed until the phase-switch implementation
// has been separately reviewed and authorized.

#include "unified_tick_recording_v1.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using a9tas::unified_tick_v1::RecordingFrameV1;
using a9tas::unified_tick_v1::RecordingHeaderV1;

enum class Stage {
    kWaiting,
    kInputOpen,
    kSawC98,
    kWaitCompletion,
    kWaitCallbackOpen,
    kWaitF64,
    kWaitCallbackClose,
    kWaitDeferredClear,
    kWaitWorldCommit,
    kComplete,
};

enum class Event {
    kDeltaNonzero,
    kDeltaZero,
    kC98,
    kC9C,
    kPrefixCertified,
    kCompletion,
    kCallbackOpen,
    kF64,
    kCallbackClose,
    kDeferredCallbackClear,
    kWorldCommit,
};

enum Action : std::uint32_t {
    kNoAction = 0,
    kSelectFrameAndApplyDelta = 1u << 0,
    kPausedCycleNoCommit = 1u << 1,
    kWriteControlPair = 1u << 2,
    kRearmPostPhase = 1u << 3,
    kCorrectionZeroWrite = 1u << 4,
    kCorrectionCopyBoth = 1u << 5,
    kCorrectionSkipped = 1u << 6,
    kCommitAndRearmInput = 1u << 7,
    kCommitFinalFrame = 1u << 8,
};

struct Machine {
    Stage stage = Stage::kWaiting;
    std::size_t frame_index = 0;
    std::size_t frame_count = 0;
};

bool Advance(Machine* machine, Event event, bool comparator_available,
             bool payload_equal, bool skip_transform,
             std::uint32_t* actions) {
    if (!machine || !actions || machine->frame_count == 0 ||
        machine->frame_index > machine->frame_count ||
        machine->stage == Stage::kComplete)
        return false;
    *actions = kNoAction;
    if (machine->stage == Stage::kWaiting && event == Event::kDeltaZero)
        return true;
    if (machine->stage == Stage::kWaiting && event == Event::kDeltaNonzero) {
        machine->stage = Stage::kInputOpen;
        *actions = kSelectFrameAndApplyDelta;
        return true;
    }
    if (machine->stage == Stage::kInputOpen && event == Event::kDeltaZero) {
        machine->stage = Stage::kWaiting;
        *actions = kPausedCycleNoCommit;
        return true;
    }
    if (machine->stage == Stage::kInputOpen && event == Event::kC98) {
        machine->stage = Stage::kSawC98;
        *actions = kWriteControlPair;
        return true;
    }
    if (machine->stage == Stage::kSawC98 && event == Event::kC9C) {
        machine->stage = Stage::kWaitCompletion;
        *actions = kWriteControlPair | kRearmPostPhase;
        return true;
    }
    if (machine->stage == Stage::kWaitCompletion &&
        event == Event::kPrefixCertified) {
        machine->stage = Stage::kWaitF64;
        return true;
    }
    if (machine->stage == Stage::kWaitCompletion &&
        event == Event::kCompletion) {
        machine->stage = Stage::kWaitCallbackOpen;
        return true;
    }
    if (machine->stage == Stage::kWaitCallbackOpen &&
        event == Event::kCallbackOpen) {
        machine->stage = Stage::kWaitF64;
        return true;
    }
    if (machine->stage == Stage::kWaitF64 && event == Event::kF64) {
        machine->stage = Stage::kWaitCallbackClose;
        return true;
    }
    if (machine->stage == Stage::kWaitCallbackClose &&
        event == Event::kCallbackClose) {
        if (!comparator_available) return false;
        machine->stage = Stage::kWaitDeferredClear;
        *actions = skip_transform
                       ? kCorrectionSkipped
                       : payload_equal ? kCorrectionZeroWrite
                                       : kCorrectionCopyBoth;
        return true;
    }
    if (machine->stage == Stage::kWaitDeferredClear &&
        event == Event::kDeferredCallbackClear) {
        machine->stage = Stage::kWaitWorldCommit;
        return true;
    }
    if (machine->stage == Stage::kWaitWorldCommit &&
        event == Event::kWorldCommit) {
        ++machine->frame_index;
        if (machine->frame_index == machine->frame_count) {
            machine->stage = Stage::kComplete;
            *actions = kCommitFinalFrame;
        } else {
            machine->stage = Stage::kWaiting;
            *actions = kCommitAndRearmInput;
        }
        return true;
    }
    return false;
}

bool FiniteBounded(const float* values, std::size_t count, float limit) {
    for (std::size_t index = 0; index < count; ++index)
        if (!std::isfinite(values[index]) || std::fabs(values[index]) > limit)
            return false;
    return true;
}

bool AllZero(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index)
        if (bytes[index] != 0) return false;
    return true;
}

bool ValidHeader(const RecordingHeaderV1& header) {
    namespace unified = a9tas::unified_tick_v1;
    return std::memcmp(header.magic, unified::kMagic,
                       sizeof(unified::kMagic)) == 0 &&
           header.version == unified::kVersion &&
           header.header_size == sizeof(RecordingHeaderV1) &&
           header.frame_size == sizeof(RecordingFrameV1) &&
           header.frame_count > 0 &&
           header.frame_count <= unified::kMaximumFrames &&
           header.fixed_interval_us >= unified::kMinimumFixedIntervalUs &&
           header.fixed_interval_us <= unified::kMaximumFixedIntervalUs &&
           header.flags == unified::kRequiredHeaderFlags &&
           header.supported_skip_mask == unified::kSupportedSkipMask &&
           std::memcmp(header.build_id, unified::kSupportedBuildId,
                       sizeof(unified::kSupportedBuildId)) == 0 &&
           header.transform_offset == unified::kTransformOffset &&
           header.linear_velocity_offset ==
               unified::kLinearVelocityOffset &&
           header.angular_velocity_offset ==
               unified::kAngularVelocityOffset &&
           header.transform_size == unified::kTransformSize &&
           header.linear_velocity_size == unified::kLinearVelocitySize &&
           header.angular_velocity_size == unified::kAngularVelocitySize &&
           AllZero(header.reserved, sizeof(header.reserved));
}

bool ValidFrame(const RecordingFrameV1& frame) {
    namespace unified = a9tas::unified_tick_v1;
    float controls[3] = {frame.steering, frame.brake, frame.accelerator};
    float physics[19]{};
    std::memcpy(physics, frame.transform_bits, sizeof(frame.transform_bits));
    std::memcpy(physics + 16, frame.linear_velocity_bits,
                sizeof(frame.linear_velocity_bits));
    return FiniteBounded(controls, 3, 8.0f) &&
           frame.nitro_activation_count <= 2 &&
           (frame.skip_override_flags & ~unified::kSupportedSkipMask) == 0 &&
           frame.respawn_button_press <= 1 &&
           AllZero(frame.padding, sizeof(frame.padding)) &&
           FiniteBounded(frame.barrel_angular_velocity, 3, 1000000.0f) &&
           FiniteBounded(frame.barrel_rbx, 2, 1000000.0f) &&
           FiniteBounded(physics, 19, 1000000.0f) &&
           frame.flags == unified::kRequiredFrameFlags &&
           frame.reserved == 0;
}

bool LoadRecording(const char* path, RecordingHeaderV1* header,
                   std::vector<RecordingFrameV1>* frames) {
    if (!path || !header || !frames) return false;
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    if (std::fread(header, sizeof(*header), 1, file) != 1) {
        std::fprintf(stderr, "a9utk1_load_error=header_read\n");
        std::fclose(file);
        return false;
    }
    if (!ValidHeader(*header)) {
        std::fprintf(
            stderr,
            "a9utk1_load_error=header_validation version=%u header=%u "
            "frame=%u count=%u fixed=%u flags=0x%x skip=0x%x\n",
            header->version, header->header_size, header->frame_size,
            header->frame_count, header->fixed_interval_us, header->flags,
            header->supported_skip_mask);
        std::fclose(file);
        return false;
    }
    frames->resize(header->frame_count);
    const bool body_ok =
        std::fread(frames->data(), sizeof(frames->front()), frames->size(),
                   file) == frames->size() &&
        std::fgetc(file) == EOF;
    std::fclose(file);
    if (!body_ok) {
        std::fprintf(stderr, "a9utk1_load_error=body_length\n");
        return false;
    }

    bool have_previous = false;
    std::uint64_t previous_tick = 0;
    std::uint64_t previous_time = 0;
    for (std::size_t frame_index = 0; frame_index < frames->size();
         ++frame_index) {
        const auto& frame = (*frames)[frame_index];
        if (!ValidFrame(frame)) {
            float controls[3] = {
                frame.steering, frame.brake, frame.accelerator,
            };
            float physics[19]{};
            std::memcpy(physics, frame.transform_bits,
                        sizeof(frame.transform_bits));
            std::memcpy(physics + 16, frame.linear_velocity_bits,
                        sizeof(frame.linear_velocity_bits));
            std::fprintf(
                stderr,
                "a9utk1_load_error=frame_validation index=%zu "
                "controls=%u nitro=%u skip=0x%x respawn=%u padding=%u "
                "angular=%u rbx=%u physics=%u flags=0x%x reserved=%u\n",
                frame_index, FiniteBounded(controls, 3, 8.0f) ? 1u : 0u,
                frame.nitro_activation_count, frame.skip_override_flags,
                frame.respawn_button_press,
                AllZero(frame.padding, sizeof(frame.padding)) ? 1u : 0u,
                FiniteBounded(frame.barrel_angular_velocity, 3,
                              1000000.0f)
                    ? 1u
                    : 0u,
                FiniteBounded(frame.barrel_rbx, 2, 1000000.0f) ? 1u : 0u,
                FiniteBounded(physics, 19, 1000000.0f) ? 1u : 0u,
                frame.flags, frame.reserved);
            return false;
        }
        if (have_previous &&
            (frame.tick != previous_tick + 1 ||
             frame.monotonic_ns < previous_time)) {
            std::fprintf(stderr,
                         "a9utk1_load_error=chronology index=%zu\n",
                         frame_index);
            return false;
        }
        have_previous = true;
        previous_tick = frame.tick;
        previous_time = frame.monotonic_ns;
    }
    return true;
}

bool RunStateMachineSelfTest() {
    Machine machine{Stage::kWaiting, 0, 2};
    std::uint32_t actions = 0;
    if (!Advance(&machine, Event::kDeltaZero, false, false, false, &actions) ||
        actions != kNoAction || machine.frame_index != 0)
        return false;
    if (!Advance(&machine, Event::kDeltaNonzero, false, false, false,
                 &actions) ||
        actions != kSelectFrameAndApplyDelta)
        return false;
    if (!Advance(&machine, Event::kDeltaZero, false, false, false, &actions) ||
        actions != kPausedCycleNoCommit || machine.frame_index != 0)
        return false;

    const Event first_tick[] = {
        Event::kDeltaNonzero, Event::kC98, Event::kC9C,
        Event::kCompletion, Event::kCallbackOpen, Event::kF64,
    };
    for (Event event : first_tick)
        if (!Advance(&machine, event, false, false, false, &actions))
            return false;
    if (Advance(&machine, Event::kWorldCommit, false, false, false, &actions))
        return false;
    if (!Advance(&machine, Event::kCallbackClose, true, false, false,
                 &actions) ||
        actions != kCorrectionCopyBoth)
        return false;
    if (!Advance(&machine, Event::kDeferredCallbackClear, false, false,
                 false, &actions) ||
        actions != kNoAction)
        return false;
    if (!Advance(&machine, Event::kWorldCommit, false, false, false,
                 &actions) ||
        actions != kCommitAndRearmInput || machine.frame_index != 1)
        return false;

    const Event second_tick[] = {
        Event::kDeltaNonzero, Event::kC98, Event::kC9C,
        Event::kCompletion, Event::kCallbackOpen, Event::kF64,
    };
    for (Event event : second_tick)
        if (!Advance(&machine, event, false, false, false, &actions))
            return false;
    if (!Advance(&machine, Event::kCallbackClose, true, true, false,
                 &actions) ||
        actions != kCorrectionZeroWrite)
        return false;
    if (!Advance(&machine, Event::kDeferredCallbackClear, false, false,
                 false, &actions))
        return false;
    if (!Advance(&machine, Event::kWorldCommit, false, false, false,
                 &actions) ||
        actions != kCommitFinalFrame || machine.frame_index != 2 ||
        machine.stage != Stage::kComplete)
        return false;

    Machine prefix_machine{Stage::kWaiting, 0, 1};
    const Event prefix_tick[] = {
        Event::kDeltaNonzero, Event::kC98, Event::kC9C,
        Event::kPrefixCertified, Event::kF64,
    };
    for (Event event : prefix_tick)
        if (!Advance(&prefix_machine, event, false, false, false, &actions))
            return false;
    return prefix_machine.stage == Stage::kWaitCallbackClose;
}

}  // namespace

#ifndef A9TAS_UNIFIED_TICK_CORE_NO_MAIN
int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s A9UTK1_PATH\n", argv[0]);
        return 2;
    }
    if (!RunStateMachineSelfTest()) {
        std::fprintf(stderr, "unified tick state-machine self-test failed\n");
        return 3;
    }
    RecordingHeaderV1 header{};
    std::vector<RecordingFrameV1> frames;
    if (!LoadRecording(argv[1], &header, &frames)) {
        std::fprintf(stderr, "invalid A9UTK1 recording\n");
        return 4;
    }

    std::uint64_t active_nitro = 0;
    std::uint64_t active_respawn = 0;
    std::uint64_t active_barrel_angular = 0;
    std::uint64_t active_barrel_rbx = 0;
    for (const auto& frame : frames) {
        using namespace a9tas::unified_tick_v1;
        if (!(frame.skip_override_flags & kSkipNitroActivation) &&
            frame.nitro_activation_count != 0)
            ++active_nitro;
        if (!(frame.skip_override_flags & kSkipRespawnButton) &&
            frame.respawn_button_press != 0)
            ++active_respawn;
        if (!(frame.skip_override_flags & kSkipBarrelAngular))
            ++active_barrel_angular;
        if (!(frame.skip_override_flags & kSkipBarrelRbx))
            ++active_barrel_rbx;
    }
    std::printf(
        "A9UTK1_CORE_V1_OK frames=%zu first_tick=%llu last_tick=%llu "
        "fixed_interval_us=%u active_nitro=%llu active_respawn=%llu "
        "active_barrel_angular=%llu active_barrel_rbx=%llu "
        "runtime_game_access=disabled\n",
        frames.size(),
        static_cast<unsigned long long>(frames.front().tick),
        static_cast<unsigned long long>(frames.back().tick),
        header.fixed_interval_us,
        static_cast<unsigned long long>(active_nitro),
        static_cast<unsigned long long>(active_respawn),
        static_cast<unsigned long long>(active_barrel_angular),
        static_cast<unsigned long long>(active_barrel_rbx));
    return 0;
}
#endif
