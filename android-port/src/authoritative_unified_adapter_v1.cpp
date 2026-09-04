// BUILD-ONLY/OFFLINE adapter from authoritative replay selection to the
// live-proven unified phase state machine.
//
// The successful HWBP executor remains untouched and hash-pinned. A future V2
// runtime may include this translation unit after independent review. This
// file has no process access, thread control, gameplay write or action call.

#define A9TAS_UNIFIED_TICK_CORE_NO_MAIN
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "unified_tick_executor_core_v1.cpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "authoritative_replay_session_v1.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace a9tas::authoritative_adapter_v1 {

using authoritative_session_v1::QueueReplayOutcome;
using authoritative_session_v1::ReplaySessionV1;
using authoritative_tick_v1::SelectionOutcome;
using authoritative_tick_v1::TickInputStateV1;
using unified_tick_v1::RecordingFrameV1;

enum class AdapterEventV1 : std::uint32_t {
    kDeltaNonzero = 0,
    kDeltaZero = 1,
    kC98 = 2,
    kC9C = 3,
    kPrefixCertified = 4,
    kCompletion = 5,
    kCallbackOpen = 6,
    kF64 = 7,
    kCallbackClose = 8,
    kDeferredCallbackClear = 9,
    kWorldCommit = 10,
};

enum class AdapterErrorV1 : std::uint32_t {
    kNone = 0,
    kInvalidInitialization = 1,
    kNotInitialized = 2,
    kUnexpectedPhaseEvent = 3,
    kSelectionNotExact = 4,
    kSelectionTickMismatch = 5,
    kCommitStateMissing = 6,
    kCommitTickMismatch = 7,
    kEndTickFailure = 8,
    kPhaseTickMismatch = 9,
    kPoisoned = 10,
};

struct AdapterAdvanceResultV1 {
    bool accepted = false;
    bool packet_selected = false;
    bool selection_rolled_back = false;
    bool committed = false;
    bool complete = false;
    AdapterErrorV1 error = AdapterErrorV1::kNone;
    SelectionOutcome selection_outcome = SelectionOutcome::kNoPacket;
    std::uint32_t phase_actions = 0;
    std::uint32_t current_tick = 0;
    RecordingFrameV1 selected_packet{};
    TickInputStateV1 published_input{};
};

struct AdapterStateV1 {
    ReplaySessionV1 session{};
    // A PRE-C98 checkpoint makes Android pause/no-commit cycles transactional:
    // packet selection and stale draining are restored if Delta resets to zero
    // before the first certified control consumer.
    ReplaySessionV1 pre_c98_checkpoint{};
    Machine phase{};
    TickInputStateV1 next_input{};
    RecordingFrameV1 selected_packet{};
    std::uint32_t target_frame_count = 0;
    std::uint32_t committed_frames = 0;
    bool initialized = false;
    bool checkpoint_valid = false;
    bool selected_packet_valid = false;
    bool poisoned = false;
};

namespace {

bool ToPhaseEvent(AdapterEventV1 input, Event* output) noexcept {
    if (output == nullptr) return false;
    switch (input) {
        case AdapterEventV1::kDeltaNonzero:
            *output = Event::kDeltaNonzero;
            return true;
        case AdapterEventV1::kDeltaZero:
            *output = Event::kDeltaZero;
            return true;
        case AdapterEventV1::kC98:
            *output = Event::kC98;
            return true;
        case AdapterEventV1::kC9C:
            *output = Event::kC9C;
            return true;
        case AdapterEventV1::kPrefixCertified:
            *output = Event::kPrefixCertified;
            return true;
        case AdapterEventV1::kCompletion:
            *output = Event::kCompletion;
            return true;
        case AdapterEventV1::kCallbackOpen:
            *output = Event::kCallbackOpen;
            return true;
        case AdapterEventV1::kF64:
            *output = Event::kF64;
            return true;
        case AdapterEventV1::kCallbackClose:
            *output = Event::kCallbackClose;
            return true;
        case AdapterEventV1::kDeferredCallbackClear:
            *output = Event::kDeferredCallbackClear;
            return true;
        case AdapterEventV1::kWorldCommit:
            *output = Event::kWorldCommit;
            return true;
    }
    return false;
}

AdapterAdvanceResultV1 Failure(const AdapterStateV1* state,
                               AdapterErrorV1 error) noexcept {
    AdapterAdvanceResultV1 result{};
    result.error = error;
    if (state != nullptr) result.current_tick = state->next_input.race_tick;
    return result;
}

}  // namespace

bool InitializeAdapter(AdapterStateV1* state,
                       const RecordingFrameV1* frames,
                       std::size_t frame_count,
                       std::uint32_t fixed_interval_us,
                       std::uint32_t target_tick) noexcept {
    if (state == nullptr) return false;
    *state = AdapterStateV1{};
    if (state->session.QueueReplay(frames, frame_count, fixed_interval_us,
                                   target_tick, false, 0) !=
        QueueReplayOutcome::kAccepted) {
        return false;
    }
    state->target_frame_count = state->session.final_tick() + 1;
    state->phase = Machine{Stage::kWaiting, 0, state->target_frame_count};
    state->next_input.race_tick = 0;
    // Startup contract: packet zero is prefilled before the first DT event.
    state->session.OnUpdate(0);
    state->initialized = true;
    return true;
}

AdapterAdvanceResultV1 AdvanceAdapter(
    AdapterStateV1* state,
    AdapterEventV1 input_event,
    bool comparator_available,
    bool payload_equal,
    const TickInputStateV1* consumed_input_at_commit) noexcept {
    if (state == nullptr || !state->initialized) {
        return Failure(state, AdapterErrorV1::kNotInitialized);
    }
    if (state->poisoned) {
        return Failure(state, AdapterErrorV1::kPoisoned);
    }

    Event event{};
    if (!ToPhaseEvent(input_event, &event)) {
        return Failure(state, AdapterErrorV1::kUnexpectedPhaseEvent);
    }

    AdapterAdvanceResultV1 result{};
    result.current_tick = state->next_input.race_tick;
    result.selection_outcome = SelectionOutcome::kNoPacket;

    if (state->phase.stage == Stage::kWaiting &&
        event == Event::kDeltaNonzero) {
        if (state->checkpoint_valid || state->selected_packet_valid ||
            state->phase.frame_index != state->next_input.race_tick) {
            state->poisoned = true;
            return Failure(state, AdapterErrorV1::kPhaseTickMismatch);
        }
        state->pre_c98_checkpoint = state->session;
        state->checkpoint_valid = true;
        const auto begin = state->session.BeginTick(
            state->next_input.race_tick, true, true, true);
        result.selection_outcome = begin.selection.outcome;
        if (begin.selection.outcome != SelectionOutcome::kExactPacket ||
            !begin.selection.has_selected_packet) {
            state->checkpoint_valid = false;
            auto failure = Failure(state, AdapterErrorV1::kSelectionNotExact);
            failure.selection_outcome = begin.selection.outcome;
            return failure;
        }
        if (begin.packet.tick != state->next_input.race_tick ||
            begin.packet.tick != state->phase.frame_index) {
            state->session = state->pre_c98_checkpoint;
            state->checkpoint_valid = false;
            state->poisoned = true;
            return Failure(state, AdapterErrorV1::kSelectionTickMismatch);
        }
        state->selected_packet = begin.packet;
        state->selected_packet_valid = true;
        result.packet_selected = true;
        result.selected_packet = begin.packet;
    }

    const bool pause_rollback =
        state->phase.stage == Stage::kInputOpen && event == Event::kDeltaZero;
    const bool control_commit =
        state->phase.stage == Stage::kInputOpen && event == Event::kC98;
    const bool world_commit =
        state->phase.stage == Stage::kWaitWorldCommit &&
        event == Event::kWorldCommit;

    if (world_commit) {
        if (!state->selected_packet_valid || consumed_input_at_commit == nullptr) {
            state->poisoned = true;
            return Failure(state, AdapterErrorV1::kCommitStateMissing);
        }
        if (consumed_input_at_commit->race_tick !=
                state->selected_packet.tick ||
            consumed_input_at_commit->race_tick != state->phase.frame_index) {
            state->poisoned = true;
            return Failure(state, AdapterErrorV1::kCommitTickMismatch);
        }
    }

    std::uint32_t actions = 0;
    const bool skip_transform =
        state->selected_packet_valid &&
        (state->selected_packet.skip_override_flags &
         unified_tick_v1::kSkipTransformForced) != 0;
    if (!Advance(&state->phase, event, comparator_available, payload_equal,
                 skip_transform, &actions)) {
        if (state->checkpoint_valid) {
            state->session = state->pre_c98_checkpoint;
            state->checkpoint_valid = false;
            state->selected_packet_valid = false;
        }
        state->poisoned = true;
        return Failure(state, AdapterErrorV1::kUnexpectedPhaseEvent);
    }
    result.accepted = true;
    result.phase_actions = actions;

    if (pause_rollback) {
        if (!state->checkpoint_valid) {
            state->poisoned = true;
            return Failure(state, AdapterErrorV1::kUnexpectedPhaseEvent);
        }
        state->session = state->pre_c98_checkpoint;
        state->checkpoint_valid = false;
        state->selected_packet_valid = false;
        result.selection_rolled_back = true;
        return result;
    }

    if (control_commit) {
        // From C98 onward the game has consumed/been overwritten with this
        // packet. Later failure is fail-closed and cannot silently requeue it.
        state->checkpoint_valid = false;
    }

    if (world_commit) {
        TickInputStateV1 current = *consumed_input_at_commit;
        TickInputStateV1 published{};
        if (!state->session.EndTick(&current, true, &published)) {
            state->poisoned = true;
            return Failure(state, AdapterErrorV1::kEndTickFailure);
        }
        if (published.race_tick != state->selected_packet.tick ||
            current.race_tick != state->phase.frame_index) {
            state->poisoned = true;
            return Failure(state, AdapterErrorV1::kPhaseTickMismatch);
        }
        // Feed the published tick, not current.race_tick, to preserve the
        // inclusive final-packet ordering from upstream ReplayStateManager.
        state->session.OnUpdate(published.race_tick);
        state->next_input = current;
        state->selected_packet_valid = false;
        ++state->committed_frames;
        result.committed = true;
        result.complete = state->phase.stage == Stage::kComplete;
        result.published_input = published;
        result.current_tick = state->next_input.race_tick;
        if (state->committed_frames != state->phase.frame_index ||
            state->committed_frames != state->next_input.race_tick ||
            (result.complete &&
             state->committed_frames != state->target_frame_count)) {
            state->poisoned = true;
            return Failure(state, AdapterErrorV1::kPhaseTickMismatch);
        }
    }
    return result;
}

}  // namespace a9tas::authoritative_adapter_v1

