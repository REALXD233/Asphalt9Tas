// Build-only ARM64 payload for the first in-process executor migration.
//
// The host may copy the exact GameplayInputController primary vtable group
// into g_controller_shadow, replace only slot 14 with ControllerTickWrapper,
// and atomically redirect one identity-checked controller object to that
// shadow.  No game instruction, executable page or global vtable is modified.
//
// The wrapper publishes the matching natural-action command, arms the existing
// final-writer permit, temporarily substitutes a tiny input source for the
// duration of the intact UpdatePerTick call, and restores the real source
// before returning.  Cursor advancement occurs only on the next game-owned
// tick after both downstream receipts exist.
//
// This file has no installer.  The arm export always returns -100.  Runtime
// deployment requires a separately reviewed host transaction and live Gates.

#include "controller_shadow_coordinator_protocol_v1.h"
#include "in_process_tick_coordinator_v1.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>

namespace coordinator = a9tas::in_process_tick_coordinator_v1;
namespace protocol = a9tas::controller_shadow_coordinator_v1;
namespace recording = a9tas::unified_tick_v1;
namespace writer = a9tas::final_writer_replay_v1;
namespace mailbox = a9tas::natural_action_callback_v1;

namespace {

using protocol::Control;
using protocol::Evidence;
using protocol::Status;
using namespace protocol;

struct ProxySource {
  std::uintptr_t vptr;
  std::uintptr_t original_source;
  std::uint32_t steering_bits;
  std::uint32_t brake_bits;
  std::uint64_t reserved;
};

static_assert(sizeof(ProxySource) == 32, "proxy source ABI");

using UpdatePerTick = std::int64_t (*)(void*, const std::uint64_t*);

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
ProxyGetValueC() {
  // This component is explicitly skipped by the current A9UTK1 capability
  // mask.  Preserve the live game value by tail-calling the same +0x10
  // virtual getter on the identity-checked real input source.  X8 (the
  // aggregate-result pointer) is deliberately left intact.
  __asm__ volatile("ldr x0, [x0, #8]\n"
                   "ldr x9, [x0]\n"
                   "ldr x9, [x9, #16]\n"
                   "br x9\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
ProxyGetSteering() {
  __asm__ volatile("ldr w9, [x0, #16]\n"
                   "str w9, [x8]\n"
                   "ret\n");
}

extern "C" __attribute__((naked, noinline, visibility("hidden"))) void
ProxyGetBrake() {
  __asm__ volatile("ldr w9, [x0, #20]\n"
                   "str w9, [x8]\n"
                   "ret\n");
}

extern "C" __attribute__((noinline, visibility("hidden"))) void
ProxyNoop(void*) {}

alignas(64) std::uintptr_t g_proxy_vtable[5] = {
    reinterpret_cast<std::uintptr_t>(&ProxyNoop),
    reinterpret_cast<std::uintptr_t>(&ProxyNoop),
    reinterpret_cast<std::uintptr_t>(&ProxyGetValueC),
    reinterpret_cast<std::uintptr_t>(&ProxyGetSteering),
    reinterpret_cast<std::uintptr_t>(&ProxyGetBrake),
};
alignas(64) ProxySource g_proxy{};
alignas(64) std::uint8_t g_controller_shadow[kControllerShadowSize] = {0xA9};
alignas(64) recording::RecordingFrameV1 g_frames[kMaximumFrames]{};
alignas(64) Control g_control = {
    .magic = {kControlMagic[0], kControlMagic[1], kControlMagic[2],
              kControlMagic[3], kControlMagic[4], kControlMagic[5],
              kControlMagic[6], kControlMagic[7]},
    .version = kProtocolVersion,
    .size = sizeof(Control),
    .flags = 0,
    .frame_count = 0,
    .expected_controller = 0,
    .original_controller_vptr = 0,
    .shadow_controller_vptr = 0,
    .original_update = 0,
    .expected_source_vptr = 0,
    .writer_control = 0,
    .writer_evidence = 0,
    .action_mailbox = 0,
    .vehicle_owner = 0,
    .session_id = 0,
    .producer_tid = 0,
    .recording_sha256 = {},
    .reserved = {},
};
alignas(64) Evidence g_evidence = {
    .magic = {kEvidenceMagic[0], kEvidenceMagic[1], kEvidenceMagic[2],
              kEvidenceMagic[3], kEvidenceMagic[4], kEvidenceMagic[5],
              kEvidenceMagic[6], kEvidenceMagic[7]},
    .version = kProtocolVersion,
    .size = sizeof(Evidence),
    .wrapper_entries = 0,
    .original_calls = 0,
    .original_returns = 0,
    .source_swaps = 0,
    .source_restores = 0,
    .selected_frames = 0,
    .completed_frames = 0,
    .failures = 0,
    .recursive_entries = 0,
    .last_controller = 0,
    .last_frame_token = 0,
    .last_source = 0,
    .observed_controller_vptr = 0,
    .observed_source_vptr = 0,
    .last_selected_frame = 0,
    .last_tid = 0,
    .last_status = 0,
    .last_coordinator_result = 0,
    .coordinator_phase = 0,
    .next_frame = 0,
    .reserved = {},
};
coordinator::State g_state{};
std::atomic<std::uint32_t> g_wrapper_depth{0};

template <typename T>
T Load(const T* value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

template <typename T>
void Store(T* target, T value) {
  __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

void Add(std::uint64_t* value) {
  __atomic_add_fetch(value, 1u, __ATOMIC_RELAXED);
}

bool HashPresent(const std::uint8_t hash[32]) {
  std::uint8_t combined = 0;
  for (std::size_t index = 0; index < 32; ++index) combined |= hash[index];
  return combined != 0;
}

bool ControlValid(const Control& control) {
  if (std::memcmp(control.magic, kControlMagic, sizeof(kControlMagic)) != 0 ||
      control.version != kProtocolVersion || control.size != sizeof(Control) ||
      control.flags != (kConfigured | kFramesLoaded) ||
      control.frame_count == 0 || control.frame_count > kMaximumFrames ||
      control.expected_controller == 0 ||
      control.original_controller_vptr < kControllerAddressPointRva ||
      control.shadow_controller_vptr !=
          reinterpret_cast<std::uintptr_t>(g_controller_shadow) +
              kControllerPrefixSize ||
      control.original_update < kControllerUpdateRva ||
      control.expected_source_vptr < kKeyboardSourceAddressPointRva ||
      control.writer_control == 0 || control.writer_evidence == 0 ||
      control.action_mailbox == 0 || control.vehicle_owner == 0 ||
      control.session_id == 0 || control.producer_tid == 0 ||
      !HashPresent(control.recording_sha256) || control.reserved[0] != 0 ||
      control.reserved[1] != 0)
    return false;
  const std::uintptr_t base =
      control.original_controller_vptr - kControllerAddressPointRva;
  return control.original_update == base + kControllerUpdateRva &&
         control.expected_source_vptr ==
             base + kKeyboardSourceAddressPointRva;
}

bool RestoreControllerVptr(void* controller, const Control& control) {
  if (controller == nullptr) return false;
  auto* slot = reinterpret_cast<std::uintptr_t*>(controller);
  const std::uintptr_t observed = Load(slot);
  if (observed == control.original_controller_vptr) return true;
  if (observed != control.shadow_controller_vptr) return false;
  Store(slot, control.original_controller_vptr);
  return Load(slot) == control.original_controller_vptr;
}

void Fail(void* controller, const Control& control, Status status,
          coordinator::Result result = coordinator::Result::kInvalidArgument) {
  Add(&g_evidence.failures);
  g_evidence.last_coordinator_result = static_cast<std::int32_t>(result);
  Store(&g_evidence.last_status, static_cast<std::int32_t>(status));
  (void)RestoreControllerVptr(controller, control);
}

coordinator::Config BuildCoordinatorConfig(const Control& control) {
  return {control.frame_count,
          control.session_id,
          control.vehicle_owner,
          control.producer_tid,
          g_frames,
          reinterpret_cast<writer::Control*>(control.writer_control),
          reinterpret_cast<const writer::Evidence*>(control.writer_evidence),
          reinterpret_cast<mailbox::Mailbox*>(control.action_mailbox)};
}

}  // namespace

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_controller_tick_wrapper_v1(void* controller,
                                 const std::uint64_t* frame_token) {
  const Control control = g_control;
  if (!ControlValid(control) || controller == nullptr) {
    Fail(controller, control, kInvalidControl);
    return 0;
  }
  if (reinterpret_cast<std::uintptr_t>(controller) !=
      control.expected_controller) {
    Fail(controller, control, kUnexpectedObject);
    return 0;
  }
  auto* controller_vptr = reinterpret_cast<std::uintptr_t*>(controller);
  const std::uintptr_t observed_controller_vptr = Load(controller_vptr);
  g_evidence.observed_controller_vptr = observed_controller_vptr;
  if (observed_controller_vptr != control.shadow_controller_vptr) {
    Fail(controller, control, kUnexpectedVptr);
    return 0;
  }
  const auto original =
      reinterpret_cast<UpdatePerTick>(control.original_update);

  const std::uint32_t previous_depth =
      g_wrapper_depth.fetch_add(1u, std::memory_order_acq_rel);
  if (previous_depth != 0) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Add(&g_evidence.recursive_entries);
    Fail(controller, control, kRecursiveEntry);
    return original(controller, frame_token);
  }
  Add(&g_evidence.wrapper_entries);
  g_evidence.last_controller = reinterpret_cast<std::uintptr_t>(controller);
  g_evidence.last_frame_token =
      reinterpret_cast<std::uintptr_t>(frame_token);
  const std::uint32_t tid =
      static_cast<std::uint32_t>(syscall(__NR_gettid));
  g_evidence.last_tid = tid;
  if (tid != control.producer_tid) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(controller, control, kWrongThread);
    return original(controller, frame_token);
  }

  auto** source_slot = reinterpret_cast<void**>(
      reinterpret_cast<std::uintptr_t>(controller) + kControllerSourceOffset);
  void* original_source = Load(source_slot);
  if (original_source == nullptr) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(controller, control, kSourceIdentityMismatch);
    return original(controller, frame_token);
  }
  const std::uintptr_t observed_source_vptr =
      Load(reinterpret_cast<std::uintptr_t*>(original_source));
  g_evidence.last_source = reinterpret_cast<std::uintptr_t>(original_source);
  g_evidence.observed_source_vptr = observed_source_vptr;
  if (observed_source_vptr != control.expected_source_vptr) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(controller, control, kSourceIdentityMismatch);
    return original(controller, frame_token);
  }

