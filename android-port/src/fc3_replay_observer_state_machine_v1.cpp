// FC-3 offline-only protocol state machine.
//
// This unit deliberately contains no ptrace, /proc, pwrite, game-call,
// NativeBridge, input, Nitro, fixed-delta, or physics-state primitive.  A later
// separately reviewed controller may feed it snapshots collected by a bounded
// read-only observer.  Until then the default executable is inert.

#include "fc3_replay_observer_protocol_v1.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace a9tas::fc3_replay_observer_v1 {

namespace {

bool QueueShapeValid(const Snapshot& snapshot) {
  return snapshot.action_queue_begin <= snapshot.action_queue_end &&
         snapshot.action_queue_end <= snapshot.action_queue_capacity &&
         ((snapshot.action_queue_end - snapshot.action_queue_begin) & 7u) ==
             0 &&
         ((snapshot.action_queue_capacity - snapshot.action_queue_begin) &
          7u) == 0 &&
         snapshot.action_queue_count ==
             (snapshot.action_queue_end - snapshot.action_queue_begin) / 8u &&
         snapshot.action_queue_count <= 4096u;
}

bool PositiveFiniteAccumulator(std::uint32_t bits) {
  const float value = std::bit_cast<float>(bits);
  return std::isfinite(value) && value > 0.0f && value <= 1.0f;
}

}  // namespace

StateMachine::StateMachine(std::int32_t owner_tid) : owner_tid_(owner_tid) {}

bool StateMachine::Reject(std::uint32_t reason, Report* report) {
  phase_ = Phase::kRejected;
  report->final_phase = static_cast<std::uint32_t>(phase_);
  report->reject_reasons |= reason;
  ++report->semantic_errors;
  return false;
}

bool StateMachine::QueueMatchesAnchor(const Snapshot& snapshot) const {
  return snapshot.action_queue_begin == anchor_.action_queue_begin &&
         snapshot.action_queue_end == anchor_.action_queue_end &&
         snapshot.action_queue_capacity == anchor_.action_queue_capacity &&
         snapshot.action_queue_count == anchor_.action_queue_count;
}

bool StateMachine::NitroMatchesAnchor(const Snapshot& snapshot) const {
  return snapshot.nitro_active == anchor_.nitro_active &&
         snapshot.nitro_mode == anchor_.nitro_mode;
}