#if defined(A9TAS_AUTHORITATIVE_UNIFIED_ADAPTER_SELFTEST)

namespace {

using a9tas::authoritative_adapter_v1::AdapterErrorV1;
using a9tas::authoritative_adapter_v1::AdapterEventV1;
using a9tas::authoritative_adapter_v1::AdapterStateV1;
using a9tas::authoritative_adapter_v1::AdvanceAdapter;
using a9tas::authoritative_adapter_v1::InitializeAdapter;
using a9tas::authoritative_tick_v1::ReplayMode;
using a9tas::authoritative_tick_v1::SelectionOutcome;
using a9tas::authoritative_tick_v1::TickInputStateV1;
using a9tas::unified_tick_v1::RecordingFrameV1;

bool Feed(AdapterStateV1* state, AdapterEventV1 event,
          const TickInputStateV1* committed = nullptr,
          bool comparator = false, bool equal = false) {
    return AdvanceAdapter(state, event, comparator, equal, committed).accepted;
}

bool AdapterSelfTest() {
    RecordingFrameV1 frames[2]{};
    for (std::size_t index = 0; index < 2; ++index) {
        frames[index].tick = index;
        frames[index].monotonic_ns = index * 100;
        frames[index].steering = index == 0 ? 0.25f : -0.5f;
        frames[index].brake = index == 0 ? -1.0f : 0.0f;
        frames[index].skip_override_flags =
            a9tas::unified_tick_v1::kSkipAccelerator |
            a9tas::unified_tick_v1::kSkipNitroActivation |
            a9tas::unified_tick_v1::kSkipBarrelAngular |
            a9tas::unified_tick_v1::kSkipBarrelRbx |
            a9tas::unified_tick_v1::kSkipRespawnButton;
    }

    AdapterStateV1 state{};
    if (!InitializeAdapter(&state, frames, 2, 16667, 1) ||
        state.session.replay_mode() != ReplayMode::kActiveBlockThread ||
        state.session.queued_packets() == 0) {
        return false;
    }

    if (!Feed(&state, AdapterEventV1::kDeltaZero)) return false;
    const auto first_begin = AdvanceAdapter(
        &state, AdapterEventV1::kDeltaNonzero, false, false, nullptr);
    if (!first_begin.accepted || !first_begin.packet_selected ||
        first_begin.selection_outcome != SelectionOutcome::kExactPacket ||
        first_begin.selected_packet.tick != 0) {
        return false;
    }
    const auto pause = AdvanceAdapter(
        &state, AdapterEventV1::kDeltaZero, false, false, nullptr);
    if (!pause.accepted || !pause.selection_rolled_back ||
        state.selected_packet_valid || state.committed_frames != 0) {
        return false;
    }
    const auto retry_begin = AdvanceAdapter(
        &state, AdapterEventV1::kDeltaNonzero, false, false, nullptr);
    if (!retry_begin.accepted || retry_begin.selected_packet.tick != 0) {
        return false;
    }

    const AdapterEventV1 first_prefix[] = {
        AdapterEventV1::kC98,
        AdapterEventV1::kC9C,
        AdapterEventV1::kPrefixCertified,
        AdapterEventV1::kF64,
    };
    for (const auto event : first_prefix)
        if (!Feed(&state, event)) return false;
    if (!Feed(&state, AdapterEventV1::kCallbackClose, nullptr, true, false) ||
        !Feed(&state, AdapterEventV1::kDeferredCallbackClear)) {
        return false;
    }
    TickInputStateV1 consumed0{};
    consumed0.race_tick = 0;
    consumed0.steering = 0.25f;
    consumed0.brake = -1.0f;
    const auto commit0 = AdvanceAdapter(
        &state, AdapterEventV1::kWorldCommit, false, false, &consumed0);
    if (!commit0.accepted || !commit0.committed || commit0.complete ||
        commit0.published_input.race_tick != 0 || state.next_input.race_tick != 1) {
        return false;
    }

    const AdapterEventV1 second_prefix[] = {
        AdapterEventV1::kDeltaNonzero,
        AdapterEventV1::kC98,
        AdapterEventV1::kC9C,
        AdapterEventV1::kCompletion,
        AdapterEventV1::kCallbackOpen,
        AdapterEventV1::kF64,
    };
    for (const auto event : second_prefix)
        if (!Feed(&state, event)) return false;
    if (!Feed(&state, AdapterEventV1::kCallbackClose, nullptr, true, true) ||
        !Feed(&state, AdapterEventV1::kDeferredCallbackClear)) {
        return false;
    }
    TickInputStateV1 consumed1{};
    consumed1.race_tick = 1;
    consumed1.steering = -0.5f;
    const auto commit1 = AdvanceAdapter(
        &state, AdapterEventV1::kWorldCommit, false, false, &consumed1);
    if (!commit1.accepted || !commit1.committed || !commit1.complete ||
        state.committed_frames != 2 || state.next_input.race_tick != 2 ||
        state.session.replay_mode() != ReplayMode::kInactive) {
        return false;
    }

    AdapterStateV1 target_zero{};
    if (!InitializeAdapter(&target_zero, frames, 2, 16667, 0)) return false;
    const auto zero_begin = AdvanceAdapter(
        &target_zero, AdapterEventV1::kDeltaNonzero, false, false, nullptr);
    if (zero_begin.accepted ||
        zero_begin.error != AdapterErrorV1::kSelectionNotExact ||
        zero_begin.selection_outcome != SelectionOutcome::kInactive) {
        return false;
    }

    AdapterStateV1 poisoned{};
    if (!InitializeAdapter(&poisoned, frames, 2, 16667, 1) ||
        !Feed(&poisoned, AdapterEventV1::kDeltaNonzero)) {
        return false;
    }
    const auto bad_order = AdvanceAdapter(
        &poisoned, AdapterEventV1::kC9C, false, false, nullptr);
    if (bad_order.accepted ||
        bad_order.error != AdapterErrorV1::kUnexpectedPhaseEvent ||
        !poisoned.poisoned) {
        return false;
    }
    const auto after_poison = AdvanceAdapter(
        &poisoned, AdapterEventV1::kDeltaNonzero, false, false, nullptr);
    if (after_poison.accepted ||
        after_poison.error != AdapterErrorV1::kPoisoned) {
        return false;
    }

    AdapterStateV1 uninitialized{};
    const auto invalid = AdvanceAdapter(
        &uninitialized, AdapterEventV1::kDeltaNonzero, false, false, nullptr);
    return !invalid.accepted && invalid.error == AdapterErrorV1::kNotInitialized;
}

}  // namespace

extern "C" int a9tas_authoritative_unified_adapter_selftest_v1() {
    return AdapterSelfTest() ? 0 : 1;
}

#endif

#if !defined(A9TAS_AUTHORITATIVE_UNIFIED_ADAPTER_NO_MAIN)
int main() {
    std::puts(
        "AUTH_UNIFIED_ADAPTER_BUILD_ONLY runtime=disabled return=-100 "
        "device_access=0");
    return 0;
}
#endif
