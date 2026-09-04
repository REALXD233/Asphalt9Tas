#pragma once

#include <cstdint>

namespace a9tas::startline_prearm_v1 {

enum class PhaseV1 : std::uint32_t {
    kUninitialized = 0,
    kReadyNoAttach = 1,
    kResumeObserved = 2,
    kAttachedWaitingForCompleteCycle = 3,
    kTickZeroCommitted = 4,
    kPoisoned = 5,
};

struct ProtocolV1 {
    PhaseV1 phase = PhaseV1::kUninitialized;
    std::uint32_t paused_zero_observations = 0;
    std::uint32_t resume_observations = 0;
    std::uint32_t attach_permissions = 0;
    std::uint32_t complete_cycles = 0;
    std::int64_t resume_delta_us = 0;
};

struct ResultV1 {
    bool accepted = false;
    bool publish_ready_no_attach = false;
    bool permit_attach = false;
    bool commit_tick_zero = false;
    PhaseV1 phase = PhaseV1::kPoisoned;
};

// Establishes a paused baseline. A positive or negative initial delta poisons
// the protocol: READY_NO_ATTACH must never be published from a moving race.
ResultV1 Initialize(ProtocolV1* protocol,
                    std::int64_t initial_delta_us) noexcept;

// Read-only polling before ptrace. Zero retains READY_NO_ATTACH. The first
// sane positive delta is the only transition that permits attaching.
ResultV1 ObserveBeforeAttach(ProtocolV1* protocol,
                             std::int64_t observed_delta_us) noexcept;

// Must be called exactly once after the process was attached successfully.
ResultV1 MarkAttached(ProtocolV1* protocol) noexcept;

// Partial prefixes after attach are deliberately ignored by the runtime. Only
// the first complete authoritative cycle establishes frame/tick zero.
ResultV1 ObserveCompleteCycle(ProtocolV1* protocol) noexcept;

}  // namespace a9tas::startline_prearm_v1
