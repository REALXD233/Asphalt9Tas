// BUILD-ONLY/OFFLINE stable bridge for the authoritative unified adapter.
// The future HWBP runtime can consume this API without including the adapter
// implementation into the same translation unit as legacy phase helpers.

#define A9TAS_AUTHORITATIVE_UNIFIED_ADAPTER_NO_MAIN
#include "authoritative_unified_adapter_v1.cpp"

#include "authoritative_adapter_bridge_v1.h"
#include "authoritative_steering_transport_v1.h"

#include <cstdio>
#include <new>

namespace a9tas::authoritative_bridge_v1 {

using authoritative_adapter_v1::AdapterEventV1;
using authoritative_adapter_v1::AdapterStateV1;
using authoritative_adapter_v1::AdvanceAdapter;
using authoritative_adapter_v1::InitializeAdapter;
using authoritative_tick_v1::TickInputStateV1;

struct BridgeV1 {
    AdapterStateV1 state{};
};

namespace {

bool MapEvent(EventV1 input, AdapterEventV1* output) noexcept {
    if (output == nullptr) return false;
    switch (input) {
        case EventV1::kDeltaNonzero: *output = AdapterEventV1::kDeltaNonzero; return true;
        case EventV1::kDeltaZero: *output = AdapterEventV1::kDeltaZero; return true;
        case EventV1::kC98: *output = AdapterEventV1::kC98; return true;
        case EventV1::kC9C: *output = AdapterEventV1::kC9C; return true;
        case EventV1::kPrefixCertified: *output = AdapterEventV1::kPrefixCertified; return true;
        case EventV1::kCompletion: *output = AdapterEventV1::kCompletion; return true;
        case EventV1::kCallbackOpen: *output = AdapterEventV1::kCallbackOpen; return true;
        case EventV1::kF64: *output = AdapterEventV1::kF64; return true;
        case EventV1::kCallbackClose: *output = AdapterEventV1::kCallbackClose; return true;
        case EventV1::kDeferredCallbackClear: *output = AdapterEventV1::kDeferredCallbackClear; return true;
        case EventV1::kWorldCommit: *output = AdapterEventV1::kWorldCommit; return true;
    }
    return false;
}

AdvanceResultV1 Snapshot(const BridgeV1* bridge) noexcept {
    AdvanceResultV1 output{};
    if (bridge == nullptr) {
        output.poisoned = true;
        return output;
    }
    output.poisoned = bridge->state.poisoned;
    output.selected_packet_valid = bridge->state.selected_packet_valid;
    output.phase_stage = static_cast<std::uint32_t>(bridge->state.phase.stage);
    output.phase_frame_index = static_cast<std::uint32_t>(bridge->state.phase.frame_index);
    output.committed_frames = bridge->state.committed_frames;
    output.current_tick = bridge->state.next_input.race_tick;
    if (bridge->state.selected_packet_valid) {
        output.selected_packet = bridge->state.selected_packet;
    }
    return output;
}

}  // namespace

BridgeV1* Create(const unified_tick_v1::RecordingFrameV1* frames,
                 std::size_t frame_count, std::uint32_t fixed_interval_us,
                 std::uint32_t target_tick) noexcept {
    BridgeV1* bridge = new (std::nothrow) BridgeV1{};
    if (bridge == nullptr) return nullptr;
    if (!InitializeAdapter(&bridge->state, frames, frame_count,
                           fixed_interval_us, target_tick)) {
        delete bridge;
        return nullptr;
    }
    return bridge;
}

void Destroy(BridgeV1* bridge) noexcept { delete bridge; }

AdvanceResultV1 Advance(BridgeV1* bridge, EventV1 event,
                        bool comparator_available, bool payload_equal,
                        bool consumed_tick_valid,
                        std::uint32_t consumed_tick) noexcept {
    if (bridge == nullptr) return Snapshot(nullptr);
    AdapterEventV1 mapped{};
    if (!MapEvent(event, &mapped)) return Snapshot(bridge);
    TickInputStateV1 consumed{};
    const TickInputStateV1* consumed_ptr = nullptr;
    if (consumed_tick_valid) {
        consumed.race_tick = consumed_tick;
        consumed_ptr = &consumed;
    }
    const auto result = AdvanceAdapter(&bridge->state, mapped,
                                       comparator_available, payload_equal,
                                       consumed_ptr);
    AdvanceResultV1 output = Snapshot(bridge);
    output.accepted = result.accepted;
    output.packet_selected = result.packet_selected;
    output.selection_rolled_back = result.selection_rolled_back;
    output.committed = result.committed;
    output.complete = result.complete;
    output.error = static_cast<std::uint32_t>(result.error);
    output.selection_outcome =
        static_cast<std::uint32_t>(result.selection_outcome);
    output.phase_actions = result.phase_actions;
    if (result.packet_selected) {
        output.selected_packet_valid = true;
        output.selected_packet = result.selected_packet;
    }
    return output;
}

}  // namespace a9tas::authoritative_bridge_v1

