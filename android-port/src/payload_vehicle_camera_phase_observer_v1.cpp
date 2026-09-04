// Build-only dual-callback phase observer linked beside the immutable,
// hash-pinned final-writer payload. The host installs the outer vehicle
// wrapper in the final-writer shadow vtable and the camera wrapper in the
// proven RaceView callback node. Both wrappers publish into one monotonic
// event stream; the camera path is observation-only.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "camera_raceview_callback_node_v1.h"
#include "final_writer_replay_protocol_v1.h"
#include "vehicle_camera_phase_observer_protocol_v1.h"

namespace phase = a9tas::vehicle_camera_phase_observer_v1;
namespace camera_node = a9tas::camera_raceview_callback_node_v1;
namespace final_writer = a9tas::final_writer_replay_v1;

extern "C" std::int64_t a9tas_final_writer_replay_callback_v1(
    void* object, const std::uint64_t* frame_token);
extern "C" std::uintptr_t
    a9tas_final_writer_replay_control_storage_data_v1;
extern "C" std::uintptr_t
    a9tas_final_writer_replay_audit_storage_data_v1;
extern "C" std::uintptr_t
    a9tas_final_writer_replay_evidence_storage_data_v1;

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_vehicle_camera_phase_vehicle_callback_v1(
    void* object, const std::uint64_t* frame_token);
extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_vehicle_camera_phase_camera_callback_v1(void* manager);

namespace {

using CameraCallback = void (*)(void*);

constexpr std::size_t kManagerWorldOffset = 0x2C;
constexpr std::size_t kManagerShapeOffset = 0xE8;
constexpr std::size_t kManagerFovOffset = 0x108;
constexpr std::size_t kShapeFinalOffset = 0x40;
constexpr std::size_t kCameraTransformBytes = 28;

alignas(64) phase::Control g_control = {
    .magic = {'A', '9', 'V', 'C', 'P', 'C', '1', 0},
    .version = phase::kVersion,
    .size = sizeof(phase::Control),
    .flags = 0,
    .maximum_events = 0,
    .expected_manager = 0,
    .expected_node = 0,
    .expected_shape = 0,
    .original_camera_callback = 0,
    .expected_node_vptr = 0,
    .reserved = {0, 0, 0, 0, 0, 0, 0, 0},
};

alignas(64) phase::Evidence g_evidence = {
    .magic = {'A', '9', 'V', 'C', 'P', 'E', '1', 0},
    .version = phase::kVersion,
    .size = sizeof(phase::Evidence),
    .next_sequence = 0,
    .committed_events = 0,
    .dropped_events = 0,
    .camera_entries = 0,
    .camera_original_calls = 0,
    .camera_original_returns = 0,
    .camera_failures = 0,
    .camera_recursive_entries = 0,
    .vehicle_before_events = 0,
    .vehicle_after_events = 0,
    .vehicle_corrected_events = 0,
    .camera_frames = 0,
    .last_status = phase::kPassive,
    .last_tid = 0,
    .reserved0 = 0,
    .last_manager = 0,
    .last_node = 0,
    .last_shape = 0,
    .reserved = {0, 0, 0, 0, 0},
};

alignas(64) phase::Event g_events[phase::kMaximumEvents]{};
std::atomic<std::uint32_t> g_vehicle_depth{0};
std::atomic<std::uint32_t> g_camera_depth{0};

void CopyBytes(void* destination, const void* source, std::size_t size) {
  auto* output = static_cast<std::uint8_t*>(destination);
  const auto* input = static_cast<const std::uint8_t*>(source);
  for (std::size_t index = 0; index < size; ++index)
    output[index] = input[index];
}

std::uint64_t HashBytes(const void* data, std::size_t size) {
  if (data == nullptr) return 0;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint64_t hash = 1469598103934665603ull;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

std::uint32_t CurrentTid() {
  return static_cast<std::uint32_t>(syscall(SYS_gettid));
}

std::uint64_t MonotonicNs(bool* valid) {
  timespec value{};
  if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &value) != 0 ||
      value.tv_sec < 0 || value.tv_nsec < 0) {
    *valid = false;
    return 0;
  }
  *valid = true;
  return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ull +
         static_cast<std::uint64_t>(value.tv_nsec);
}

