#pragma once

#include <cstdint>

#include "fc3_replay_observer_protocol_v1.h"

namespace a9tas::fc3_observer_event_core_v1 {

enum class WatchPlan : std::uint32_t {
  // DR0 callback flags, DR1 dedicated_entries, DR2 action queue end,
  // DR3 phase-witness accumulator.
  kBeforeDedicated = 0,
  // DR0 callback flags, DR1 Nitro active/mode 8-byte block,
  // DR2 action queue end, DR3 phase-witness accumulator.
  kAfterDedicated = 1,
};

enum class Decision : std::uint32_t {
  kIgnorePreGate = 0,
  kContinue = 1,
  kRearmNitroAndContinue = 2,
  kPhaseProved = 3,
  kReject = 4,
};

struct Stop {
  std::uint32_t dr6;
  fc3_replay_observer_v1::Snapshot snapshot;
  std::uint32_t dedicated_members_full;
  std::uint32_t payload_failures;
};

static_assert(sizeof(Stop) == 76, "FC-3 event-core stop ABI");

class Core {
 public:
  explicit Core(std::int32_t owner_tid);

  Decision Consume(const Stop& stop,
                   fc3_replay_observer_v1::Report* report);
  WatchPlan watch_plan() const { return watch_plan_; }
  bool rejected() const { return rejected_ || machine_.rejected(); }
  bool phase_proved() const { return machine_.phase_proved(); }
  fc3_replay_observer_v1::Phase phase() const { return machine_.phase(); }

 private:
  bool rejected_ = false;
  WatchPlan watch_plan_ = WatchPlan::kBeforeDedicated;
  fc3_replay_observer_v1::StateMachine machine_;
};

}  // namespace a9tas::fc3_observer_event_core_v1
