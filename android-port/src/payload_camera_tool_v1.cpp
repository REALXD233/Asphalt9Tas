// Standalone continuous Camera Tool carrier for the exact Android RaceView
// callback.  It follows upstream ordering: original camera update first,
// optional override second.  It does not install itself and is not a replay
// camera track.

#include "camera_raceview_callback_node_v1.h"
#include "camera_tool_protocol_v1.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sys/syscall.h>
#include <unistd.h>

namespace callback = a9tas::camera_raceview_callback_node_v1;
namespace camera = a9tas::camera_tool_v1;

extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_camera_tool_callback_v1(void* manager);

namespace {

using OriginalCallback = void (*)(void*);
using CombinedTransform = void (*)(void*, const void*);
using PositionSetter = void (*)(void*, const float*);
using RotationSetter = void (*)(void*, const float*);
using FovSetter = void (*)(void*, float, std::uint32_t);

constexpr std::uintptr_t kManagerEmbeddedOffset = 0x08;
constexpr std::uintptr_t kManagerWorldOffset = 0x2C;
constexpr std::uintptr_t kManagerSourceOffset = 0xD8;
constexpr std::uintptr_t kManagerShapeOffset = 0xE8;
constexpr std::uintptr_t kManagerFovOffset = 0x108;
constexpr std::uintptr_t kEmbeddedCombinedSlot = 0x40;
constexpr std::uintptr_t kSourcePositionOffset = 0x38;
constexpr std::uintptr_t kSourceRotationOffset = 0x44;
constexpr std::uintptr_t kSourceFovOffset = 0x124;
constexpr std::uintptr_t kSourceFovModeOffset = 0x128;
constexpr std::uintptr_t kSourcePositionSetterSlot = 0x88;
constexpr std::uintptr_t kSourceRotationSetterSlot = 0x90;
constexpr std::size_t kTransformBytes = 7 * sizeof(float);

alignas(64) camera::Control g_control = {
    .magic = {'A','9','C','T','C','1',0,0},
    .version = camera::kVersion,
    .size = sizeof(camera::Control),
    .flags = 0,
    .override_flags = 0,
    .expected_manager = 0,
    .expected_node = 0,
    .expected_shape = 0,
    .original_callback = 0,
    .expected_node_vptr = 0,
    .expected_embedded_vptr = 0,
    .expected_combined_function = 0,
    .expected_producer_tid = 0,
    .reserved0 = 0,
    .command_sequence = 0,
    .position = {0,0,0},
    .rotation = {0,0,0,1},
    .fov_radians = 1.0f,
    .offset_relative_to_car = {0,0,0},
    .look_backwards = 0,
    .expected_source = 0,
    .expected_source_vptr = 0,
    .expected_position_setter = 0,
    .expected_rotation_setter = 0,
    .expected_fov_setter = 0,
    .reserved = 0,
};

alignas(64) camera::Evidence g_evidence = {
    .magic = {'A','9','C','T','E','1',0,0},
    .version = camera::kVersion,
    .size = sizeof(camera::Evidence),
    .wrapper_entries = 0,
    .original_calls = 0,
    .original_returns = 0,
    .active_entries = 0,
    .inactive_entries = 0,
    .combined_calls = 0,
    .combined_returns = 0,
    .fov_writes = 0,
    .command_busy_entries = 0,
    .failures = 0,
    .recursive_entries = 0,
    .producer_tid = 0,
    .last_status = camera::kPassive,
    .last_command_sequence = 0,
    .last_manager = 0,
    .last_node = 0,
    .last_shape = 0,
    .last_override_flags = 0,
    .reserved0 = 0,
    .natural_transform = {0,0,0,0,0,0,1},
    .applied_transform = {0,0,0,0,0,0,1},
    .natural_fov = 0,
    .applied_fov = 0,
    .source_override_calls = 0,
    .source_override_returns = 0,
    .source_readback_passes = 0,
    .last_source = 0,
    .last_source_vptr = 0,
};

std::atomic<std::uint32_t> g_wrapper_depth{0};

struct CommandSnapshot {
  std::uint64_t sequence{};
  std::uint32_t flags{};
  float position[3]{};
  float rotation[4]{};
  float fov_radians{};
};

void CopyBytes(void* destination, const void* source, std::size_t size) {
  auto* output = static_cast<std::uint8_t*>(destination);
  const auto* input = static_cast<const std::uint8_t*>(source);
  for (std::size_t index = 0; index < size; ++index) output[index] = input[index];
}

void SetStatus(std::int32_t status) {
  __atomic_store_n(&g_evidence.last_status, status, __ATOMIC_RELEASE);
}

void Fail(std::int32_t status) {
  __atomic_add_fetch(&g_evidence.failures, 1u, __ATOMIC_RELAXED);
  SetStatus(status);
}

std::uint32_t CurrentTid() {
  return static_cast<std::uint32_t>(syscall(SYS_gettid));
}

bool NodeIdentity(std::uintptr_t node, std::uintptr_t manager,
                  std::uintptr_t expected_vptr) {
  if (node == 0 || expected_vptr == 0) return false;
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(node);
  const auto callback_address =
      reinterpret_cast<std::uintptr_t>(&a9tas_camera_tool_callback_v1);
  return *reinterpret_cast<const std::uintptr_t*>(node) == expected_vptr &&
      bytes[callback::kEnabledOffset] == 1 &&
      *reinterpret_cast<const std::uintptr_t*>(node + callback::kOwnerOffset) == manager &&
      *reinterpret_cast<const std::uintptr_t*>(node + callback::kSelfOffset) ==
          node + callback::kOwnerOffset &&
      *reinterpret_cast<const std::uintptr_t*>(node + callback::kCallbackOffset) ==
          callback_address &&
      *reinterpret_cast<const std::uintptr_t*>(node + callback::kContextOffset) == 0;
}

bool ReadStableCommand(CommandSnapshot* output) {
  if (output == nullptr) return false;
  for (std::uint32_t attempt = 0; attempt < 3; ++attempt) {
    const std::uint64_t before =
        __atomic_load_n(&g_control.command_sequence, __ATOMIC_ACQUIRE);
    if ((before & 1u) != 0) continue;
    CommandSnapshot value{};
    value.sequence = before;
    value.flags = __atomic_load_n(&g_control.override_flags, __ATOMIC_RELAXED);
    CopyBytes(value.position, g_control.position, sizeof(value.position));
    CopyBytes(value.rotation, g_control.rotation, sizeof(value.rotation));
    CopyBytes(&value.fov_radians, &g_control.fov_radians,
              sizeof(value.fov_radians));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const std::uint64_t after =
        __atomic_load_n(&g_control.command_sequence, __ATOMIC_ACQUIRE);
    if (before == after && (after & 1u) == 0) {
      *output = value;
      return true;
    }
  }
  return false;
}

bool FiniteCommand(const CommandSnapshot& command) {
  if ((command.flags & camera::kOverridePosition) != 0)
    for (float value : command.position) if (!std::isfinite(value)) return false;
  if ((command.flags & camera::kOverrideRotation) != 0)
    for (float value : command.rotation) if (!std::isfinite(value)) return false;
  if ((command.flags & camera::kOverrideFov) != 0 &&
      !std::isfinite(command.fov_radians)) return false;
  return true;
}

}  // namespace

