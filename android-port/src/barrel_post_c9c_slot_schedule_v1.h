#pragma once

// Transport-independent four-slot schedule after the certified C9C input
// boundary. RBX and BarrelYaw are independent conditional calls in the Android
// vehicle update, so they are orthogonal sub-states. During C9C-to-F64 DR0
// walks the two RBX fields while DR1 observes angular aux. At F64 the host
// verifies callback dispatch is still open and restores DR1 to callback flags
// before resuming. DR2/DR3 remain F64/world in the active window.

#include <cstdint>

#include "barrel_yaw_tail_payload_protocol_v1.h"

namespace a9tas::barrel_post_c9c_schedule_v1 {

inline constexpr std::uint32_t kMaximumTransactionsPerFrame =
    a9tas::barrel_yaw_tail_payload_v1::kMaximumTransactionsPerFrame;

enum class Stage : std::uint32_t {
  kAwaitCertifiedC9C = 0,
  kActiveWindow = 1,
  kAwaitWorldCommit = 2,
  kComplete = 3,
  kFaulted = 4,
};

enum class RbxPhase : std::uint32_t {
  kAwaitFirst = 0,
  kAwaitSecond = 1,
};

enum class Event : std::uint32_t {
  kCompletionStore = 0,
  kCertifiedC9C = 1,
  kRbxFirstStore = 2,
  kRbxSecondStore = 3,
  kAngularSetterCertificate = 4,
  kAngularPayloadComplete = 5,
  kAngularAbsentCancelled = 6,
  kAngularIdleReceiptAtF64 = 7,
  kF64 = 8,
  kWorldCommit = 9,
};

enum Action : std::uint32_t {
  kActionNone = 0,
  kActionInstallParallelWatchSet = 1u << 0,
  kActionSwapRbxFirstForSecond = 1u << 1,
  kActionApplyRbxTail = 1u << 2,
  kActionResetRbxFirstWatch = 1u << 3,
  // Host must publish the unique token first and the transient shadow vptr
  // last while the certified owner is stopped.  Token-only publication is not
  // a legal implementation of this action.
  kActionArmAngularTransientWindow = 1u << 4,
  kActionVerifyBarrelTerminal = 1u << 5,
  kActionRestorePostF64WatchSet = 1u << 6,
  kActionCommitFrame = 1u << 7,
};

struct State {
  Stage stage{Stage::kAwaitCertifiedC9C};
  RbxPhase rbx_phase{RbxPhase::kAwaitFirst};
  bool completion_seen{};
  bool angular_token_armed{};
  bool angular_idle_receipt{};
  std::uint32_t rbx_pairs{};
  std::uint32_t angular_transactions{};
  std::uint32_t faults{};
};

inline bool Fault(State* state) {
  state->stage = Stage::kFaulted;
  ++state->faults;
  return false;
}

inline bool Advance(State* state, Event event, std::uint32_t* actions) {
  if (state == nullptr || actions == nullptr) return false;
  *actions = kActionNone;
  if (state->stage == Stage::kFaulted || state->stage == Stage::kComplete)
    return false;

  if (state->stage == Stage::kAwaitCertifiedC9C) {
    if (event == Event::kCompletionStore) {
      if (state->completion_seen) return Fault(state);
      state->completion_seen = true;
      return true;
    }
    if (event != Event::kCertifiedC9C || !state->completion_seen)
      return Fault(state);
    state->stage = Stage::kActiveWindow;
    state->rbx_phase = RbxPhase::kAwaitFirst;
    *actions = kActionInstallParallelWatchSet;
    return true;
  }

  if (state->stage == Stage::kActiveWindow) {
    if (event == Event::kRbxFirstStore) {
      if (state->rbx_phase != RbxPhase::kAwaitFirst ||
          state->rbx_pairs >= kMaximumTransactionsPerFrame)
        return Fault(state);
      state->rbx_phase = RbxPhase::kAwaitSecond;
      *actions = kActionSwapRbxFirstForSecond;
      return true;
    }
    if (event == Event::kRbxSecondStore) {
      if (state->rbx_phase != RbxPhase::kAwaitSecond ||
          state->rbx_pairs >= kMaximumTransactionsPerFrame)
        return Fault(state);
      state->rbx_phase = RbxPhase::kAwaitFirst;
      ++state->rbx_pairs;
      *actions = kActionApplyRbxTail | kActionResetRbxFirstWatch;
      return true;
    }
    if (event == Event::kAngularSetterCertificate) {
      if (state->angular_token_armed || state->angular_idle_receipt ||
          state->angular_transactions >= kMaximumTransactionsPerFrame)
        return Fault(state);
      state->angular_token_armed = true;
      *actions = kActionArmAngularTransientWindow;
      return true;
    }
    if (event == Event::kAngularPayloadComplete) {
      if (!state->angular_token_armed)
        return Fault(state);
      state->angular_token_armed = false;
      ++state->angular_transactions;
      return true;
    }
    if (event == Event::kAngularAbsentCancelled) {
      if (!state->angular_token_armed)
        return Fault(state);
      state->angular_token_armed = false;
      return true;
    }
    if (event == Event::kAngularIdleReceiptAtF64) {
      if (state->angular_token_armed || state->angular_idle_receipt)
        return Fault(state);
      state->angular_idle_receipt = true;
      return true;
    }
    if (event == Event::kF64) {
      if (state->rbx_phase == RbxPhase::kAwaitSecond ||
          state->angular_token_armed || !state->angular_idle_receipt)
        return Fault(state);
      state->stage = Stage::kAwaitWorldCommit;
      *actions = kActionVerifyBarrelTerminal |
                 kActionRestorePostF64WatchSet;
      return true;
    }
    return Fault(state);
  }

  if (state->stage == Stage::kAwaitWorldCommit &&
      event == Event::kWorldCommit) {
    state->stage = Stage::kComplete;
    *actions = kActionCommitFrame;
    return true;
  }
  return Fault(state);
}

inline bool Terminal(const State& state) {
  return state.stage == Stage::kComplete && state.completion_seen &&
         state.rbx_phase == RbxPhase::kAwaitFirst &&
         !state.angular_token_armed &&
         state.angular_idle_receipt &&
         state.rbx_pairs <= kMaximumTransactionsPerFrame &&
         state.angular_transactions <= kMaximumTransactionsPerFrame &&
         state.faults == 0;
}

// Composition guard for the live-proven unified executor.  The barrel
// sub-machine may be terminal at F64, but the frame is not commit-eligible
// until the original main machine has observed callback close and deferred
// callback clear in that order.
enum class MainStage : std::uint32_t {
  kAwaitCompletion = 0,
  kAwaitC9C = 1,
  kActivePostC9C = 2,
  kAwaitCallbackClose = 3,
  kAwaitDeferredClear = 4,
  kAwaitWorldCommit = 5,
  kComplete = 6,
  kFaulted = 7,
};

enum class InterlockedEvent : std::uint32_t {
  kCompletionStore = 0,
  kCertifiedC9C = 1,
  kRbxFirstStore = 2,
  kRbxSecondStore = 3,
  kAngularSetterCertificate = 4,
  kAngularPayloadComplete = 5,
  kAngularAbsentCancelled = 6,
  kAngularTerminalReceipt = 7,
  kF64 = 8,
  kCallbackClose = 9,
  kDeferredCallbackClear = 10,
  kWorldCommit = 11,
};

struct InterlockedState {
  State barrel{};
  MainStage main{MainStage::kAwaitCompletion};
  std::uint32_t faults{};
};

inline bool InterlockedFault(InterlockedState* state) {
  if (state == nullptr) return false;
  state->main = MainStage::kFaulted;
  ++state->faults;
  if (state->barrel.stage != Stage::kFaulted &&
      state->barrel.stage != Stage::kComplete)
    Fault(&state->barrel);
  return false;
}

inline bool AdvanceInterlocked(InterlockedState* state,
                               InterlockedEvent event,
                               std::uint32_t* actions) {
  if (state == nullptr || actions == nullptr ||
      state->main == MainStage::kFaulted ||
      state->main == MainStage::kComplete)
    return false;
  if (event == InterlockedEvent::kCompletionStore) {
    if (state->main != MainStage::kAwaitCompletion ||
        !Advance(&state->barrel, Event::kCompletionStore, actions))
      return InterlockedFault(state);
    state->main = MainStage::kAwaitC9C;
    return true;
  }
  if (event == InterlockedEvent::kCertifiedC9C) {
    if (state->main != MainStage::kAwaitC9C ||
        !Advance(&state->barrel, Event::kCertifiedC9C, actions))
      return InterlockedFault(state);
    state->main = MainStage::kActivePostC9C;
    return true;
  }
  if (state->main == MainStage::kActivePostC9C) {
    Event barrel_event{};
    switch (event) {
      case InterlockedEvent::kRbxFirstStore:
        barrel_event = Event::kRbxFirstStore;
        break;
      case InterlockedEvent::kRbxSecondStore:
        barrel_event = Event::kRbxSecondStore;
        break;
      case InterlockedEvent::kAngularSetterCertificate:
        barrel_event = Event::kAngularSetterCertificate;
        break;
      case InterlockedEvent::kAngularPayloadComplete:
        barrel_event = Event::kAngularPayloadComplete;
        break;
      case InterlockedEvent::kAngularAbsentCancelled:
        barrel_event = Event::kAngularAbsentCancelled;
        break;
      case InterlockedEvent::kAngularTerminalReceipt:
        barrel_event = Event::kAngularIdleReceiptAtF64;
        break;
      case InterlockedEvent::kF64:
        barrel_event = Event::kF64;
        break;
      default:
        return InterlockedFault(state);
    }
    if (!Advance(&state->barrel, barrel_event, actions))
      return InterlockedFault(state);
    if (event == InterlockedEvent::kF64)
      state->main = MainStage::kAwaitCallbackClose;
    return true;
  }
  if (event == InterlockedEvent::kCallbackClose &&
      state->main == MainStage::kAwaitCallbackClose) {
    *actions = kActionNone;
    state->main = MainStage::kAwaitDeferredClear;
    return true;
  }
  if (event == InterlockedEvent::kDeferredCallbackClear &&
      state->main == MainStage::kAwaitDeferredClear) {
    *actions = kActionNone;
    state->main = MainStage::kAwaitWorldCommit;
    return true;
  }
  if (event == InterlockedEvent::kWorldCommit &&
      state->main == MainStage::kAwaitWorldCommit &&
      Advance(&state->barrel, Event::kWorldCommit, actions)) {
    state->main = MainStage::kComplete;
    return true;
  }
  return InterlockedFault(state);
}

inline bool InterlockedTerminal(const InterlockedState& state) {
  return state.main == MainStage::kComplete && state.faults == 0 &&
         Terminal(state.barrel);
}

}  // namespace a9tas::barrel_post_c9c_schedule_v1
