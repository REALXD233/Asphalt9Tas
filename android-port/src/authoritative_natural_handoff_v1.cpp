// BUILD-ONLY/OFFLINE handoff from a zero-write natural-anchor search cycle to
// authoritative replay. The matched search world-commit is never forwarded as
// replay tick zero; selection begins only at the next positive Delta event.

#include "authoritative_natural_handoff_v1.h"

#include <cstdio>
#include <new>

namespace a9tas::authoritative_natural_v1 {

using authoritative_bridge_v1::EventV1;

enum class SearchStageV1 : std::uint32_t {
    kWaiting = 0,
    kInputOpen = 1,
    kAfterC98 = 2,
    kWaitPrefix = 3,
    kWaitF64 = 4,
    kWaitCallbackClose = 5,
    kWaitDeferredClear = 6,
    kWaitWorldCommit = 7,
};

struct ControllerV1 {
    authoritative_bridge_v1::BridgeV1* replay = nullptr;
    SearchStageV1 search_stage = SearchStageV1::kWaiting;
    ModeV1 mode = ModeV1::kSearching;
    std::uint32_t search_cycles = 0;
    std::uint32_t matched_cycle = 0xffffffffU;
    bool pending_anchor_match = false;
};

namespace {

bool AdvanceSearch(ControllerV1* state, EventV1 event,
                   bool comparator_available, bool anchor_matches,
                   bool* cycle_committed, bool* matched) noexcept {
    if (state == nullptr || cycle_committed == nullptr || matched == nullptr)
        return false;
    *cycle_committed = false;
    *matched = false;
    switch (state->search_stage) {
        case SearchStageV1::kWaiting:
            if (event == EventV1::kDeltaZero) return true;
            if (event == EventV1::kDeltaNonzero) {
                state->search_stage = SearchStageV1::kInputOpen;
                return true;
            }
            return false;
        case SearchStageV1::kInputOpen:
            if (event == EventV1::kDeltaZero) {
                state->search_stage = SearchStageV1::kWaiting;
                state->pending_anchor_match = false;
                return true;
            }
            if (event == EventV1::kC98) {
                state->search_stage = SearchStageV1::kAfterC98;
                return true;
            }
            return false;
        case SearchStageV1::kAfterC98:
            if (event != EventV1::kC9C) return false;
            state->search_stage = SearchStageV1::kWaitPrefix;
            return true;
        case SearchStageV1::kWaitPrefix:
            if (event != EventV1::kPrefixCertified) return false;
            state->search_stage = SearchStageV1::kWaitF64;
            return true;
        case SearchStageV1::kWaitF64:
            if (event != EventV1::kF64) return false;
            state->search_stage = SearchStageV1::kWaitCallbackClose;
            return true;
        case SearchStageV1::kWaitCallbackClose:
            if (event != EventV1::kCallbackClose || !comparator_available)
                return false;
            state->pending_anchor_match = anchor_matches;
            state->search_stage = SearchStageV1::kWaitDeferredClear;
            return true;
        case SearchStageV1::kWaitDeferredClear:
            if (event != EventV1::kDeferredCallbackClear) return false;
            state->search_stage = SearchStageV1::kWaitWorldCommit;
            return true;
        case SearchStageV1::kWaitWorldCommit:
            if (event != EventV1::kWorldCommit) return false;
            *cycle_committed = true;
            *matched = state->pending_anchor_match;
            if (state->search_cycles == 0xffffffffU) return false;
            if (state->pending_anchor_match) {
                state->matched_cycle = state->search_cycles;
                state->mode = ModeV1::kReplay;
            }
            ++state->search_cycles;
            state->pending_anchor_match = false;
            state->search_stage = SearchStageV1::kWaiting;
            return true;
    }
    return false;
}

AdvanceResultV1 Snapshot(const ControllerV1* controller) noexcept {
    AdvanceResultV1 output{};
    if (controller == nullptr) return output;
    output.mode = controller->mode;
    output.search_cycles = controller->search_cycles;
    output.matched_cycle = controller->matched_cycle;
    if (controller->mode == ModeV1::kSearching) {
        output.phase_stage =
            static_cast<std::uint32_t>(controller->search_stage);
    }
    return output;
}

}  // namespace

ControllerV1* Create(const unified_tick_v1::RecordingFrameV1* frames,
                     std::size_t frame_count,
                     std::uint32_t fixed_interval_us,
                     std::uint32_t target_tick) noexcept {
    ControllerV1* controller = new (std::nothrow) ControllerV1{};
    if (controller == nullptr) return nullptr;
    controller->replay = authoritative_bridge_v1::Create(
        frames, frame_count, fixed_interval_us, target_tick);
    if (controller->replay == nullptr) {
        delete controller;
        return nullptr;
    }
    return controller;
}

void Destroy(ControllerV1* controller) noexcept {
    if (controller == nullptr) return;
    authoritative_bridge_v1::Destroy(controller->replay);
    delete controller;
}

bool RejectSearchPrefix(ControllerV1* controller) noexcept {
    if (controller == nullptr || controller->mode != ModeV1::kSearching)
        return false;
    controller->search_stage = SearchStageV1::kWaiting;
    controller->pending_anchor_match = false;
    return true;
}

AdvanceResultV1 Inspect(const ControllerV1* controller) noexcept {
    return Snapshot(controller);
}

AdvanceResultV1 Advance(ControllerV1* controller, EventV1 event,
                        bool anchor_comparator_available,
                        bool anchor_matches, bool payload_equal,
                        bool consumed_tick_valid,
                        std::uint32_t consumed_tick) noexcept {
    if (controller == nullptr || controller->mode == ModeV1::kPoisoned ||
        controller->mode == ModeV1::kComplete) {
        return Snapshot(controller);
    }
    AdvanceResultV1 output = Snapshot(controller);
    if (controller->mode == ModeV1::kSearching) {
        if (!AdvanceSearch(controller, event, anchor_comparator_available,
                           anchor_matches, &output.search_cycle_committed,
                           &output.anchor_matched)) {
            controller->mode = ModeV1::kPoisoned;
        } else {
            output.accepted = true;
        }
    } else {
        output.replay = authoritative_bridge_v1::Advance(
            controller->replay, event, anchor_comparator_available,
            payload_equal, consumed_tick_valid, consumed_tick);
        output.accepted = output.replay.accepted;
        if (output.replay.poisoned || !output.replay.accepted) {
            controller->mode = ModeV1::kPoisoned;
        } else if (output.replay.complete) {
            controller->mode = ModeV1::kComplete;
        }
    }
    output.mode = controller->mode;
    output.search_cycles = controller->search_cycles;
    output.matched_cycle = controller->matched_cycle;
    if (controller->mode == ModeV1::kSearching) {
        output.phase_stage =
            static_cast<std::uint32_t>(controller->search_stage);
        output.phase_frame_index = 0;
    } else {
        output.phase_stage = output.replay.phase_stage;
        output.phase_frame_index = output.replay.phase_frame_index;
    }
    return output;
}

}  // namespace a9tas::authoritative_natural_v1