bool ControlValid(std::uint32_t required_flag) {
  if (__atomic_load_n(&g_control.version, __ATOMIC_ACQUIRE) !=
          phase::kVersion ||
      __atomic_load_n(&g_control.size, __ATOMIC_ACQUIRE) !=
          sizeof(phase::Control))
    return false;
  for (std::size_t index = 0; index < sizeof(phase::kControlMagic); ++index)
    if (g_control.magic[index] != phase::kControlMagic[index]) return false;
  const std::uint32_t flags =
      __atomic_load_n(&g_control.flags, __ATOMIC_ACQUIRE);
  const std::uint32_t allowed = phase::kConfigured | phase::kObserveVehicle |
                                phase::kObserveCamera;
  return (flags & ~allowed) == 0 &&
         (flags & (phase::kConfigured | required_flag)) ==
             (phase::kConfigured | required_flag);
}

void Fail(std::int32_t status) {
  __atomic_add_fetch(&g_evidence.camera_failures, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_status, status, __ATOMIC_RELEASE);
}

void RecordEvent(std::uint32_t kind, std::uint32_t flags,
                 std::uint32_t vehicle_frame, std::uint32_t camera_frame,
                 std::uint64_t vehicle_pose_hash,
                 std::uint64_t vehicle_linear_hash,
                 std::uint64_t camera_world_hash,
                 std::uint64_t camera_shape_hash,
                 std::uint32_t camera_fov_bits) {
  const std::uint32_t maximum_events =
      __atomic_load_n(&g_control.maximum_events, __ATOMIC_ACQUIRE);
  if (maximum_events == 0 || maximum_events > phase::kMaximumEvents) return;
  const std::uint64_t sequence = __atomic_fetch_add(
      &g_evidence.next_sequence, 1u, __ATOMIC_ACQ_REL);
  if (sequence >= maximum_events) {
    __atomic_add_fetch(&g_evidence.dropped_events, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_evidence.last_status, phase::kEventOverflow,
                     __ATOMIC_RELEASE);
    return;
  }
  bool clock_valid = false;
  const std::uint64_t monotonic_ns = MonotonicNs(&clock_valid);
  if (clock_valid) flags |= phase::kClockValid;
  phase::Event& event = g_events[sequence];
  event.sequence = sequence;
  event.monotonic_ns = monotonic_ns;
  event.kind = kind;
  event.flags = flags;
  event.producer_tid = CurrentTid();
  event.vehicle_frame = vehicle_frame;
  event.camera_frame = camera_frame;
  event.reserved0 = 0;
  event.vehicle_pose_hash = vehicle_pose_hash;
  event.vehicle_linear_hash = vehicle_linear_hash;
  event.camera_world_hash = camera_world_hash;
  event.camera_shape_hash = camera_shape_hash;
  event.camera_fov_bits = camera_fov_bits;
  event.reserved1 = 0;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_add_fetch(&g_evidence.committed_events, 1u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.last_tid, event.producer_tid,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_status, phase::kRecording,
                   __ATOMIC_RELEASE);
}

bool NodeIdentity(std::uintptr_t node, std::uintptr_t manager,
                  std::uintptr_t expected_vptr) {
  if (node == 0 || expected_vptr == 0) return false;
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(node);
  const auto callback_address = reinterpret_cast<std::uintptr_t>(
      &a9tas_vehicle_camera_phase_camera_callback_v1);
  return *reinterpret_cast<const std::uintptr_t*>(node) == expected_vptr &&
         bytes[camera_node::kEnabledOffset] == 1 &&
         *reinterpret_cast<const std::uintptr_t*>(
             node + camera_node::kOwnerOffset) == manager &&
         *reinterpret_cast<const std::uintptr_t*>(
             node + camera_node::kSelfOffset) ==
             node + camera_node::kOwnerOffset &&
         *reinterpret_cast<const std::uintptr_t*>(
             node + camera_node::kCallbackOffset) == callback_address &&
         *reinterpret_cast<const std::uintptr_t*>(
             node + camera_node::kContextOffset) == 0;
}

}  // namespace

extern "C" __attribute__((visibility("default"), used)) phase::Control*
    a9tas_vehicle_camera_phase_control_storage_v1 = &g_control;
extern "C" __attribute__((visibility("default"), used)) phase::Evidence*
    a9tas_vehicle_camera_phase_evidence_storage_v1 = &g_evidence;
