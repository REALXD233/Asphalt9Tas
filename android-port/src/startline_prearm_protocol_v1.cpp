#include "startline_prearm_protocol_v1.h"

#include <climits>
#include <cstdio>

namespace a9tas::startline_prearm_v1 {
namespace {

constexpr std::int64_t kMaximumSaneDeltaUs = 1000000;

ResultV1 Snapshot(const ProtocolV1* protocol, bool accepted,
                  bool publish_ready = false, bool permit_attach = false,
                  bool commit_tick_zero = false) noexcept {
    ResultV1 result{};
    result.accepted = accepted;
    result.publish_ready_no_attach = publish_ready;
    result.permit_attach = permit_attach;
    result.commit_tick_zero = commit_tick_zero;
    if (protocol != nullptr) result.phase = protocol->phase;
    return result;
}

ResultV1 Poison(ProtocolV1* protocol) noexcept {
    if (protocol != nullptr) protocol->phase = PhaseV1::kPoisoned;
    return Snapshot(protocol, false);
}

}  // namespace

ResultV1 Initialize(ProtocolV1* protocol,
                    std::int64_t initial_delta_us) noexcept {
    if (protocol == nullptr) return Snapshot(nullptr, false);
    *protocol = {};
    if (initial_delta_us != 0) return Poison(protocol);
    protocol->phase = PhaseV1::kReadyNoAttach;
    protocol->paused_zero_observations = 1;
    return Snapshot(protocol, true, true);
}

ResultV1 ObserveBeforeAttach(ProtocolV1* protocol,
                             std::int64_t observed_delta_us) noexcept {
    if (protocol == nullptr ||
        protocol->phase != PhaseV1::kReadyNoAttach)
        return Snapshot(protocol, false);
    if (observed_delta_us == 0) {
        if (protocol->paused_zero_observations == UINT32_MAX)
            return Poison(protocol);
        ++protocol->paused_zero_observations;
        return Snapshot(protocol, true);
    }
    if (observed_delta_us < 0 || observed_delta_us > kMaximumSaneDeltaUs)
        return Poison(protocol);
    protocol->phase = PhaseV1::kResumeObserved;
    protocol->resume_delta_us = observed_delta_us;
    protocol->resume_observations = 1;
    protocol->attach_permissions = 1;
    return Snapshot(protocol, true, false, true);
}

ResultV1 MarkAttached(ProtocolV1* protocol) noexcept {
    if (protocol == nullptr ||
        protocol->phase != PhaseV1::kResumeObserved ||
        protocol->attach_permissions != 1)
        return Poison(protocol);
    protocol->phase = PhaseV1::kAttachedWaitingForCompleteCycle;
    return Snapshot(protocol, true);
}

ResultV1 ObserveCompleteCycle(ProtocolV1* protocol) noexcept {
    if (protocol == nullptr ||
        protocol->phase != PhaseV1::kAttachedWaitingForCompleteCycle)
        return Poison(protocol);
    protocol->phase = PhaseV1::kTickZeroCommitted;
    protocol->complete_cycles = 1;
    return Snapshot(protocol, true, false, false, true);
}

}  // namespace a9tas::startline_prearm_v1

#if defined(A9TAS_STARTLINE_PREARM_PROTOCOL_SELFTEST)
extern "C" int a9tas_startline_prearm_protocol_selftest_v1() {
    using namespace a9tas::startline_prearm_v1;
    ProtocolV1 protocol{};
    const auto ready = Initialize(&protocol, 0);
    if (!ready.accepted || !ready.publish_ready_no_attach ||
        ready.permit_attach)
        return 1;
    if (!ObserveBeforeAttach(&protocol, 0).accepted) return 2;
    const auto resume = ObserveBeforeAttach(&protocol, 16667);
    if (!resume.accepted || !resume.permit_attach ||
        protocol.attach_permissions != 1)
        return 3;
    if (!MarkAttached(&protocol).accepted) return 4;
    const auto tick0 = ObserveCompleteCycle(&protocol);
    if (!tick0.accepted || !tick0.commit_tick_zero ||
        protocol.complete_cycles != 1)
        return 5;
    if (ObserveCompleteCycle(&protocol).accepted) return 6;
    ProtocolV1 unsafe{};
    if (Initialize(&unsafe, 1).accepted ||
        unsafe.phase != PhaseV1::kPoisoned)
        return 7;
    return 0;
}
#endif

#if !defined(A9TAS_STARTLINE_PREARM_PROTOCOL_NO_MAIN)
int main() {
#if defined(A9TAS_STARTLINE_PREARM_PROTOCOL_SELFTEST)
    const int result = a9tas_startline_prearm_protocol_selftest_v1();
    std::printf("STARTLINE_PREARM_PROTOCOL_SELFTEST passed=%d return=%d "
                "device_access=0 game_writes=0 ptrace=0\n",
                result == 0 ? 1 : 0, result);
    return result;
#else
    std::puts("STARTLINE_PREARM_PROTOCOL_BUILD_ONLY runtime=disabled "
              "device_access=0 game_writes=0 ptrace=0");
    return 100;
#endif
}
#endif
