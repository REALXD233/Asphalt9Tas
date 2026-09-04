// Pure offline event classifier for the future FC-3 host observer.
// It contains no process/device primitive and performs no writes or calls.

#include "fc3_observer_event_core_v1.h"

#include <cstdio>

namespace a9tas::fc3_observer_event_core_v1 {

namespace protocol = a9tas::fc3_replay_observer_v1;

Core::Core(std::int32_t owner_tid) : machine_(owner_tid) {}

Decision Core::Consume(const Stop& stop, protocol::Report* report) {
  if (report == nullptr) {
    rejected_ = true;
    return Decision::kReject;
  }
  if (rejected_ || machine_.rejected() || machine_.phase_proved())
    return Decision::kReject;
  const std::uint32_t hits = stop.dr6 & 0xFu;
  if (hits == 0 || (hits & (hits - 1u)) != 0) {
    report->reject_reasons |= protocol::kRejectPtrace;
    ++report->ptrace_errors;
    rejected_ = true;
    return Decision::kReject;
  }

  protocol::Event event{};
  event.snapshot = stop.snapshot;
  event.dedicated_members_full = stop.dedicated_members_full;
  event.payload_failures = stop.payload_failures;

  if (watch_plan_ == WatchPlan::kBeforeDedicated) {
    if (hits != 0x2u) {
      // Callback flags and the natural accumulator may fire while FC-2 is
      // still establishing membership.  They are outside the FC-3 window.
      // Action-queue activity is never silently ignored.
      if (hits == 0x4u) {
        event.kind = protocol::EventKind::kActionQueueWrite;
        (void)machine_.Consume(event, report);
        rejected_ = true;
        return Decision::kReject;
      }
      return Decision::kIgnorePreGate;
    }
    event.kind = protocol::EventKind::kDedicatedHit;
    if (!machine_.Consume(event, report)) {
      rejected_ = true;
      return Decision::kReject;
    }
    watch_plan_ = WatchPlan::kAfterDedicated;
    return Decision::kRearmNitroAndContinue;
  }

  if (hits == 0x2u) {
    // DR1 covers the aligned NitroState [active+0x188, mode+0x18f] block.
    // Either active or mode writes are forbidden in the neutral FC-3 window.
    event.kind = protocol::EventKind::kNitroActiveWrite;
  } else if (hits == 0x4u) {
    event.kind = protocol::EventKind::kActionQueueWrite;
  } else if (hits == 0x8u) {
    event.kind = protocol::EventKind::kNextAccumulator;
  } else {
    const std::uint8_t flags =
        static_cast<std::uint8_t>(stop.snapshot.callback_flags & 0xFFu);
    if (machine_.phase() == protocol::Phase::kWaitingCallbackClose &&
        flags == 0u) {
      event.kind = protocol::EventKind::kCallbackClose;
    } else if (machine_.phase() ==
                   protocol::Phase::kWaitingNextCallbackOpen &&
               flags == 1u) {
      event.kind = protocol::EventKind::kNextCallbackOpen;
    } else {
      report->reject_reasons |= protocol::kRejectWrongOrder;
      ++report->semantic_errors;
      rejected_ = true;
      return Decision::kReject;
    }
  }

  if (!machine_.Consume(event, report)) {
    rejected_ = true;
    return Decision::kReject;
  }
  return machine_.phase_proved() ? Decision::kPhaseProved
                                 : Decision::kContinue;
}

}  // namespace a9tas::fc3_observer_event_core_v1

#ifndef A9TAS_FC3_EVENT_CORE_REVIEW
#define A9TAS_FC3_EVENT_CORE_REVIEW 0
#endif

#if A9TAS_FC3_EVENT_CORE_REVIEW == 1

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_fc3_event_core_review_protocol_v1() {
  return 1;
}

#else

int main() {
  std::puts("FC3_EVENT_CORE_BUILD_ONLY runtime=disabled return=-100 "
            "device_access=0 attached=0 game_writes=0 action_calls=0");
  return 100;
}

#endif
