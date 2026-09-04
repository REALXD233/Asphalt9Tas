#include "startline_pause_gate_v1.h"

#include <cstdio>

namespace a9tas::startline_pause_gate_v1 {

namespace {

constexpr std::int64_t kMaximumSaneDeltaUs = 1000000;

ResultV1 Snapshot(const GateV1* gate, bool accepted,
                  bool fire_tick_zero = false) noexcept {
    ResultV1 result{};
    result.accepted = accepted;
    result.fire_tick_zero = fire_tick_zero;
    if (gate != nullptr) {
        result.phase = gate->phase;
        result.zero_observations = gate->zero_observations;
        result.trigger_count = gate->trigger_count;
    }
    return result;
}

}  // namespace

bool Initialize(GateV1* gate, std::int64_t initial_delta_us) noexcept {
    if (gate == nullptr) return false;
    *gate = {};
    if (initial_delta_us != 0) {
        gate->phase = PhaseV1::kPoisoned;
        return false;
    }
    gate->phase = PhaseV1::kPausedArmed;
    gate->zero_observations = 1;
    return true;
}

ResultV1 ObserveBeforeReplay(GateV1* gate,
                             std::int64_t observed_delta_us) noexcept {
    if (gate == nullptr || gate->phase == PhaseV1::kUninitialized ||
        gate->phase == PhaseV1::kPoisoned)
        return Snapshot(gate, false);
    if (gate->phase == PhaseV1::kTriggered)
        return Snapshot(gate, false);
    if (observed_delta_us == 0) {
        if (gate->zero_observations == UINT32_MAX) {
            gate->phase = PhaseV1::kPoisoned;
            return Snapshot(gate, false);
        }
        ++gate->zero_observations;
        return Snapshot(gate, true);
    }
    if (observed_delta_us < 0 ||
        observed_delta_us > kMaximumSaneDeltaUs) {
        gate->phase = PhaseV1::kPoisoned;
        return Snapshot(gate, false);
    }
    gate->phase = PhaseV1::kTriggered;
    gate->trigger_delta_us = observed_delta_us;
    gate->trigger_count = 1;
    return Snapshot(gate, true, true);
}

}  // namespace a9tas::startline_pause_gate_v1

#if defined(A9TAS_STARTLINE_PAUSE_GATE_SELFTEST)
extern "C" int a9tas_startline_pause_gate_selftest_v1() {
    using namespace a9tas::startline_pause_gate_v1;
    GateV1 gate{};
    if (!Initialize(&gate, 0)) return 1;
    const auto zero = ObserveBeforeReplay(&gate, 0);
    if (!zero.accepted || zero.fire_tick_zero || zero.zero_observations != 2)
        return 2;
    const auto trigger = ObserveBeforeReplay(&gate, 16667);
    if (!trigger.accepted || !trigger.fire_tick_zero ||
        trigger.phase != PhaseV1::kTriggered || trigger.trigger_count != 1)
        return 3;
    if (ObserveBeforeReplay(&gate, 16667).accepted) return 4;
    GateV1 unsafe{};
    if (Initialize(&unsafe, 1) || unsafe.phase != PhaseV1::kPoisoned) return 5;
    return 0;
}
#endif

#if !defined(A9TAS_STARTLINE_PAUSE_GATE_NO_MAIN)
int main() {
#if defined(A9TAS_STARTLINE_PAUSE_GATE_SELFTEST)
    const int result = a9tas_startline_pause_gate_selftest_v1();
    std::printf("STARTLINE_PAUSE_GATE_SELFTEST passed=%d return=%d "
                "device_access=0 game_writes=0\n",
                result == 0 ? 1 : 0, result);
    return result;
#else
    std::puts("STARTLINE_PAUSE_GATE_BUILD_ONLY runtime=disabled return=-100 "
              "device_access=0 game_writes=0");
    return 100;
#endif
}
#endif