extern "C" __attribute__((visibility("default"), used)) camera::Control*
    a9tas_camera_tool_control_storage_v1 = &g_control;
extern "C" __attribute__((visibility("default"), used)) camera::Evidence*
    a9tas_camera_tool_evidence_storage_v1 = &g_evidence;

extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_camera_tool_callback_v1(void* manager) {
  __atomic_add_fetch(&g_evidence.wrapper_entries, 1u, __ATOMIC_RELAXED);
  const std::uintptr_t original_address =
      __atomic_load_n(&g_control.original_callback, __ATOMIC_ACQUIRE);
  if (manager == nullptr || original_address == 0 ||
      original_address == reinterpret_cast<std::uintptr_t>(
                              &a9tas_camera_tool_callback_v1)) {
    Fail(camera::kInvalidControl);
    return;
  }

  const std::uint32_t previous_depth =
      g_wrapper_depth.fetch_add(1u, std::memory_order_acq_rel);
  if (previous_depth != 0) {
    // The authoritative Camera setters notify observers.  A synchronous
    // RaceView re-entry must preserve the game's notification callback but
    // must not recursively publish the override again.
    __atomic_add_fetch(&g_evidence.recursive_entries, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_evidence.original_calls, 1u, __ATOMIC_RELAXED);
    reinterpret_cast<OriginalCallback>(original_address)(manager);
    __atomic_add_fetch(&g_evidence.original_returns, 1u, __ATOMIC_RELAXED);
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    return;
  }

  // Exact upstream Camera Tool order: let the game produce its camera first.
  __atomic_add_fetch(&g_evidence.original_calls, 1u, __ATOMIC_RELAXED);
  reinterpret_cast<OriginalCallback>(original_address)(manager);
  __atomic_add_fetch(&g_evidence.original_returns, 1u, __ATOMIC_RELAXED);

  const std::uint32_t control_flags =
      __atomic_load_n(&g_control.flags, __ATOMIC_ACQUIRE);
  const std::uintptr_t expected_manager =
      __atomic_load_n(&g_control.expected_manager, __ATOMIC_ACQUIRE);
  if ((control_flags & camera::kConfigured) == 0 || expected_manager == 0 ||
      reinterpret_cast<std::uintptr_t>(manager) != expected_manager) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kInvalidControl);
    return;
  }

  const std::uintptr_t node =
      __atomic_load_n(&g_control.expected_node, __ATOMIC_ACQUIRE);
  const std::uintptr_t source = *reinterpret_cast<const std::uintptr_t*>(
      expected_manager + kManagerSourceOffset);
  const std::uintptr_t shape = *reinterpret_cast<const std::uintptr_t*>(
      expected_manager + kManagerShapeOffset);
  if (!NodeIdentity(node, expected_manager,
                    __atomic_load_n(&g_control.expected_node_vptr,
                                    __ATOMIC_ACQUIRE))) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kUnexpectedNode);
    return;
  }
  if (shape == 0 || shape !=
      __atomic_load_n(&g_control.expected_shape, __ATOMIC_ACQUIRE)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kUnexpectedShape);
    return;
  }
  if (source == 0 || source !=
      __atomic_load_n(&g_control.expected_source, __ATOMIC_ACQUIRE)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kUnexpectedSource);
    return;
  }
  const std::uintptr_t source_vptr =
      *reinterpret_cast<const std::uintptr_t*>(source);
  if (source_vptr !=
      __atomic_load_n(&g_control.expected_source_vptr, __ATOMIC_ACQUIRE)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kUnexpectedSourceVptr);
    return;
  }
  const std::uintptr_t position_setter =
      *reinterpret_cast<const std::uintptr_t*>(source_vptr +
                                               kSourcePositionSetterSlot);
  const std::uintptr_t rotation_setter =
      *reinterpret_cast<const std::uintptr_t*>(source_vptr +
                                               kSourceRotationSetterSlot);
  const std::uintptr_t fov_setter =
      __atomic_load_n(&g_control.expected_fov_setter, __ATOMIC_ACQUIRE);
  if (position_setter == 0 || rotation_setter == 0 || fov_setter == 0 ||
      position_setter != __atomic_load_n(&g_control.expected_position_setter,
                                         __ATOMIC_ACQUIRE) ||
      rotation_setter != __atomic_load_n(&g_control.expected_rotation_setter,
                                         __ATOMIC_ACQUIRE)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kUnexpectedSourceSetter);
    return;
  }

  void* embedded = reinterpret_cast<void*>(expected_manager + kManagerEmbeddedOffset);
  const std::uintptr_t embedded_vptr =
      *reinterpret_cast<const std::uintptr_t*>(embedded);
  if (embedded_vptr != __atomic_load_n(&g_control.expected_embedded_vptr,
                                       __ATOMIC_ACQUIRE)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kUnexpectedEmbeddedVptr);
    return;
  }
  const std::uintptr_t combined_address =
      *reinterpret_cast<const std::uintptr_t*>(embedded_vptr +
                                               kEmbeddedCombinedSlot);
  if (combined_address == 0 || combined_address !=
      __atomic_load_n(&g_control.expected_combined_function,
                      __ATOMIC_ACQUIRE)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kUnexpectedCombinedFunction);
    return;
  }

  const std::uint32_t tid = CurrentTid();
  const std::uint32_t expected_tid =
      __atomic_load_n(&g_control.expected_producer_tid, __ATOMIC_ACQUIRE);
  if (expected_tid != 0 && tid != expected_tid) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kUnexpectedThread);
    return;
  }
  std::uint32_t observed_tid =
      __atomic_load_n(&g_evidence.producer_tid, __ATOMIC_ACQUIRE);
  if (observed_tid == 0) {
    __atomic_compare_exchange_n(&g_evidence.producer_tid, &observed_tid, tid,
                                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    observed_tid = __atomic_load_n(&g_evidence.producer_tid, __ATOMIC_ACQUIRE);
  }
  if (observed_tid != tid) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kUnexpectedThread);
    return;
  }

  float natural_transform[7]{};
  float natural_fov{};
  CopyBytes(natural_transform,
            reinterpret_cast<const void*>(expected_manager + kManagerWorldOffset),
            kTransformBytes);
  CopyBytes(&natural_fov,
            reinterpret_cast<const void*>(expected_manager + kManagerFovOffset),
            sizeof(natural_fov));
  CopyBytes(g_evidence.natural_transform, natural_transform, kTransformBytes);
  CopyBytes(&g_evidence.natural_fov, &natural_fov, sizeof(natural_fov));

  __atomic_store_n(&g_evidence.last_manager, expected_manager, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_node, node, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_shape, shape, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_source, source, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_source_vptr, source_vptr,
                   __ATOMIC_RELAXED);

  if ((control_flags & camera::kActive) == 0) {
    CopyBytes(g_evidence.applied_transform, natural_transform, kTransformBytes);
    CopyBytes(&g_evidence.applied_fov, &natural_fov, sizeof(natural_fov));
    __atomic_add_fetch(&g_evidence.inactive_entries, 1u, __ATOMIC_RELAXED);
    SetStatus(camera::kInactive);
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    return;
  }

  CommandSnapshot command{};
  if (!ReadStableCommand(&command)) {
    __atomic_add_fetch(&g_evidence.command_busy_entries, 1u, __ATOMIC_RELAXED);
    SetStatus(camera::kCommandBusy);
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    return;
  }
  if ((command.flags & ~camera::kKnownOverrideMask) != 0 || command.flags == 0) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kInvalidOverrideFlags);
    return;
  }
  if ((command.flags & camera::kOverrideRelativeToCar) != 0) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kUnsupportedRelativeMode);
    return;
  }
  if (!FiniteCommand(command)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kNonFiniteCommand);
    return;
  }

  float applied_transform[7]{};
  CopyBytes(applied_transform, natural_transform, kTransformBytes);
  if ((command.flags & camera::kOverridePosition) != 0)
    CopyBytes(applied_transform, command.position, sizeof(command.position));
  if ((command.flags & camera::kOverrideRotation) != 0)
    CopyBytes(applied_transform + 3, command.rotation, sizeof(command.rotation));

  __atomic_add_fetch(&g_evidence.source_override_calls, 1u,
                     __ATOMIC_RELAXED);
  if ((command.flags & camera::kOverridePosition) != 0)
    reinterpret_cast<PositionSetter>(position_setter)(
        reinterpret_cast<void*>(source), command.position);
  if ((command.flags & camera::kOverrideRotation) != 0)
    reinterpret_cast<RotationSetter>(rotation_setter)(
        reinterpret_cast<void*>(source), command.rotation);
  if ((command.flags & camera::kOverrideFov) != 0) {
    const std::uint32_t fov_mode =
        *reinterpret_cast<const std::uint32_t*>(source + kSourceFovModeOffset);
    reinterpret_cast<FovSetter>(fov_setter)(reinterpret_cast<void*>(source),
                                            command.fov_radians, fov_mode);
  }
  __atomic_add_fetch(&g_evidence.source_override_returns, 1u,
                     __ATOMIC_RELAXED);

  bool source_readback_ok = true;
  if ((command.flags & camera::kOverridePosition) != 0)
    source_readback_ok = source_readback_ok &&
        __builtin_memcmp(reinterpret_cast<const void*>(
                             source + kSourcePositionOffset),
                         command.position, sizeof(command.position)) == 0;
  if ((command.flags & camera::kOverrideRotation) != 0)
    source_readback_ok = source_readback_ok &&
        __builtin_memcmp(reinterpret_cast<const void*>(
                             source + kSourceRotationOffset),
                         command.rotation, sizeof(command.rotation)) == 0;
  if ((command.flags & camera::kOverrideFov) != 0)
    source_readback_ok = source_readback_ok &&
        __builtin_memcmp(reinterpret_cast<const void*>(
                             source + kSourceFovOffset),
                         &command.fov_radians, sizeof(command.fov_radians)) == 0;
  if (!source_readback_ok) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(camera::kSourceReadbackMismatch);
    return;
  }
  __atomic_add_fetch(&g_evidence.source_readback_passes, 1u,
                     __ATOMIC_RELAXED);

  if ((command.flags & (camera::kOverridePosition |
                        camera::kOverrideRotation)) != 0) {
    __atomic_add_fetch(&g_evidence.combined_calls, 1u, __ATOMIC_RELAXED);
    reinterpret_cast<CombinedTransform>(combined_address)(embedded,
                                                          applied_transform);
    __atomic_add_fetch(&g_evidence.combined_returns, 1u, __ATOMIC_RELAXED);
  }

  float applied_fov = natural_fov;
  if ((command.flags & camera::kOverrideFov) != 0) {
    applied_fov = command.fov_radians;
    *reinterpret_cast<float*>(expected_manager + kManagerFovOffset) = applied_fov;
    __atomic_add_fetch(&g_evidence.fov_writes, 1u, __ATOMIC_RELAXED);
  }

  CopyBytes(g_evidence.applied_transform, applied_transform, kTransformBytes);
  CopyBytes(&g_evidence.applied_fov, &applied_fov, sizeof(applied_fov));
  __atomic_store_n(&g_evidence.last_command_sequence, command.sequence,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_override_flags, command.flags,
                   __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_evidence.active_entries, 1u, __ATOMIC_RELAXED);
  SetStatus(camera::kRunning);
  g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_camera_tool_arm_v1() {
  return -100;
}
