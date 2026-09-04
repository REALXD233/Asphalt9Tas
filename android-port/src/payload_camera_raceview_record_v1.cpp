// Build-only RaceView post-original recorder. It cannot install itself and it
// contains no replay/camera write branch. Its arm export always returns -100.

#include "camera_raceview_callback_node_v1.h"
#include "camera_raceview_record_protocol_v1.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sys/syscall.h>
#include <unistd.h>

namespace callback = a9tas::camera_raceview_callback_node_v1;
namespace record = a9tas::camera_raceview_record_v1;

extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_camera_raceview_record_callback_v1(void* manager);

namespace {

using OriginalCallback = void (*)(void*);

constexpr std::size_t kManagerLocalOffset = 0x10;
constexpr std::size_t kManagerWorldOffset = 0x2C;
constexpr std::size_t kManagerShapeOffset = 0xE8;
constexpr std::size_t kManagerFovOffset = 0x108;
constexpr std::size_t kShapeFinalOffset = 0x40;
constexpr std::size_t kTransformBytes = 28;

alignas(64) record::Control g_control = {
    .magic = {'A', '9', 'C', 'R', 'C', '1', 0, 0},
    .version = record::kVersion,
    .size = sizeof(record::Control),
    .flags = 0,
    .frame_count = 0,
    .expected_manager = 0,
    .expected_node = 0,
    .expected_shape = 0,
    .original_callback = 0,
    .expected_node_vptr = 0,
    .frame_permit = record::kPermitDisarmed,
    .expected_producer_tid = 0,
    .reserved0 = 0,
    .reserved = {0, 0, 0, 0, 0, 0},
};

alignas(64) record::Evidence g_evidence = {
    .magic = {'A', '9', 'C', 'R', 'E', '1', 0, 0},
    .version = record::kVersion,
    .size = sizeof(record::Evidence),
    .wrapper_entries = 0,
    .original_calls = 0,
    .original_returns = 0,
    .idle_entries = 0,
    .claimed_permits = 0,
    .recorded_frames = 0,
    .failures = 0,
    .recursive_entries = 0,
    .processed_frames = 0,
    .last_status = record::kPassive,
    .last_manager = 0,
    .last_node = 0,
    .last_shape = 0,
    .last_tid = 0,
    .last_flags = 0,
    .reserved = 0,
};

alignas(64) record::Frame g_frames[record::kMaximumFrames]{};
std::atomic<std::uint32_t> g_wrapper_depth{0};

void CopyBytes(void* destination, const void* source, std::size_t size) {
  auto* output = static_cast<std::uint8_t*>(destination);
  const auto* input = static_cast<const std::uint8_t*>(source);
  for (std::size_t index = 0; index < size; ++index) output[index] = input[index];
}

void Fail(std::int32_t status) {
  __atomic_add_fetch(&g_evidence.failures, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_status, status, __ATOMIC_RELEASE);
}

std::uint32_t CurrentTid() {
  return static_cast<std::uint32_t>(syscall(SYS_gettid));
}

bool NodeIdentity(std::uintptr_t node, std::uintptr_t manager,
                  std::uintptr_t expected_vptr) {
  if (node == 0 || expected_vptr == 0) return false;
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(node);
  const auto* words = reinterpret_cast<const std::uintptr_t*>(node);
  const auto callback_address = reinterpret_cast<std::uintptr_t>(
      &a9tas_camera_raceview_record_callback_v1);
  return words[0] == expected_vptr && bytes[callback::kEnabledOffset] == 1 &&
         *reinterpret_cast<const std::uintptr_t*>(
             node + callback::kOwnerOffset) == manager &&
         *reinterpret_cast<const std::uintptr_t*>(
             node + callback::kSelfOffset) == node + callback::kOwnerOffset &&
         *reinterpret_cast<const std::uintptr_t*>(
             node + callback::kCallbackOffset) == callback_address &&
         *reinterpret_cast<const std::uintptr_t*>(
             node + callback::kContextOffset) == 0;
}

}  // namespace

// Direct storage pointers let the host resolve/configure the recorder without
// making any guest-side getter call.  The pointed-to storage remains private
// to this payload; only the pointer symbols are part of the preload ABI.
extern "C" __attribute__((visibility("default"), used)) record::Control*
    a9tas_camera_raceview_record_control_storage_v1 = &g_control;
extern "C" __attribute__((visibility("default"), used)) record::Evidence*
    a9tas_camera_raceview_record_evidence_storage_v1 = &g_evidence;
extern "C" __attribute__((visibility("default"), used)) record::Frame*
    a9tas_camera_raceview_record_frames_storage_v1 = g_frames;

extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_camera_raceview_record_callback_v1(void* manager) {
  __atomic_add_fetch(&g_evidence.wrapper_entries, 1u, __ATOMIC_RELAXED);
  const std::uintptr_t expected_manager =
      __atomic_load_n(&g_control.expected_manager, __ATOMIC_ACQUIRE);
  const std::uintptr_t original_address =
      __atomic_load_n(&g_control.original_callback, __ATOMIC_ACQUIRE);
  const std::uint32_t flags =
      __atomic_load_n(&g_control.flags, __ATOMIC_ACQUIRE);
  const std::uint32_t frame_count =
      __atomic_load_n(&g_control.frame_count, __ATOMIC_ACQUIRE);
  const bool callable_original =
      manager != nullptr && original_address != 0 &&
      original_address != reinterpret_cast<std::uintptr_t>(
                              &a9tas_camera_raceview_record_callback_v1);
  if (!callable_original) {
    Fail(record::kInvalidControl);
    return;
  }
  const std::uint32_t allowed_flags =
      record::kConfigured | record::kContinuousCapture;
  if ((flags & record::kConfigured) == 0 ||
      (flags & ~allowed_flags) != 0 || expected_manager == 0 ||
      reinterpret_cast<std::uintptr_t>(manager) != expected_manager ||
      frame_count == 0 || frame_count > record::kMaximumFrames) {
    // Fail open: once the callback slot has been replaced, malformed or stale
    // recorder configuration must never suppress the game's camera update.
    __atomic_add_fetch(&g_evidence.original_calls, 1u, __ATOMIC_RELAXED);
    reinterpret_cast<OriginalCallback>(original_address)(manager);
    __atomic_add_fetch(&g_evidence.original_returns, 1u, __ATOMIC_RELAXED);
    Fail(record::kInvalidControl);
    return;
  }

  const std::uint32_t previous_depth =
      g_wrapper_depth.fetch_add(1u, std::memory_order_acq_rel);
  if (previous_depth != 0) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    __atomic_add_fetch(&g_evidence.recursive_entries, 1u, __ATOMIC_RELAXED);
    Fail(record::kRecursiveEntry);
    return;
  }

  // AluTasV2 camera ordering: the complete real CameraUpdate runs first.
  __atomic_add_fetch(&g_evidence.original_calls, 1u, __ATOMIC_RELAXED);
  reinterpret_cast<OriginalCallback>(original_address)(manager);
  __atomic_add_fetch(&g_evidence.original_returns, 1u, __ATOMIC_RELAXED);

  const std::uint32_t frame_index =
      __atomic_load_n(&g_evidence.processed_frames, __ATOMIC_ACQUIRE);
  if (frame_index == frame_count) {
    __atomic_add_fetch(&g_evidence.idle_entries, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_evidence.last_status, record::kComplete,
                     __ATOMIC_RELEASE);
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    return;
  }
  if (frame_index > frame_count || frame_index >= record::kMaximumFrames) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(record::kFrameOverflow);
    return;
  }
  const bool continuous_capture =
      (flags & record::kContinuousCapture) != 0;
  std::uint64_t claimed_permit = record::kContinuousPermit;
  if (!continuous_capture) {
    std::uint64_t expected_permit = record::FramePermit(frame_index);
    if (!__atomic_compare_exchange_n(
            &g_control.frame_permit, &expected_permit,
            record::kPermitDisarmed, false, __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE)) {
      __atomic_add_fetch(&g_evidence.idle_entries, 1u, __ATOMIC_RELAXED);
      g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
      return;
    }
    claimed_permit = record::FramePermit(frame_index);
  }
  __atomic_add_fetch(&g_evidence.claimed_permits, 1u, __ATOMIC_RELAXED);

  const std::uint32_t tid = CurrentTid();
  const std::uint32_t expected_tid =
      __atomic_load_n(&g_control.expected_producer_tid, __ATOMIC_ACQUIRE);
  if (expected_tid != 0 && tid != expected_tid) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(record::kUnexpectedThread);
    return;
  }
  const std::uintptr_t node =
      __atomic_load_n(&g_control.expected_node, __ATOMIC_ACQUIRE);
  const std::uintptr_t shape =
      *reinterpret_cast<const std::uintptr_t*>(
          expected_manager + kManagerShapeOffset);
  const std::uintptr_t expected_shape =
      __atomic_load_n(&g_control.expected_shape, __ATOMIC_ACQUIRE);
  if (!NodeIdentity(node, expected_manager,
                    __atomic_load_n(&g_control.expected_node_vptr,
                                    __ATOMIC_ACQUIRE))) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(record::kUnexpectedNode);
    return;
  }
  if (shape == 0 || shape != expected_shape) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(record::kUnexpectedShape);
    return;
  }

  record::Frame& frame = g_frames[frame_index];
  frame.frame_index = frame_index;
  frame.producer_tid = tid;
  frame.claimed_permit = claimed_permit;
  frame.flags = record::kOriginalReturned | record::kManagerIdentity |
                record::kNodeIdentity | record::kShapeIdentity;
  frame.reserved = 0;
  CopyBytes(frame.manager_local,
            reinterpret_cast<const void*>(expected_manager +
                                          kManagerLocalOffset),
            kTransformBytes);
  CopyBytes(frame.manager_world,
            reinterpret_cast<const void*>(expected_manager +
                                          kManagerWorldOffset),
            kTransformBytes);
  CopyBytes(frame.shape_final,
            reinterpret_cast<const void*>(shape + kShapeFinalOffset),
            kTransformBytes);
  CopyBytes(&frame.fov_radians,
            reinterpret_cast<const void*>(expected_manager +
                                          kManagerFovOffset),
            sizeof(frame.fov_radians));

  __atomic_store_n(&g_evidence.last_manager, expected_manager,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_node, node, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_shape, shape, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_tid, tid, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_flags, frame.flags, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_evidence.recorded_frames, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.processed_frames, frame_index + 1,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.last_status,
                   frame_index + 1 == frame_count ? record::kComplete
                                                  : record::kRecording,
                   __ATOMIC_RELEASE);
  g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
}

extern "C" __attribute__((visibility("default"))) record::Control*
a9tas_camera_raceview_record_control_v1() {
  return &g_control;
}

extern "C" __attribute__((visibility("default"))) record::Evidence*
a9tas_camera_raceview_record_evidence_v1() {
  return &g_evidence;
}

extern "C" __attribute__((visibility("default"))) record::Frame*
a9tas_camera_raceview_record_frames_v1() {
  return g_frames;
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_camera_raceview_record_arm_v1() {
  return record::kRejectedBuildOnly;
}
