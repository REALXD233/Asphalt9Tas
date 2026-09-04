// Build-only five-callback post-original RaceView same-state writer.
// It cannot install itself and its arm export always returns -100.

#include "camera_raceview_callback_node_v1.h"
#include "camera_raceview_same_state_protocol_v1.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sys/syscall.h>
#include <unistd.h>

namespace callback = a9tas::camera_raceview_callback_node_v1;
namespace gate = a9tas::camera_raceview_same_state_v1;

extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_camera_raceview_same_state_callback_v1(void* manager);

namespace {

using OriginalCallback = void (*)(void*);
using CombinedTransform = void (*)(void*, const void*);

constexpr std::uintptr_t kManagerEmbeddedOffset = 0x08;
constexpr std::uintptr_t kManagerWorldOffset = 0x2C;
constexpr std::uintptr_t kManagerShapeOffset = 0xE8;
constexpr std::uintptr_t kManagerFovOffset = 0x108;
constexpr std::uintptr_t kEmbeddedCombinedSlot = 0x40;
constexpr std::uintptr_t kShapeFinalOffset = 0x40;
constexpr std::size_t kTransformBytes = 28;

alignas(64) gate::Control g_control = {
    .magic = {'A','9','C','S','C','1',0,0},
    .version = gate::kVersion,
    .size = sizeof(gate::Control),
    .flags = 0,
    .frame_count = 0,
    .expected_manager = 0,
    .expected_node = 0,
    .expected_shape = 0,
    .original_callback = 0,
    .expected_node_vptr = 0,
    .expected_embedded_vptr = 0,
    .expected_combined_function = 0,
    .expected_producer_tid = 0,
    .reserved0 = 0,
    .reserved = {0,0,0,0},
};
alignas(64) gate::Evidence g_evidence = {
    .magic = {'A','9','C','S','E','1',0,0},
    .version = gate::kVersion,
    .size = sizeof(gate::Evidence),
    .wrapper_entries = 0,
    .original_calls = 0,
    .original_returns = 0,
    .combined_calls = 0,
    .combined_returns = 0,
    .fov_same_writes = 0,
    .recorded_frames = 0,
    .idle_entries = 0,
    .failures = 0,
    .recursive_entries = 0,
    .processed_frames = 0,
    .last_status = gate::kPassive,
    .last_manager = 0,
    .last_node = 0,
    .last_shape = 0,
};
alignas(64) gate::Frame g_frames[gate::kMaximumFrames]{};
std::atomic<std::uint32_t> g_wrapper_depth{0};

void CopyBytes(void* destination, const void* source, std::size_t size) {
  auto* output = static_cast<std::uint8_t*>(destination);
  const auto* input = static_cast<const std::uint8_t*>(source);
  for (std::size_t index = 0; index < size; ++index) output[index] = input[index];
}

bool SameBytes(const void* left, const void* right, std::size_t size) {
  const auto* a = static_cast<const std::uint8_t*>(left);
  const auto* b = static_cast<const std::uint8_t*>(right);
  for (std::size_t index = 0; index < size; ++index)
    if (a[index] != b[index]) return false;
  return true;
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
  const auto callback_address = reinterpret_cast<std::uintptr_t>(
      &a9tas_camera_raceview_same_state_callback_v1);
  return *reinterpret_cast<const std::uintptr_t*>(node) == expected_vptr &&
      bytes[callback::kEnabledOffset] == 1 &&
      *reinterpret_cast<const std::uintptr_t*>(node + callback::kOwnerOffset) == manager &&
      *reinterpret_cast<const std::uintptr_t*>(node + callback::kSelfOffset) ==
          node + callback::kOwnerOffset &&
      *reinterpret_cast<const std::uintptr_t*>(node + callback::kCallbackOffset) ==
          callback_address &&
      *reinterpret_cast<const std::uintptr_t*>(node + callback::kContextOffset) == 0;
}

}  // namespace

extern "C" __attribute__((visibility("default"), used)) gate::Control*
    a9tas_camera_raceview_same_state_control_storage_v1 = &g_control;
extern "C" __attribute__((visibility("default"), used)) gate::Evidence*
    a9tas_camera_raceview_same_state_evidence_storage_v1 = &g_evidence;
extern "C" __attribute__((visibility("default"), used)) gate::Frame*
    a9tas_camera_raceview_same_state_frames_storage_v1 = g_frames;

extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_camera_raceview_same_state_callback_v1(void* manager) {
  __atomic_add_fetch(&g_evidence.wrapper_entries, 1u, __ATOMIC_RELAXED);
  const std::uintptr_t original_address =
      __atomic_load_n(&g_control.original_callback, __ATOMIC_ACQUIRE);
  const bool callable_original = manager != nullptr && original_address != 0 &&
      original_address != reinterpret_cast<std::uintptr_t>(
                              &a9tas_camera_raceview_same_state_callback_v1);
  if (!callable_original) {
    Fail(gate::kInvalidControl);
    return;
  }
  const std::uintptr_t expected_manager =
      __atomic_load_n(&g_control.expected_manager, __ATOMIC_ACQUIRE);
  const std::uint32_t frame_count =
      __atomic_load_n(&g_control.frame_count, __ATOMIC_ACQUIRE);
  if (__atomic_load_n(&g_control.flags, __ATOMIC_ACQUIRE) != gate::kConfigured ||
      expected_manager == 0 || reinterpret_cast<std::uintptr_t>(manager) != expected_manager ||
      frame_count == 0 || frame_count > gate::kMaximumFrames) {
    __atomic_add_fetch(&g_evidence.original_calls, 1u, __ATOMIC_RELAXED);
    reinterpret_cast<OriginalCallback>(original_address)(manager);
    __atomic_add_fetch(&g_evidence.original_returns, 1u, __ATOMIC_RELAXED);
    Fail(gate::kInvalidControl);
    return;
  }

  const std::uint32_t previous_depth =
      g_wrapper_depth.fetch_add(1u, std::memory_order_acq_rel);
  if (previous_depth != 0) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    __atomic_add_fetch(&g_evidence.recursive_entries, 1u, __ATOMIC_RELAXED);
    Fail(gate::kRecursiveEntry);
    return;
  }

  // Exact upstream order: complete the game's real CameraUpdate first.
  __atomic_add_fetch(&g_evidence.original_calls, 1u, __ATOMIC_RELAXED);
  reinterpret_cast<OriginalCallback>(original_address)(manager);
  __atomic_add_fetch(&g_evidence.original_returns, 1u, __ATOMIC_RELAXED);

  const std::uint32_t frame_index =
      __atomic_load_n(&g_evidence.processed_frames, __ATOMIC_ACQUIRE);
  if (frame_index == frame_count) {
    __atomic_add_fetch(&g_evidence.idle_entries, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_evidence.last_status, gate::kComplete, __ATOMIC_RELEASE);
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    return;
  }
  if (frame_index > frame_count || frame_index >= gate::kMaximumFrames) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(gate::kFrameOverflow);
    return;
  }

  const std::uint32_t tid = CurrentTid();
  const std::uint32_t expected_tid =
      __atomic_load_n(&g_control.expected_producer_tid, __ATOMIC_ACQUIRE);
  if (expected_tid != 0 && tid != expected_tid) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(gate::kUnexpectedThread);
    return;
  }
  const std::uintptr_t node =
      __atomic_load_n(&g_control.expected_node, __ATOMIC_ACQUIRE);
  if (!NodeIdentity(node, expected_manager,
                    __atomic_load_n(&g_control.expected_node_vptr,
                                    __ATOMIC_ACQUIRE))) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(gate::kUnexpectedNode);
    return;
  }
  const std::uintptr_t shape = *reinterpret_cast<const std::uintptr_t*>(
      expected_manager + kManagerShapeOffset);
  if (shape == 0 || shape !=
      __atomic_load_n(&g_control.expected_shape, __ATOMIC_ACQUIRE)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(gate::kUnexpectedShape);
    return;
  }

  void* embedded = reinterpret_cast<void*>(expected_manager + kManagerEmbeddedOffset);
  const std::uintptr_t embedded_vptr =
      *reinterpret_cast<const std::uintptr_t*>(embedded);
  if (embedded_vptr != __atomic_load_n(&g_control.expected_embedded_vptr,
                                       __ATOMIC_ACQUIRE)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(gate::kUnexpectedEmbeddedVptr);
    return;
  }
  const std::uintptr_t combined_address =
      *reinterpret_cast<const std::uintptr_t*>(embedded_vptr +
                                               kEmbeddedCombinedSlot);
  if (combined_address == 0 || combined_address !=
      __atomic_load_n(&g_control.expected_combined_function,
                      __ATOMIC_ACQUIRE)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(gate::kUnexpectedCombinedFunction);
    return;
  }

  gate::Frame& frame = g_frames[frame_index];
  frame.frame_index = frame_index;
  frame.producer_tid = tid;
  frame.flags = gate::kOriginalReturned | gate::kManagerIdentity |
      gate::kNodeIdentity | gate::kShapeIdentity |
      gate::kEmbeddedVptrIdentity | gate::kCombinedFunctionIdentity;
  CopyBytes(frame.manager_world_before,
            reinterpret_cast<const void*>(expected_manager + kManagerWorldOffset),
            kTransformBytes);
  CopyBytes(frame.shape_before,
            reinterpret_cast<const void*>(shape + kShapeFinalOffset),
            kTransformBytes);
  CopyBytes(&frame.fov_before,
            reinterpret_cast<const void*>(expected_manager + kManagerFovOffset),
            sizeof(frame.fov_before));

  __atomic_add_fetch(&g_evidence.combined_calls, 1u, __ATOMIC_RELAXED);
  reinterpret_cast<CombinedTransform>(combined_address)(
      embedded, frame.manager_world_before);
  __atomic_add_fetch(&g_evidence.combined_returns, 1u, __ATOMIC_RELAXED);
  frame.flags |= gate::kCombinedReturned;

  *reinterpret_cast<float*>(expected_manager + kManagerFovOffset) =
      frame.fov_before;
  __atomic_add_fetch(&g_evidence.fov_same_writes, 1u, __ATOMIC_RELAXED);
  CopyBytes(frame.shape_after,
            reinterpret_cast<const void*>(shape + kShapeFinalOffset),
            kTransformBytes);
  CopyBytes(&frame.fov_after,
            reinterpret_cast<const void*>(expected_manager + kManagerFovOffset),
            sizeof(frame.fov_after));
  if (!SameBytes(&frame.fov_before, &frame.fov_after,
                 sizeof(frame.fov_before))) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(gate::kFovChanged);
    return;
  }
  frame.flags |= gate::kFovSameBytes;
  __atomic_store_n(&g_evidence.last_manager, expected_manager, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_node, node, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_shape, shape, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_evidence.recorded_frames, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.processed_frames, frame_index + 1,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.last_status,
                   frame_index + 1 == frame_count ? gate::kComplete
                                                  : gate::kRunning,
                   __ATOMIC_RELEASE);
  g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_camera_raceview_same_state_arm_v1() {
  return gate::kRejectedBuildOnly;
}