  // Validate the frame that BeginTick would publish before crossing the
  // mailbox selector publication edge.  An in-flight state commits the
  // previous frame first, so its prospective selection is next_index + 1.
  const std::uint32_t prospective =
      g_state.next_index +
      (g_state.phase == coordinator::Phase::kInFlight ? 1u : 0u);
  if (prospective < control.frame_count &&
      !protocol::FrameInputSupported(g_frames[prospective], prospective)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(controller, control, kCoordinatorFailure,
         coordinator::Result::kFrameInvalid);
    return original(controller, frame_token);
  }

  std::uint32_t selected = 0;
  const coordinator::Result selected_result = coordinator::BeginTick(
      BuildCoordinatorConfig(control), &g_state, tid, &selected);
  g_evidence.last_coordinator_result =
      static_cast<std::int32_t>(selected_result);
  g_evidence.coordinator_phase = static_cast<std::uint32_t>(g_state.phase);
  g_evidence.next_frame = g_state.next_index;
  if (selected_result == coordinator::Result::kComplete) {
    g_evidence.completed_frames = control.frame_count;
    const bool restored = RestoreControllerVptr(controller, control);
    Store(&g_evidence.last_status,
          static_cast<std::int32_t>(restored ? kComplete
                                             : kUnexpectedVptr));
    if (!restored) Add(&g_evidence.failures);
    Add(&g_evidence.original_calls);
    const std::int64_t result = original(controller, frame_token);
    Add(&g_evidence.original_returns);
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    return result;
  }
  if (selected_result != coordinator::Result::kFrameSelected ||
      selected >= control.frame_count ||
      !protocol::FrameInputSupported(g_frames[selected], selected)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(controller, control, kCoordinatorFailure, selected_result);
    return original(controller, frame_token);
  }