bool StateMachine::Consume(const Event& event, Report* report) {
  if (report == nullptr || phase_ == Phase::kRejected ||
      phase_ == Phase::kPhaseProved)
    return false;
  if (event.snapshot.tid != owner_tid_)
    return Reject(kRejectWrongOwner, report);
  if (!QueueShapeValid(event.snapshot) || event.snapshot.nitro_active > 1u)
    return Reject(kRejectIdentity, report);
  if (event.snapshot.direct_mode != 0u)
    return Reject(kRejectDirectMode, report);
  if (event.payload_failures != 0)
    return Reject(kRejectPayloadEvidence, report);

  if (event.kind == EventKind::kActionQueueWrite) {
    ++report->action_queue_writes;
    return Reject(kRejectActionQueueWrite, report);
  }
  if (event.kind == EventKind::kNitroActiveWrite ||
      event.kind == EventKind::kNitroModeWrite) {
    ++report->nitro_writes;
    return Reject(kRejectNitroMutation, report);
  }

  switch (phase_) {
    case Phase::kWaitingDedicatedHit:
      if (event.kind != EventKind::kDedicatedHit)
        return Reject(kRejectWrongOrder, report);
      if ((event.snapshot.callback_flags & 0xFFu) != 1u)
        return Reject(kRejectCallbackState, report);
      if (event.snapshot.direct_mode != 0u)
        return Reject(kRejectDirectMode, report);
      if (event.snapshot.action_queue_count != 0u)
        return Reject(kRejectActionQueueNotEmpty, report);
      if (event.snapshot.nitro_active != 0u)
        return Reject(kRejectNitroNotIdle, report);
      if (event.snapshot.dedicated_entries != 1u ||
          event.dedicated_members_full != 1u)
        return Reject(kRejectPayloadEvidence | kRejectDedicatedMembership,
                      report);
      anchor_ = event.snapshot;
      report->dedicated = event.snapshot;
      ++report->dedicated_hits;
      report->flags |= kDedicatedHit | kCallbackOpenAtDedicated |
                       kDirectModeZero | kActionQueueEmpty | kNitroIdle;
      phase_ = Phase::kWaitingCallbackClose;
      break;

    case Phase::kWaitingCallbackClose:
      if (event.kind != EventKind::kCallbackClose)
        return Reject(kRejectWrongOrder, report);
      if ((event.snapshot.callback_flags & 0xFFu) != 0u)
        return Reject(kRejectCallbackState, report);
      if (!QueueMatchesAnchor(event.snapshot))
        return Reject(kRejectActionQueueWrite, report);
      if (!NitroMatchesAnchor(event.snapshot))
        return Reject(kRejectNitroMutation, report);
      report->callback_close = event.snapshot;
      ++report->callback_close_hits;
      report->flags |= kCallbackCloseSeen | kSameOwner |
                       kActionQueueUnchanged | kNitroUnchanged;
      phase_ = Phase::kWaitingNextAccumulator;
      break;

    case Phase::kWaitingNextAccumulator:
      if (event.kind != EventKind::kNextAccumulator)
        return Reject(kRejectWrongOrder, report);
      if ((event.snapshot.callback_flags & 0xFFu) != 0u)
        return Reject(kRejectCallbackState, report);
      if (!PositiveFiniteAccumulator(event.snapshot.accumulator_bits))
        return Reject(kRejectAccumulator, report);
      if (!QueueMatchesAnchor(event.snapshot))
        return Reject(kRejectActionQueueWrite, report);
      if (!NitroMatchesAnchor(event.snapshot))
        return Reject(kRejectNitroMutation, report);
      report->next_accumulator = event.snapshot;
      ++report->accumulator_hits;
      report->flags |= kNextAccumulatorSeen;
      phase_ = Phase::kWaitingNextCallbackOpen;
      break;

    case Phase::kWaitingNextCallbackOpen:
      if (event.kind != EventKind::kNextCallbackOpen)
        return Reject(kRejectWrongOrder, report);
      if ((event.snapshot.callback_flags & 0xFFu) != 1u)
        return Reject(kRejectCallbackState, report);
      if (!QueueMatchesAnchor(event.snapshot))
        return Reject(kRejectActionQueueWrite, report);
      if (!NitroMatchesAnchor(event.snapshot))
        return Reject(kRejectNitroMutation, report);
      if (event.dedicated_members_full != 0u)
        return Reject(kRejectDedicatedMembership, report);
      report->next_callback_open = event.snapshot;
      report->dedicated_members_final = event.dedicated_members_full;
      report->flags |=
          kNextCallbackOpenSeen | kDedicatedMembershipAbsent;
      phase_ = Phase::kPhaseProved;
      break;

    default:
      return Reject(kRejectWrongOrder, report);
  }

  report->final_phase = static_cast<std::uint32_t>(phase_);
  return true;
}

}  // namespace a9tas::fc3_replay_observer_v1

#ifndef A9TAS_FC3_PROTOCOL_NO_MAIN
#define A9TAS_FC3_PROTOCOL_NO_MAIN 0
#endif

#if A9TAS_FC3_PROTOCOL_NO_MAIN == 0

#ifndef A9TAS_FC3_PROTOCOL_SELFTEST
#define A9TAS_FC3_PROTOCOL_SELFTEST 0
#endif

#if A9TAS_FC3_PROTOCOL_SELFTEST == 1