extern "C" __attribute__((visibility("default"), used)) phase::Event*
    a9tas_vehicle_camera_phase_events_storage_v1 = g_events;

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_vehicle_camera_phase_vehicle_callback_v1(
    void* object, const std::uint64_t* frame_token) {
  const auto* control = reinterpret_cast<const final_writer::Control*>(
      a9tas_final_writer_replay_control_storage_data_v1);
  const auto* evidence = reinterpret_cast<const final_writer::Evidence*>(
      a9tas_final_writer_replay_evidence_storage_data_v1);
  const auto* audits = reinterpret_cast<const final_writer::FrameAudit*>(
      a9tas_final_writer_replay_audit_storage_data_v1);
  const std::uint32_t frame_index =
      evidence == nullptr
          ? UINT32_MAX
          : __atomic_load_n(&evidence->processed_frames, __ATOMIC_ACQUIRE);
  const std::uint64_t frame_permit =
      control == nullptr
          ? final_writer::kFramePermitDisarmed
          : __atomic_load_n(&control->reserved[0], __ATOMIC_ACQUIRE);
  const auto* pose =
      control == nullptr
          ? nullptr
          : reinterpret_cast<const void*>(__atomic_load_n(
                &control->native_pose, __ATOMIC_ACQUIRE));
  const auto* linear =
      control == nullptr
          ? nullptr
          : reinterpret_cast<const void*>(__atomic_load_n(
                &control->native_linear, __ATOMIC_ACQUIRE));

  const std::uint32_t previous_depth =
      g_vehicle_depth.fetch_add(1u, std::memory_order_acq_rel);
  const bool observe = previous_depth == 0 &&
                       ControlValid(phase::kObserveVehicle) &&
                       frame_index < final_writer::kMaximumFrames &&
                       frame_permit == final_writer::FramePermit(frame_index) &&
                       pose != nullptr && linear != nullptr && audits != nullptr;
  if (observe) {
    RecordEvent(phase::kVehicleBeforeFinalWriter, 0, frame_index, UINT32_MAX,
                HashBytes(pose, final_writer::kTransformSize),
                HashBytes(linear, final_writer::kLinearSize), 0, 0, 0);
    __atomic_add_fetch(&g_evidence.vehicle_before_events, 1u,
                       __ATOMIC_RELAXED);
  }

  const std::int64_t result =
      a9tas_final_writer_replay_callback_v1(object, frame_token);

  if (observe) {
    const std::uint32_t audit_flags = audits[frame_index].flags;
    std::uint32_t flags = phase::kOriginalReturned;
    if ((audit_flags & final_writer::kAuditEqual) != 0)
      flags |= phase::kVehicleEqual;
    if ((audit_flags & final_writer::kAuditCorrected) != 0)
      flags |= phase::kVehicleCorrected;
    if ((audit_flags & final_writer::kAuditImmediateExact) != 0)
      flags |= phase::kVehicleImmediateExact;
    RecordEvent(phase::kVehicleAfterFinalWriter, flags, frame_index,
                UINT32_MAX, HashBytes(pose, final_writer::kTransformSize),
                HashBytes(linear, final_writer::kLinearSize), 0, 0, 0);
    __atomic_add_fetch(&g_evidence.vehicle_after_events, 1u,
                       __ATOMIC_RELAXED);
    if ((audit_flags & final_writer::kAuditCorrected) != 0)
      __atomic_add_fetch(&g_evidence.vehicle_corrected_events, 1u,
                         __ATOMIC_RELAXED);
  }
  g_vehicle_depth.fetch_sub(1u, std::memory_order_release);
  return result;
}

extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_vehicle_camera_phase_camera_callback_v1(void* manager) {
  __atomic_add_fetch(&g_evidence.camera_entries, 1u, __ATOMIC_RELAXED);
  const std::uintptr_t original_address = __atomic_load_n(
      &g_control.original_camera_callback, __ATOMIC_ACQUIRE);
  if (manager == nullptr || original_address == 0 ||
      original_address == reinterpret_cast<std::uintptr_t>(
                              &a9tas_vehicle_camera_phase_camera_callback_v1)) {
    Fail(phase::kInvalidControl);
    return;
  }
  const std::uint32_t previous_depth =
      g_camera_depth.fetch_add(1u, std::memory_order_acq_rel);
  if (previous_depth != 0) {
    g_camera_depth.fetch_sub(1u, std::memory_order_release);
    __atomic_add_fetch(&g_evidence.camera_recursive_entries, 1u,
                       __ATOMIC_RELAXED);
    Fail(phase::kRecursiveCameraEntry);
    return;
  }

  __atomic_add_fetch(&g_evidence.camera_original_calls, 1u,
                     __ATOMIC_RELAXED);
  reinterpret_cast<CameraCallback>(original_address)(manager);
  __atomic_add_fetch(&g_evidence.camera_original_returns, 1u,
                     __ATOMIC_RELAXED);

  if (!ControlValid(phase::kObserveCamera)) {
    g_camera_depth.fetch_sub(1u, std::memory_order_release);
    Fail(phase::kInvalidControl);
    return;
  }
  const std::uintptr_t expected_manager =
      __atomic_load_n(&g_control.expected_manager, __ATOMIC_ACQUIRE);
  if (reinterpret_cast<std::uintptr_t>(manager) != expected_manager) {
    g_camera_depth.fetch_sub(1u, std::memory_order_release);
    Fail(phase::kUnexpectedManager);
    return;
  }
  const std::uintptr_t node =
      __atomic_load_n(&g_control.expected_node, __ATOMIC_ACQUIRE);
  if (!NodeIdentity(node, expected_manager,
                    __atomic_load_n(&g_control.expected_node_vptr,
                                    __ATOMIC_ACQUIRE))) {
    g_camera_depth.fetch_sub(1u, std::memory_order_release);
    Fail(phase::kUnexpectedNode);
    return;
  }
  const std::uintptr_t shape =
      *reinterpret_cast<const std::uintptr_t*>(expected_manager +
                                               kManagerShapeOffset);
  if (shape == 0 ||
      shape != __atomic_load_n(&g_control.expected_shape, __ATOMIC_ACQUIRE)) {
    g_camera_depth.fetch_sub(1u, std::memory_order_release);
    Fail(phase::kUnexpectedShape);
    return;
  }

  std::uint32_t fov_bits = 0;
  CopyBytes(&fov_bits,
            reinterpret_cast<const void*>(expected_manager +
                                          kManagerFovOffset),
            sizeof(fov_bits));
  const std::uint32_t camera_frame = __atomic_fetch_add(
      &g_evidence.camera_frames, 1u, __ATOMIC_ACQ_REL);
  const auto* final_evidence =
      reinterpret_cast<const final_writer::Evidence*>(
          a9tas_final_writer_replay_evidence_storage_data_v1);
  const auto* final_control =
      reinterpret_cast<const final_writer::Control*>(
          a9tas_final_writer_replay_control_storage_data_v1);
  const std::uint32_t vehicle_frame =
      final_evidence == nullptr
          ? UINT32_MAX
          : __atomic_load_n(&final_evidence->processed_frames,
                            __ATOMIC_ACQUIRE);
  const auto* vehicle_pose =
      final_control == nullptr
          ? nullptr
          : reinterpret_cast<const void*>(__atomic_load_n(
                &final_control->native_pose, __ATOMIC_ACQUIRE));
  const auto* vehicle_linear =
      final_control == nullptr
          ? nullptr
          : reinterpret_cast<const void*>(__atomic_load_n(
                &final_control->native_linear, __ATOMIC_ACQUIRE));
  const bool vehicle_snapshot_present =
      vehicle_pose != nullptr && vehicle_linear != nullptr;
  RecordEvent(
      phase::kCameraAfterOriginal,
      phase::kOriginalReturned | phase::kManagerIdentity |
          phase::kNodeIdentity | phase::kShapeIdentity |
          (vehicle_snapshot_present ? phase::kVehicleSnapshotPresent : 0),
      vehicle_frame, camera_frame,
      vehicle_snapshot_present
          ? HashBytes(vehicle_pose, final_writer::kTransformSize)
          : 0,
      vehicle_snapshot_present
          ? HashBytes(vehicle_linear, final_writer::kLinearSize)
          : 0,
      HashBytes(reinterpret_cast<const void*>(expected_manager +
                                              kManagerWorldOffset),
                kCameraTransformBytes),
      HashBytes(reinterpret_cast<const void*>(shape + kShapeFinalOffset),
                kCameraTransformBytes),
      fov_bits);
  __atomic_store_n(&g_evidence.last_manager, expected_manager,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_node, node, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_shape, shape, __ATOMIC_RELAXED);
  g_camera_depth.fetch_sub(1u, std::memory_order_release);
}

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_vehicle_camera_phase_protocol_v1() {
  return phase::kVersion;
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_vehicle_camera_phase_arm_v1() {
  return phase::kRejectedBuildOnly;
}