  const recording::RecordingFrameV1& frame = g_frames[selected];
  g_proxy.vptr = reinterpret_cast<std::uintptr_t>(g_proxy_vtable);
  g_proxy.original_source = reinterpret_cast<std::uintptr_t>(original_source);
  std::memcpy(&g_proxy.steering_bits, &frame.steering,
              sizeof(g_proxy.steering_bits));
  std::memcpy(&g_proxy.brake_bits, &frame.brake,
              sizeof(g_proxy.brake_bits));
  g_proxy.reserved = 0;

  Store(source_slot, static_cast<void*>(&g_proxy));
  if (Load(source_slot) != &g_proxy) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(controller, control, kSourceSwapFailure);
    return original(controller, frame_token);
  }
  Add(&g_evidence.source_swaps);
  Add(&g_evidence.selected_frames);
  g_evidence.last_selected_frame = selected;
  Store(&g_evidence.last_status, static_cast<std::int32_t>(kRunning));

  Add(&g_evidence.original_calls);
  const std::int64_t result = original(controller, frame_token);
  Add(&g_evidence.original_returns);

  if (Load(source_slot) != &g_proxy) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(controller, control, kSourceSwapFailure);
    return result;
  }
  Store(source_slot, original_source);
  if (Load(source_slot) != original_source) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(controller, control, kSourceSwapFailure);
    return result;
  }
  Add(&g_evidence.source_restores);
  g_evidence.completed_frames = g_state.next_index;
  g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
  return result;
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_controller_shadow_coordinator_arm_v1() {
  return kRejectedBuildOnly;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_controller_shadow_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_controller_shadow);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_controller_shadow_control_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_control);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_controller_shadow_evidence_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_evidence);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_controller_shadow_frames_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_frames);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_controller_shadow_storage_size_v1 = sizeof(g_controller_shadow);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_controller_shadow_control_size_v1 = sizeof(g_control);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_controller_shadow_evidence_size_v1 = sizeof(g_evidence);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_controller_shadow_frame_size_v1 = sizeof(g_frames[0]);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_controller_shadow_frame_capacity_v1 = kMaximumFrames;
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_controller_shadow_prefix_size_v1 = kControllerPrefixSize;
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_controller_shadow_update_slot_v1 = kControllerUpdateSlotOffset;
