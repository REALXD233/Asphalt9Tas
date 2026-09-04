#pragma once

#include <cstdint>

namespace a9tas::startline_pause_gate_v1 {

enum class PhaseV1 : std::uint32_t {
    kUninitialized = 0,
    kPausedArmed = 1,
    kTriggered = 2,
    kPoisoned = 3,
};

struct GateV1 {
    PhaseV1 phase = PhaseV1::kUninitialized;
    std::uint32_t zero_observations = 0;
    std::uint32_t trigger_count = 0;
    std::int64_t trigger_delta_us = 0;
};

struct ResultV1 {
    bool accepted = false;
    bool fire_tick_zero = false;
    PhaseV1 phase = PhaseV1::kPoisoned;
    std::uint32_t zero_observations = 0;
    std::uint32_t trigger_count = 0;
};

// Initialization is deliberately strict: the executor must be deployed while
// the race is paused and the live accumulator/delta value is exactly zero.
bool Initialize(GateV1* gate, std::int64_t initial_delta_us) noexcept;

// Before the one-shot trigger, only zero or a sane positive delta is valid.
// The first sane positive value after the verified zero baseline fires tick 0.
// Once triggered, the replay state machine owns all subsequent delta events.
ResultV1 ObserveBeforeReplay(GateV1* gate,
                             std::int64_t observed_delta_us) noexcept;

}  // namespace a9tas::startline_pause_gate_v1
