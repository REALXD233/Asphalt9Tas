#include "fc3_phase_map_v1.h"

#include <cmath>
#include <cstring>

namespace a9tas::fc3_phase_map_v1 {

namespace {

bool FiniteFloatBits(std::uint32_t bits, float absolute_limit) {
  float value = 0.0f;
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&value, &bits, sizeof(value));
  return std::isfinite(value) && std::fabs(value) <= absolute_limit;
}

}  // namespace

Core::Core(std::int32_t callback_owner_tid)
    : callback_owner_tid_(callback_owner_tid) {}

Decision Core::Reject(Report* report, std::uint32_t reason) {
  if (report != nullptr) {
    report->reject_reasons |= reason;
    ++report->semantic_errors;
    report->final_phase = static_cast<std::uint32_t>(Phase::kRejected);
  }
  phase_ = Phase::kRejected;
  return Decision::kReject;
}

Decision Core::Consume(const Event& event, Report* report) {
  if (report == nullptr) return Reject(nullptr, kRejectNullReport);
  if (phase_ == Phase::kRejected || phase_ == Phase::kProved)
    return Reject(report, kRejectDuplicateOrLateEvent);

  switch (phase_) {
    case Phase::kWaitingBootstrapClose:
      // DT/C98 can still be observed after the open event at which FC-2
      // installs the watch plan.  They belong to the unnumbered prefix before
      // this proof's close anchor and must never be counted as cycle k.
      if (event.kind == EventKind::kDelta) {
        if (++report->pre_anchor_delta_hits > 16)
          return Reject(report, kRejectDuplicateOrLateEvent);
        return Decision::kContinue;
      }
      if (event.kind == EventKind::kC98) {
        if (++report->pre_anchor_c98_hits > 16)
          return Reject(report, kRejectDuplicateOrLateEvent);
        return Decision::kContinue;
      }
      if (event.kind != EventKind::kCallbackFlags)
        return Reject(report, kRejectWrongOrder);
      ++report->callback_flag_hits;
      if (event.tid != callback_owner_tid_)
        return Reject(report, kRejectWrongCallbackValue);
      // DR0 covers both callback-list bytes.  A 0x0101 observation is the
      // deferred-registration flag changing while dispatch remains open; it
      // is not a phase boundary.
      if ((event.callback_flags & 0xFFu) == 1u &&
          (event.callback_flags >> 8) == 1u)
        return Decision::kContinue;
      if ((event.callback_flags & 0xFFu) != 0u)
        return Reject(report, kRejectWrongCallbackValue);
      report->bootstrap_close_ns = event.monotonic_ns;
      report->flags |= kBootstrapCloseSeen;
      phase_ = Phase::kWaitingDelta;
      break;

    case Phase::kWaitingDelta:
      if (event.kind == EventKind::kCallbackFlags) {
        ++report->callback_flag_hits;
        // Deferred compaction is ordered after callback close and before the
        // following DT.  It may change 0x0100 to 0x0000 without reopening.
        if (event.tid == callback_owner_tid_ &&
            (event.callback_flags & 0xFFu) == 0u)
          return Decision::kContinue;
        return Reject(report, kRejectWrongCallbackValue);
      }
      if (event.kind != EventKind::kDelta)
        return Reject(report, kRejectWrongOrder);
      ++report->delta_hits;
      report->delta_ns = event.monotonic_ns;
      report->delta_tid = event.tid;
      report->flags |= kDeltaSeen;
      phase_ = Phase::kWaitingC98;
      break;

    case Phase::kWaitingC98:
      if (event.kind != EventKind::kC98)
        return Reject(report, kRejectWrongOrder);
      ++report->c98_hits;
      report->c98_ns = event.monotonic_ns;
      report->c98_tid = event.tid;
      report->flags |= kC98Seen;
      phase_ = Phase::kWaitingCallbackOpen;
      break;

    case Phase::kWaitingCallbackOpen:
      if (event.kind != EventKind::kCallbackFlags)
        return Reject(report, kRejectWrongOrder);
      ++report->callback_flag_hits;
      if (event.tid != callback_owner_tid_ ||
          (event.callback_flags & 0xFFu) != 1u)
        return Reject(report, kRejectWrongCallbackValue);
      if (event.car_callback_index == UINT32_MAX ||
          event.dedicated_callback_index == UINT32_MAX ||
          event.car_callback_index >= event.dedicated_callback_index)
        return Reject(report, kRejectCallbackListOrder);
      report->callback_open_ns = event.monotonic_ns;
      report->car_callback_index = event.car_callback_index;
      report->dedicated_callback_index = event.dedicated_callback_index;
      report->flags |= kCallbackOpenSeen | kDedicatedAfterCar;
      phase_ = Phase::kWaitingC9C;
      report->final_phase = static_cast<std::uint32_t>(phase_);
      return Decision::kRearmC9C;

    case Phase::kWaitingC9C:
      if (event.kind != EventKind::kC9C)
        return Reject(report, kRejectWrongOrder);
      ++report->c9c_hits;
      report->c9c_ns = event.monotonic_ns;
      report->c9c_tid = event.tid;
      report->flags |= kC9CSeen;
      phase_ = Phase::kWaitingF64;
      report->final_phase = static_cast<std::uint32_t>(phase_);
      return Decision::kRearmF64;

    case Phase::kWaitingF64:
      if (event.kind != EventKind::kF64)
        return Reject(report, kRejectWrongOrder);
      if (event.tid != callback_owner_tid_)
        return Reject(report, kRejectWrongPhaseThread);
      if (!FiniteFloatBits(event.observed_bits, 1000000.0f))
        return Reject(report, kRejectInvalidObservedValue);
      ++report->f64_hits;
      report->f64_ns = event.monotonic_ns;
      report->f64_tid = event.tid;
      report->f64_bits = event.observed_bits;
      report->flags |= kF64Seen;
      phase_ = Phase::kWaitingDedicated;
      break;

    case Phase::kWaitingDedicated:
      if (event.kind != EventKind::kDedicated)
        return Reject(report, kRejectWrongOrder);
      ++report->dedicated_hits;
      if (event.tid != callback_owner_tid_)
        return Reject(report, kRejectWrongDedicatedThread);
      report->dedicated_ns = event.monotonic_ns;
      report->flags |= kDedicatedSeen;
      phase_ = Phase::kWaitingCallbackClose;
      report->final_phase = static_cast<std::uint32_t>(phase_);
      return Decision::kRearmWorldCommit;

    case Phase::kWaitingCallbackClose:
      if (event.kind != EventKind::kCallbackFlags)
        return Reject(report, kRejectWrongOrder);
      ++report->callback_flag_hits;
      if (event.tid != callback_owner_tid_)
        return Reject(report, kRejectWrongCallbackValue);
      // Removal marks the list deferred while dispatch remains open.
      if ((event.callback_flags & 0xFFu) == 1u &&
          (event.callback_flags >> 8) == 1u)
        return Decision::kContinue;
      if ((event.callback_flags & 0xFFu) != 0u)
        return Reject(report, kRejectWrongCallbackValue);
      report->callback_close_ns = event.monotonic_ns;
      report->flags |= kCallbackCloseSeen;
      phase_ = Phase::kWaitingWorldCommit;
      break;

    case Phase::kWaitingWorldCommit:
      if (event.kind == EventKind::kCallbackFlags) {
        ++report->callback_flag_hits;
        if (event.tid == callback_owner_tid_ &&
            event.callback_flags == 0u) {
          post_close_deferred_clear_seen_ = true;
          report->flags |= kDeferredClearSeen;
          return Decision::kContinue;
        }
        return Reject(report, kRejectWrongCallbackValue);
      }
      if (event.kind != EventKind::kWorldCommit)
        return Reject(report, kRejectWrongOrder);
      if (!post_close_deferred_clear_seen_)
        return Reject(report, kRejectMissingDeferredClear);
      if (event.tid == callback_owner_tid_)
        return Reject(report, kRejectWrongPhaseThread);
      if (!FiniteFloatBits(event.observed_bits, 60.0f))
        return Reject(report, kRejectInvalidObservedValue);
      ++report->world_commit_hits;
      report->world_commit_ns = event.monotonic_ns;
      report->world_commit_tid = event.tid;
      report->world_commit_bits = event.observed_bits;
      report->flags |= kWorldCommitSeen;
      phase_ = Phase::kWaitingNextDelta;
      report->final_phase = static_cast<std::uint32_t>(phase_);
      return Decision::kRearmNextCycle;

    case Phase::kWaitingNextDelta:
      if (event.kind == EventKind::kCallbackFlags) {
        ++report->callback_flag_hits;
        if (event.tid == callback_owner_tid_ &&
            (event.callback_flags & 0xFFu) == 0u)
          return Decision::kContinue;
        return Reject(report, kRejectWrongCallbackValue);
      }
      if (event.kind != EventKind::kDelta)
        return Reject(report, kRejectWrongOrder);
      ++report->delta_hits;
      report->next_delta_ns = event.monotonic_ns;
      report->next_delta_tid = event.tid;
      report->flags |= kNextDeltaSeen;
      phase_ = Phase::kWaitingNextC98;
      break;

    case Phase::kWaitingNextC98:
      if (event.kind != EventKind::kC98)
        return Reject(report, kRejectWrongOrder);
      ++report->c98_hits;
      report->next_c98_ns = event.monotonic_ns;
      report->next_c98_tid = event.tid;
      report->flags |= kNextC98Seen | kPhaseOrderProved;
      phase_ = Phase::kProved;
      report->final_phase = static_cast<std::uint32_t>(phase_);
      return Decision::kProved;

    default:
      return Reject(report, kRejectDuplicateOrLateEvent);
  }

  report->final_phase = static_cast<std::uint32_t>(phase_);
  return Decision::kContinue;
}

}  // namespace a9tas::fc3_phase_map_v1
