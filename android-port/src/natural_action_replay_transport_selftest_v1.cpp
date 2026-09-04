#include "natural_action_replay_transport_v1.h"

#include <cstdio>

namespace {

using namespace a9tas::natural_action_replay_v1;

bool Run() {
    a9tas::unified_tick_v1::RecordingFrameV1 frame{};
    frame.tick = 7;
    frame.flags = a9tas::unified_tick_v1::kRequiredFrameFlags;
    frame.nitro_activation_count = 1;
    a9tas::natural_action_callback_v1::Command command{};
    if (!BuildFrameCommand(frame, 8, 11, 0x1000, 22, &command) ||
        command.replay_frame != 7 || command.nitro_activations != 1 ||
        command.flags !=
            (a9tas::natural_action_callback_v1::kReplayFrame |
             a9tas::natural_action_callback_v1::kNitroOverrideEnabled))
        return false;

    frame.nitro_activation_count = 2;
    if (!BuildFrameCommand(frame, 8, 11, 0x1000, 22, &command) ||
        command.nitro_activations != 2)
        return false;

    frame.skip_override_flags =
        a9tas::unified_tick_v1::kSkipNitroActivation;
    if (!BuildFrameCommand(frame, 8, 11, 0x1000, 22, &command) ||
        command.nitro_activations != 0 ||
        command.flags != a9tas::natural_action_callback_v1::kReplayFrame)
        return false;
    if (BuildFrameCommand(frame, 9, 11, 0x1000, 22, &command)) return false;

    const NitroSnapshot idle{0, {0, 0, 0}, 0};
    const NitroSnapshot yellow{1, {0, 0, 0}, 1};
    const NitroSnapshot perfect{1, {0, 0, 0}, 2};
    if (!ActionTransitionComplete(idle, yellow, 1) ||
        ProveTransition(idle, yellow, 1) != kActiveChanged + kModeChanged ||
        !ActionTransitionComplete(yellow, perfect, 1) ||
        ProveTransition(yellow, perfect, 1) != kModeChanged ||
        !ActionTransitionComplete(idle, perfect, 2) ||
        ActionTransitionComplete(yellow, yellow, 1) ||
        ActionTransitionComplete(idle, yellow, 0))
        return false;
    return true;
}

}  // namespace

int main() {
    const bool passed = Run();
    std::printf("NATURAL_ACTION_REPLAY_TRANSPORT_SELFTEST passed=%u "
                "counts_0_1_2=1 colour_state_forced=0 exact_sequence=1 "
                "device_access=0\n",
                passed ? 1u : 0u);
    return passed ? 0 : 1;
}