namespace {

using namespace a9tas::fc3_replay_observer_v1;

Snapshot BaseSnapshot(std::int32_t tid, std::uint16_t flags) {
  Snapshot snapshot{};
  snapshot.monotonic_ns = 1;
  snapshot.tid = tid;
  snapshot.callback_flags = flags;
  snapshot.direct_mode = 0;
  snapshot.nitro_active = 0;
  snapshot.nitro_mode = 0;
  snapshot.accumulator_bits = std::bit_cast<std::uint32_t>(1.0f / 60.0f);
  snapshot.action_queue_begin = 0x1000;
  snapshot.action_queue_end = 0x1000;
  snapshot.action_queue_capacity = 0x1040;
  snapshot.action_queue_count = 0;
  snapshot.dedicated_entries = 1;
  return snapshot;
}

bool RunSuccess() {
  Report report{};
  StateMachine machine(77);
  Event event{EventKind::kDedicatedHit, BaseSnapshot(77, 1), 1, 0};
  if (!machine.Consume(event, &report)) return false;
  event.kind = EventKind::kCallbackClose;
  event.snapshot.callback_flags = 0;
  if (!machine.Consume(event, &report)) return false;
  event.kind = EventKind::kNextAccumulator;
  if (!machine.Consume(event, &report)) return false;
  event.kind = EventKind::kNextCallbackOpen;
  event.snapshot.callback_flags = 1;
  event.dedicated_members_full = 0;
  return machine.Consume(event, &report) && machine.phase_proved() &&
         report.reject_reasons == 0 && report.semantic_errors == 0 &&
         report.dedicated_hits == 1 && report.callback_close_hits == 1 &&
         report.accumulator_hits == 1 &&
         (report.flags & (kDedicatedHit | kCallbackCloseSeen |
                          kNextAccumulatorSeen | kNextCallbackOpenSeen |
                          kDedicatedMembershipAbsent)) ==
             (kDedicatedHit | kCallbackCloseSeen | kNextAccumulatorSeen |
              kNextCallbackOpenSeen | kDedicatedMembershipAbsent);
}

bool RejectsActionWrite() {
  Report report{};
  StateMachine machine(77);
  Event event{EventKind::kDedicatedHit, BaseSnapshot(77, 1), 1, 0};
  if (!machine.Consume(event, &report)) return false;
  event.kind = EventKind::kActionQueueWrite;
  return !machine.Consume(event, &report) && machine.rejected() &&
         (report.reject_reasons & kRejectActionQueueWrite) != 0;
}

bool RejectsWrongOrder() {
  Report report{};
  StateMachine machine(77);
  Event event{EventKind::kNextAccumulator, BaseSnapshot(77, 0), 0, 0};
  return !machine.Consume(event, &report) && machine.rejected() &&
         (report.reject_reasons & kRejectWrongOrder) != 0;
}

bool RejectsNitroMutation() {
  Report report{};
  StateMachine machine(77);
  Event event{EventKind::kDedicatedHit, BaseSnapshot(77, 1), 1, 0};
  if (!machine.Consume(event, &report)) return false;
  event.kind = EventKind::kCallbackClose;
  event.snapshot.callback_flags = 0;
  event.snapshot.nitro_mode = 1;
  return !machine.Consume(event, &report) && machine.rejected() &&
         (report.reject_reasons & kRejectNitroMutation) != 0;
}

bool RejectsPostGateDirectMode() {
  Report report{};
  StateMachine machine(77);
  Event event{EventKind::kDedicatedHit, BaseSnapshot(77, 1), 1, 0};
  if (!machine.Consume(event, &report)) return false;
  event.kind = EventKind::kCallbackClose;
  event.snapshot.callback_flags = 0;
  event.snapshot.direct_mode = 1;
  return !machine.Consume(event, &report) && machine.rejected() &&
         (report.reject_reasons & kRejectDirectMode) != 0;
}

}  // namespace

int main() {
  const bool success = RunSuccess();
  const bool action_reject = RejectsActionWrite();
  const bool order_reject = RejectsWrongOrder();
  const bool nitro_reject = RejectsNitroMutation();
  const bool direct_reject = RejectsPostGateDirectMode();
  std::printf(
      "FC3_PROTOCOL_SELFTEST success=%u action_reject=%u order_reject=%u "
      "nitro_reject=%u direct_reject=%u device_access=0 game_writes=0 "
      "action_calls=0\n",
      success, action_reject, order_reject, nitro_reject, direct_reject);
  return success && action_reject && order_reject && nitro_reject &&
                 direct_reject
             ? 0
             : 1;
}

#else

int main() {
  std::puts("FC3_PROTOCOL_BUILD_ONLY runtime=disabled return=-100 "
            "device_access=0 game_writes=0 action_calls=0 nitro_writes=0");
  return 100;
}

#endif

#endif  // A9TAS_FC3_PROTOCOL_NO_MAIN == 0