#if defined(A9TAS_AUTHORITATIVE_ADAPTER_BRIDGE_SELFTEST)

extern "C" int a9tas_authoritative_adapter_bridge_selftest_v1() {
    using namespace a9tas::authoritative_bridge_v1;
    using namespace a9tas::authoritative_steering_v1;
    using a9tas::unified_tick_v1::RecordingFrameV1;

    constexpr std::size_t kFrames = 344;
    RecordingFrameV1 frames[kFrames]{};
    for (std::size_t index = 0; index < kFrames; ++index) {
        frames[index].tick = index;
        frames[index].monotonic_ns = index * 16667000ULL;
        frames[index].steering = index < 250 ? 0.0f : -0.75f;
        frames[index].skip_override_flags = kSteeringOnlySkipMask;
        frames[index].flags = a9tas::unified_tick_v1::kRequiredFrameFlags;
    }
    BridgeV1* bridge = Create(frames, kFrames, 16667, kFrames - 1);
    if (bridge == nullptr) return 1;
    std::uint64_t pair_writes = 0;
    for (std::uint32_t tick = 0; tick < kFrames; ++tick) {
        auto result = Advance(bridge, EventV1::kDeltaNonzero);
        if (!result.accepted || !result.packet_selected ||
            !result.selected_packet_valid ||
            result.selected_packet.tick != tick) {
            Destroy(bridge);
            return 2;
        }
        const auto selected = result.selected_packet;
        const std::uint64_t c98_before = 0xaaaaaaaadead0000ULL | tick;
        const std::uint64_t c9c_before = 0x55555555beef0000ULL | tick;
        result = Advance(bridge, EventV1::kC98);
        const auto c98 = PlanPair(selected, c98_before);
        if (!result.accepted || !VerifyAppliedPair(c98, c98.pair_intended)) {
            Destroy(bridge);
            return 3;
        }
        ++pair_writes;
        result = Advance(bridge, EventV1::kC9C);
        const auto c9c = PlanPair(selected, c9c_before);
        if (!result.accepted || !VerifyAppliedPair(c9c, c9c.pair_intended)) {
            Destroy(bridge);
            return 4;
        }
        ++pair_writes;
        const EventV1 tail[] = {
            EventV1::kPrefixCertified, EventV1::kF64,
            EventV1::kCallbackClose, EventV1::kDeferredCallbackClear,
        };
        for (EventV1 event : tail) {
            result = Advance(bridge, event, event == EventV1::kCallbackClose,
                             false);
            if (!result.accepted) {
                Destroy(bridge);
                return 5;
            }
        }
        result = Advance(bridge, EventV1::kWorldCommit, false, false, true,
                         tick);
        if (!result.accepted || !result.committed ||
            result.committed_frames != tick + 1 ||
            result.current_tick != tick + 1 ||
            (tick + 1 == kFrames) != result.complete) {
            Destroy(bridge);
            return 6;
        }
    }
    Destroy(bridge);
    return pair_writes == 688 ? 0 : 7;
}

#endif

#if !defined(A9TAS_AUTHORITATIVE_ADAPTER_BRIDGE_NO_MAIN)
int main() {
#if defined(A9TAS_AUTHORITATIVE_ADAPTER_BRIDGE_SELFTEST)
    const int result = a9tas_authoritative_adapter_bridge_selftest_v1();
    std::printf(
        "AUTHORITATIVE_ADAPTER_BRIDGE_SELFTEST passed=%d return=%d "
        "frames=344 pair_slots=688 device_access=0 game_writes=0\n",
        result == 0 ? 1 : 0, result);
    return result;
#else
    std::puts(
        "AUTHORITATIVE_ADAPTER_BRIDGE_V1_BUILD_ONLY runtime=disabled "
        "return=-100 device_access=0 game_writes=0");
    return -100;
#endif
}
#endif