#if defined(A9TAS_AUTHORITATIVE_NATURAL_HANDOFF_SELFTEST)
extern "C" int a9tas_authoritative_natural_handoff_selftest_v1() {
    using namespace a9tas::authoritative_natural_v1;
    using a9tas::authoritative_bridge_v1::EventV1;
    a9tas::unified_tick_v1::RecordingFrameV1 frames[2]{};
    for (std::uint32_t tick = 0; tick < 2; ++tick) {
        frames[tick].tick = tick;
        frames[tick].monotonic_ns = tick * 16667000ULL;
        frames[tick].skip_override_flags = 0xfe;
        frames[tick].flags = 7;
    }
    ControllerV1* state = Create(frames, 2, 16667, 1);
    if (state == nullptr) return 1;
    const EventV1 prefix[] = {EventV1::kDeltaNonzero, EventV1::kC98,
        EventV1::kC9C, EventV1::kPrefixCertified, EventV1::kF64};
    for (EventV1 event : prefix) if (!Advance(state, event).accepted) return 2;
    if (!Advance(state, EventV1::kCallbackClose, true, false).accepted ||
        !Advance(state, EventV1::kDeferredCallbackClear).accepted ||
        !Advance(state, EventV1::kWorldCommit).search_cycle_committed) return 3;
    if (!Advance(state, EventV1::kDeltaNonzero).accepted ||
        !Advance(state, EventV1::kDeltaZero).accepted) return 4;
    for (EventV1 event : prefix) if (!Advance(state, event).accepted) return 5;
    if (!Advance(state, EventV1::kCallbackClose, true, true).accepted ||
        !Advance(state, EventV1::kDeferredCallbackClear).accepted) return 6;
    auto handoff = Advance(state, EventV1::kWorldCommit);
    if (!handoff.accepted || !handoff.anchor_matched ||
        handoff.mode != ModeV1::kReplay || handoff.search_cycles != 2 ||
        handoff.replay.packet_selected) return 7;
    auto tick0 = Advance(state, EventV1::kDeltaNonzero);
    if (!tick0.accepted || !tick0.replay.packet_selected ||
        tick0.replay.selected_packet.tick != 0) return 8;
    Destroy(state);
    return 0;
}
#endif

#if !defined(A9TAS_AUTHORITATIVE_NATURAL_HANDOFF_NO_MAIN)
int main() {
#if defined(A9TAS_AUTHORITATIVE_NATURAL_HANDOFF_SELFTEST)
    const int result = a9tas_authoritative_natural_handoff_selftest_v1();
    std::printf("AUTHORITATIVE_NATURAL_HANDOFF_SELFTEST passed=%d return=%d device_access=0 game_writes=0\n",
                result == 0 ? 1 : 0, result);
    return result;
#else
    std::puts("AUTHORITATIVE_NATURAL_HANDOFF_V1_BUILD_ONLY runtime=disabled return=-100 device_access=0 game_writes=0");
    return -100;
#endif
}
#endif
