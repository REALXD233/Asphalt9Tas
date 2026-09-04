// Phase-aligned Camera Tool runtime for the exact Android RaceView callback.
// The host publishes only a stable input snapshot.  Camera integration,
// optional vehicle target sampling, source setters and the combined submit all
// execute in this callback after the game's natural camera update.

#include "camera_raceview_callback_node_v1.h"
#include "camera_tool_runtime_core_v2.h"
#include "camera_tool_runtime_protocol_v2.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <sys/syscall.h>
#include <unistd.h>

namespace callback = a9tas::camera_raceview_callback_node_v1;
namespace core = a9tas::camera_tool_runtime_core_v2;
namespace protocol = a9tas::camera_tool_runtime_v2;

extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_camera_tool_runtime_callback_v2(void* manager);

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
constexpr std::uintptr_t kVehicleBackendInterfaceOffset = 0x30;
constexpr std::uintptr_t kVehicleNativeBodyOffset = 0x90;
constexpr std::uintptr_t kVehicleNativePoseOffset = 0x10;
constexpr std::size_t kTransformBytes = 7 * sizeof(float);
constexpr float kDefaultFovRadians = 0.959931076f;

constexpr protocol::Control InitialControl() {
  protocol::Control value{};
  for (std::size_t index = 0; index < 8; ++index)
    value.magic[index] = protocol::kControlMagic[index];
  value.version = protocol::kVersion;
  value.size = sizeof(protocol::Control);
  for (auto& command : value.commands) {
    command.mode = protocol::Mode::kAbsolute;
    command.fov_radians = kDefaultFovRadians;
    command.absolute_override_flags = protocol::kAbsoluteOverrideMask;
    command.absolute_rotation[3] = 1.0f;
  }
  return value;
}

constexpr protocol::Evidence InitialEvidence() {
  protocol::Evidence value{};
  for (std::size_t index = 0; index < 8; ++index)
    value.magic[index] = protocol::kEvidenceMagic[index];
  value.version = protocol::kVersion;
  value.size = sizeof(protocol::Evidence);
  value.last_status = protocol::kPassive;
  value.last_mode = protocol::Mode::kAbsolute;
  value.natural_transform[6] = 1.0f;
  value.applied_transform[6] = 1.0f;
  return value;
}

alignas(64) protocol::Control g_control = InitialControl();
alignas(64) protocol::Evidence g_evidence = InitialEvidence();

std::atomic<std::uint32_t> g_wrapper_depth{0};
core::State g_runtime_state{};
std::uint64_t g_last_time_ns{};
bool g_have_last_command{};
bool g_have_requested_move_speed{};
float g_requested_move_speed{};

struct CommandSnapshot {
  std::uint64_t sequence{};
  protocol::Mode mode{protocol::Mode::kAbsolute};
  std::uint32_t input_bits{};
  float yaw_degrees{};
  float pitch_degrees{};
  float move_speed{};
  float sensitivity{};
  float orbital_distance{};
  float orbital_zoom_speed{};
  float fov_radians{};
  std::uint32_t absolute_override_flags{};
  float absolute_position[3]{};
  float absolute_rotation[4]{};
};

CommandSnapshot g_last_complete_command{};

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

std::uint64_t MonotonicNanoseconds() {
  timespec value{};
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
  return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ull +
      static_cast<std::uint64_t>(value.tv_nsec);
}

bool Finite(const float* values, std::size_t count) {
  for (std::size_t index = 0; index < count; ++index)
    if (!std::isfinite(values[index]) || std::fabs(values[index]) > 1000000.0f)
      return false;
  return true;
}

bool HeaderValid() {
  return __builtin_memcmp(g_control.magic, protocol::kControlMagic, 8) == 0 &&
      g_control.version == protocol::kVersion &&
      g_control.size == sizeof(protocol::Control);
}

bool NodeIdentity(std::uintptr_t node, std::uintptr_t manager,
                  std::uintptr_t expected_vptr) {
  if (node == 0 || expected_vptr == 0) return false;
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(node);
  const auto callback_address = reinterpret_cast<std::uintptr_t>(
      &a9tas_camera_tool_runtime_callback_v2);
  return *reinterpret_cast<const std::uintptr_t*>(node) == expected_vptr &&
      bytes[callback::kEnabledOffset] == 1 &&
      *reinterpret_cast<const std::uintptr_t*>(node + callback::kOwnerOffset) ==
          manager &&
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
        __atomic_load_n(&g_control.published_command, __ATOMIC_ACQUIRE);
    const std::size_t slot = static_cast<std::size_t>(before & 1u);
    const protocol::Control::Command& source = g_control.commands[slot];
    CommandSnapshot value{};
    value.sequence = before;
    CopyBytes(&value.mode, &source.mode, sizeof(value.mode));
    CopyBytes(&value.input_bits, &source.input_bits, sizeof(value.input_bits));
    CopyBytes(&value.yaw_degrees, &source.yaw_degrees,
              sizeof(value.yaw_degrees));
    CopyBytes(&value.pitch_degrees, &source.pitch_degrees,
              sizeof(value.pitch_degrees));
    CopyBytes(&value.move_speed, &source.move_speed, sizeof(value.move_speed));
    CopyBytes(&value.sensitivity, &source.sensitivity,
              sizeof(value.sensitivity));
    CopyBytes(&value.orbital_distance, &source.orbital_distance,
              sizeof(value.orbital_distance));
    CopyBytes(&value.orbital_zoom_speed, &source.orbital_zoom_speed,
              sizeof(value.orbital_zoom_speed));
    CopyBytes(&value.fov_radians, &source.fov_radians,
              sizeof(value.fov_radians));
    CopyBytes(&value.absolute_override_flags,
              &source.absolute_override_flags,
              sizeof(value.absolute_override_flags));
    CopyBytes(value.absolute_position, source.absolute_position,
              sizeof(value.absolute_position));
    CopyBytes(value.absolute_rotation, source.absolute_rotation,
              sizeof(value.absolute_rotation));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const std::uint64_t after =
        __atomic_load_n(&g_control.published_command, __ATOMIC_ACQUIRE);
    if (before == after) {
      *output = value;
      return true;
    }
  }
  return false;
}

bool CommandValid(const CommandSnapshot& value) {
  const float scalars[] = {
      value.yaw_degrees, value.pitch_degrees, value.move_speed,
      value.sensitivity, value.orbital_distance, value.orbital_zoom_speed,
      value.fov_radians,
  };
  if ((value.input_bits & ~protocol::kKnownInputMask) != 0 ||
      !Finite(scalars, std::size(scalars)) || value.move_speed < 0.1f ||
      value.move_speed > 1000.0f || value.orbital_distance < 0.1f ||
      value.orbital_zoom_speed < 0.0f || value.fov_radians <= 0.0f)
    return false;
  if (value.mode == protocol::Mode::kAbsolute) {
    if (value.absolute_override_flags == 0 ||
        (value.absolute_override_flags & ~protocol::kAbsoluteOverrideMask) != 0)
      return false;
    if ((value.absolute_override_flags & protocol::kOverridePosition) != 0 &&
        !Finite(value.absolute_position, 3)) return false;
    if ((value.absolute_override_flags & protocol::kOverrideRotation) != 0 &&
        !Finite(value.absolute_rotation, 4)) return false;
    return true;
  }
  return value.mode == protocol::Mode::kFreeFlight ||
      value.mode == protocol::Mode::kOrbital;
}

bool ReadVehicleTarget(core::Vec3* output) {
  if (output == nullptr) return false;
  const std::uintptr_t physics_base = __atomic_load_n(
      &g_control.vehicle_physics_base, __ATOMIC_ACQUIRE);
  const std::uintptr_t backend_interface = __atomic_load_n(
      &g_control.vehicle_backend_interface, __ATOMIC_ACQUIRE);
  const std::uintptr_t velocity_interface = __atomic_load_n(
      &g_control.vehicle_velocity_interface, __ATOMIC_ACQUIRE);
  const std::uintptr_t native_body = __atomic_load_n(
      &g_control.vehicle_native_body, __ATOMIC_ACQUIRE);
  const std::uintptr_t native_vptr = __atomic_load_n(
      &g_control.vehicle_native_vptr, __ATOMIC_ACQUIRE);
  if (physics_base == 0 || backend_interface == 0 || velocity_interface == 0 ||
      native_body == 0 || native_vptr == 0 ||
      *reinterpret_cast<const std::uintptr_t*>(
          physics_base + kVehicleBackendInterfaceOffset) != backend_interface ||
      *reinterpret_cast<const std::uintptr_t*>(
          velocity_interface + kVehicleNativeBodyOffset) != native_body ||
      *reinterpret_cast<const std::uintptr_t*>(native_body) != native_vptr)
    return false;
  float transform[16]{};
  CopyBytes(transform,
            reinterpret_cast<const void*>(native_body + kVehicleNativePoseOffset),
            sizeof(transform));
  if (!Finite(transform, 16)) return false;
  *output = {transform[12], transform[13], transform[14]};
  return true;
}

core::Camera NaturalCamera(const float transform[7], float fov) {
  core::Camera result{};
  result.position = core::GameToOpenGl(
      {transform[0], transform[1], transform[2]});
  result.rotation = core::GameToOpenGl(
      transform[3], transform[4], transform[5], transform[6]);
  result.fov_radians = fov;
  return result;
}

void ToGameTransform(const core::Camera& value, float output[7]) {
  const core::Vec3 position = core::OpenGlToGame(value.position);
  output[0] = position.x;
  output[1] = position.y;
  output[2] = position.z;
  core::OpenGlToGame(value.rotation, output + 3);
}

void FlushHudInstructionCacheIfPending() {
  const std::uint32_t pending = __atomic_load_n(
      &g_control.hud_cache_flush_pending, __ATOMIC_ACQUIRE);
  if (pending != protocol::kHudFlushInstall &&
      pending != protocol::kHudFlushRestore)
    return;
  const std::uintptr_t target = __atomic_load_n(
      &g_control.hud_cache_flush_target, __ATOMIC_ACQUIRE);
  const std::uint32_t patch = __atomic_load_n(
      &g_control.hud_patch_instruction, __ATOMIC_ACQUIRE);
  const std::uint64_t original = __atomic_load_n(
      &g_control.hud_original_code, __ATOMIC_ACQUIRE);
  const std::uint32_t expected = pending == protocol::kHudFlushInstall ?
      patch : static_cast<std::uint32_t>(original);
  if (target == 0 || patch == 0 || original == 0 ||
      *reinterpret_cast<const volatile std::uint32_t*>(target) != expected)
    return;
  __builtin___clear_cache(reinterpret_cast<char*>(target),
                          reinterpret_cast<char*>(target + sizeof(expected)));
  std::uint32_t observed = pending;
  if (__atomic_compare_exchange_n(&g_control.hud_cache_flush_pending,
                                  &observed, protocol::kHudFlushSettled,
                                  false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    __atomic_add_fetch(&g_evidence.hud_cache_flushes, 1u, __ATOMIC_RELAXED);
  }
}

}  // namespace

extern "C" __attribute__((visibility("default"), used)) protocol::Control*
    a9tas_camera_tool_runtime_control_storage_v2 = &g_control;
extern "C" __attribute__((visibility("default"), used)) protocol::Evidence*
    a9tas_camera_tool_runtime_evidence_storage_v2 = &g_evidence;

extern "C" __attribute__((noinline, visibility("default"), used)) std::uint32_t
a9tas_camera_tool_hud_visible_getter_v1(void* object) {
  __atomic_add_fetch(&g_evidence.hud_calls, 1u, __ATOMIC_RELAXED);
  const std::uintptr_t expected_root = __atomic_load_n(
      &g_control.expected_hud_root, __ATOMIC_ACQUIRE);
  const std::uint64_t original_code = __atomic_load_n(
      &g_control.hud_original_code, __ATOMIC_ACQUIRE);
  const std::uint32_t load = static_cast<std::uint32_t>(original_code);
  const std::uint32_t ret = static_cast<std::uint32_t>(original_code >> 32);
  // LDRB W0,[X0,#imm12] followed by RET.  The getter is shared by thousands
  // of vtables, so every object other than the exact HUD root must retain the
  // original leaf semantics instead of being forced visible or hidden.
  if (object == nullptr || expected_root == 0 ||
      (load & 0xFFC003FFu) != 0x39400000u || ret != 0xD65F03C0u) {
    __atomic_add_fetch(&g_evidence.hud_failures, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_evidence.hud_returns, 1u, __ATOMIC_RELAXED);
    return 1u;
  }

  const std::uint64_t publication = __atomic_load_n(
      &g_control.published_hud_visibility, __ATOMIC_ACQUIRE);
  std::uint32_t visible = 0;
  if (reinterpret_cast<std::uintptr_t>(object) == expected_root &&
      (publication & protocol::kHudPublicationActive) != 0) {
    visible = static_cast<std::uint32_t>(publication & 1u);
  } else {
    const std::uint32_t offset = (load >> 10) & 0xFFFu;
    visible = reinterpret_cast<const volatile std::uint8_t*>(object)[offset] & 1u;
  }
  __atomic_store_n(&g_evidence.last_hud_publication, publication,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.last_hud_visible, visible, __ATOMIC_RELEASE);
  __atomic_add_fetch(&g_evidence.hud_returns, 1u, __ATOMIC_RELAXED);
  return visible;
}

extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_camera_tool_runtime_callback_v2(void* manager) {
  FlushHudInstructionCacheIfPending();
  __atomic_add_fetch(&g_evidence.wrapper_entries, 1u, __ATOMIC_RELAXED);
  const std::uintptr_t original_address =
      __atomic_load_n(&g_control.original_callback, __ATOMIC_ACQUIRE);
  if (manager == nullptr || original_address == 0 ||
      original_address == reinterpret_cast<std::uintptr_t>(
                              &a9tas_camera_tool_runtime_callback_v2)) {
    Fail(protocol::kInvalidControl);
    return;
  }

  const std::uint32_t previous_depth =
      g_wrapper_depth.fetch_add(1u, std::memory_order_acq_rel);
  if (previous_depth != 0) {
    __atomic_add_fetch(&g_evidence.recursive_entries, 1u, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_evidence.original_calls, 1u, __ATOMIC_RELAXED);
    reinterpret_cast<OriginalCallback>(original_address)(manager);
    __atomic_add_fetch(&g_evidence.original_returns, 1u, __ATOMIC_RELAXED);
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    return;
  }

  __atomic_add_fetch(&g_evidence.original_calls, 1u, __ATOMIC_RELAXED);
  reinterpret_cast<OriginalCallback>(original_address)(manager);
  __atomic_add_fetch(&g_evidence.original_returns, 1u, __ATOMIC_RELAXED);

  const std::uint32_t control_flags =
      __atomic_load_n(&g_control.flags, __ATOMIC_ACQUIRE);
  const std::uintptr_t expected_manager =
      __atomic_load_n(&g_control.expected_manager, __ATOMIC_ACQUIRE);
  if (!HeaderValid() || (control_flags & protocol::kConfigured) == 0 ||
      (control_flags & ~(protocol::kConfigured | protocol::kActive)) != 0 ||
      expected_manager == 0 ||
      reinterpret_cast<std::uintptr_t>(manager) != expected_manager) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(protocol::kInvalidControl);
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
                                    __ATOMIC_ACQUIRE)) ||
      shape == 0 ||
      shape != __atomic_load_n(&g_control.expected_shape, __ATOMIC_ACQUIRE) ||
      source == 0 ||
      source != __atomic_load_n(&g_control.expected_source, __ATOMIC_ACQUIRE)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(protocol::kUnexpectedIdentity);
    return;
  }

  const std::uintptr_t source_vptr =
      *reinterpret_cast<const std::uintptr_t*>(source);
  const std::uintptr_t position_setter = source_vptr == 0 ? 0 :
      *reinterpret_cast<const std::uintptr_t*>(
          source_vptr + kSourcePositionSetterSlot);
  const std::uintptr_t rotation_setter = source_vptr == 0 ? 0 :
      *reinterpret_cast<const std::uintptr_t*>(
          source_vptr + kSourceRotationSetterSlot);
  const std::uintptr_t fov_setter =
      __atomic_load_n(&g_control.expected_fov_setter, __ATOMIC_ACQUIRE);
  void* embedded = reinterpret_cast<void*>(
      expected_manager + kManagerEmbeddedOffset);
  const std::uintptr_t embedded_vptr =
      *reinterpret_cast<const std::uintptr_t*>(embedded);
  const std::uintptr_t combined_address = embedded_vptr == 0 ? 0 :
      *reinterpret_cast<const std::uintptr_t*>(
          embedded_vptr + kEmbeddedCombinedSlot);
  if (source_vptr != __atomic_load_n(&g_control.expected_source_vptr,
                                     __ATOMIC_ACQUIRE) ||
      position_setter == 0 ||
      position_setter != __atomic_load_n(&g_control.expected_position_setter,
                                         __ATOMIC_ACQUIRE) ||
      rotation_setter == 0 ||
      rotation_setter != __atomic_load_n(&g_control.expected_rotation_setter,
                                         __ATOMIC_ACQUIRE) ||
      fov_setter == 0 ||
      embedded_vptr != __atomic_load_n(&g_control.expected_embedded_vptr,
                                       __ATOMIC_ACQUIRE) ||
      combined_address == 0 ||
      combined_address != __atomic_load_n(
          &g_control.expected_combined_function, __ATOMIC_ACQUIRE)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(protocol::kUnexpectedIdentity);
    return;
  }

  const std::uint32_t tid = CurrentTid();
  const std::uint32_t expected_tid =
      __atomic_load_n(&g_control.expected_producer_tid, __ATOMIC_ACQUIRE);
  std::uint32_t observed_tid =
      __atomic_load_n(&g_evidence.producer_tid, __ATOMIC_ACQUIRE);
  if (expected_tid != 0 && expected_tid != tid) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(protocol::kUnexpectedThread);
    return;
  }
  if (observed_tid == 0) {
    __atomic_compare_exchange_n(&g_evidence.producer_tid, &observed_tid, tid,
                                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    observed_tid = __atomic_load_n(&g_evidence.producer_tid, __ATOMIC_ACQUIRE);
  }
  if (observed_tid != tid) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(protocol::kUnexpectedThread);
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
  if (!Finite(natural_transform, 7) || !Finite(&natural_fov, 1)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(protocol::kInvalidControl);
    return;
  }
  CopyBytes(g_evidence.natural_transform, natural_transform, kTransformBytes);
  CopyBytes(&g_evidence.natural_fov, &natural_fov, sizeof(natural_fov));

  if ((control_flags & protocol::kActive) == 0) {
    g_runtime_state = {};
    g_last_time_ns = 0;
    g_have_last_command = false;
    g_have_requested_move_speed = false;
    CopyBytes(g_evidence.applied_transform, natural_transform, kTransformBytes);
    CopyBytes(&g_evidence.applied_fov, &natural_fov, sizeof(natural_fov));
    __atomic_add_fetch(&g_evidence.inactive_entries, 1u, __ATOMIC_RELAXED);
    SetStatus(protocol::kInactive);
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    return;
  }

  CommandSnapshot command{};
  if (!ReadStableCommand(&command)) {
    __atomic_add_fetch(&g_evidence.command_busy_entries, 1u, __ATOMIC_RELAXED);
    if (!g_have_last_command) {
      SetStatus(protocol::kCommandBusy);
      g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
      return;
    }
    // A three-attempt collision is exceptionally rare with double buffering.
    // Preserve the last complete command rather than exposing one natural
    // camera frame.  The complete snapshot is retained below after every
    // successful read.
    command = g_last_complete_command;
  } else {
    g_last_complete_command = command;
    g_have_last_command = true;
  }
  __atomic_add_fetch(&g_evidence.input_snapshots, 1u, __ATOMIC_RELAXED);
  if (!CommandValid(command)) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(command.input_bits & ~protocol::kKnownInputMask ?
             protocol::kInvalidInput : protocol::kNonFiniteCommand);
    return;
  }

  const core::Camera natural = NaturalCamera(natural_transform, natural_fov);
  core::Camera applied = natural;
  const bool mode_transition =
      !g_runtime_state.initialized || g_runtime_state.mode != command.mode;
  const std::uint64_t now_ns = MonotonicNanoseconds();
  float dt_seconds = 0.0f;
  if (!mode_transition && g_last_time_ns != 0 && now_ns >= g_last_time_ns)
    dt_seconds = static_cast<float>(now_ns - g_last_time_ns) / 1000000000.0f;
  g_last_time_ns = now_ns;
  if (mode_transition)
    __atomic_add_fetch(&g_evidence.mode_transitions, 1u, __ATOMIC_RELAXED);

  std::uint32_t override_flags = protocol::kAbsoluteOverrideMask;
  if (command.mode == protocol::Mode::kAbsolute) {
    override_flags = command.absolute_override_flags;
    if ((override_flags & protocol::kOverridePosition) != 0)
      applied.position = core::GameToOpenGl({command.absolute_position[0],
                                            command.absolute_position[1],
                                            command.absolute_position[2]});
    if ((override_flags & protocol::kOverrideRotation) != 0)
      applied.rotation = core::GameToOpenGl(command.absolute_rotation[0],
                                            command.absolute_rotation[1],
                                            command.absolute_rotation[2],
                                            command.absolute_rotation[3]);
    if ((override_flags & protocol::kOverrideFov) != 0)
      applied.fov_radians = command.fov_radians;
    g_runtime_state = {};
  } else {
    core::Input input{};
    input.mode = command.mode;
    input.bits = command.input_bits;
    input.yaw_degrees = command.yaw_degrees;
    input.pitch_degrees = command.pitch_degrees;
    input.move_speed = command.move_speed;
    input.synchronize_move_speed = !g_have_requested_move_speed ||
        command.move_speed != g_requested_move_speed;
    g_requested_move_speed = command.move_speed;
    g_have_requested_move_speed = true;
    input.sensitivity = command.sensitivity;
    input.orbital_distance = command.orbital_distance;
    input.orbital_zoom_speed = command.orbital_zoom_speed;
    input.fov_radians = command.fov_radians > 0.0f ?
        command.fov_radians : kDefaultFovRadians;
    input.dt_seconds = dt_seconds;
    if (command.mode == protocol::Mode::kOrbital) {
      if (!ReadVehicleTarget(&input.vehicle_target_game)) {
        g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
        Fail(protocol::kUnexpectedVehicle);
        return;
      }
      CopyBytes(g_evidence.last_vehicle_target_game,
                &input.vehicle_target_game,
                sizeof(g_evidence.last_vehicle_target_game));
      __atomic_add_fetch(&g_evidence.vehicle_reads, 1u, __ATOMIC_RELAXED);
    }
    if (!core::Step(&g_runtime_state, input, natural, &applied)) {
      g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
      Fail(protocol::kInvalidMode);
      return;
    }
    __atomic_add_fetch(&g_evidence.runtime_steps, 1u, __ATOMIC_RELAXED);
  }

  float applied_transform[7]{};
  ToGameTransform(applied, applied_transform);
  __atomic_add_fetch(&g_evidence.source_calls, 1u, __ATOMIC_RELAXED);
  if ((override_flags & protocol::kOverridePosition) != 0)
    reinterpret_cast<PositionSetter>(position_setter)(
        reinterpret_cast<void*>(source), applied_transform);
  if ((override_flags & protocol::kOverrideRotation) != 0)
    reinterpret_cast<RotationSetter>(rotation_setter)(
        reinterpret_cast<void*>(source), applied_transform + 3);
  if ((override_flags & protocol::kOverrideFov) != 0) {
    const std::uint32_t fov_mode =
        *reinterpret_cast<const std::uint32_t*>(source + kSourceFovModeOffset);
    reinterpret_cast<FovSetter>(fov_setter)(reinterpret_cast<void*>(source),
                                            applied.fov_radians, fov_mode);
  }
  __atomic_add_fetch(&g_evidence.source_returns, 1u, __ATOMIC_RELAXED);

  bool readback_ok = true;
  if ((override_flags & protocol::kOverridePosition) != 0)
    readback_ok = __builtin_memcmp(
        reinterpret_cast<const void*>(source + kSourcePositionOffset),
        applied_transform, 3 * sizeof(float)) == 0;
  if ((override_flags & protocol::kOverrideRotation) != 0)
    readback_ok = readback_ok && __builtin_memcmp(
        reinterpret_cast<const void*>(source + kSourceRotationOffset),
        applied_transform + 3, 4 * sizeof(float)) == 0;
  if ((override_flags & protocol::kOverrideFov) != 0)
    readback_ok = readback_ok && __builtin_memcmp(
        reinterpret_cast<const void*>(source + kSourceFovOffset),
        &applied.fov_radians, sizeof(applied.fov_radians)) == 0;
  if (!readback_ok) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    Fail(protocol::kSourceReadbackMismatch);
    return;
  }
  __atomic_add_fetch(&g_evidence.source_readback_passes, 1u,
                     __ATOMIC_RELAXED);

  if ((override_flags & (protocol::kOverridePosition |
                         protocol::kOverrideRotation)) != 0) {
    __atomic_add_fetch(&g_evidence.combined_calls, 1u, __ATOMIC_RELAXED);
    reinterpret_cast<CombinedTransform>(combined_address)(embedded,
                                                          applied_transform);
    __atomic_add_fetch(&g_evidence.combined_returns, 1u, __ATOMIC_RELAXED);
  }
  if ((override_flags & protocol::kOverrideFov) != 0)
    *reinterpret_cast<float*>(expected_manager + kManagerFovOffset) =
        applied.fov_radians;

  CopyBytes(g_evidence.applied_transform, applied_transform, kTransformBytes);
  CopyBytes(&g_evidence.applied_fov, &applied.fov_radians,
            sizeof(applied.fov_radians));
  CopyBytes(&g_evidence.runtime_move_speed, &g_runtime_state.move_speed,
            sizeof(g_runtime_state.move_speed));
  CopyBytes(&g_evidence.runtime_orbital_distance,
            &g_runtime_state.orbital_distance,
            sizeof(g_runtime_state.orbital_distance));
  CopyBytes(&g_evidence.last_dt_seconds, &dt_seconds, sizeof(dt_seconds));
  if (dt_seconds > g_evidence.max_dt_seconds)
    CopyBytes(&g_evidence.max_dt_seconds, &dt_seconds, sizeof(dt_seconds));
  __atomic_store_n(&g_evidence.last_command_sequence, command.sequence,
                   __ATOMIC_RELAXED);
  CopyBytes(&g_evidence.last_mode, &command.mode, sizeof(command.mode));
  __atomic_store_n(&g_evidence.last_input_bits, command.input_bits,
                   __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_evidence.active_entries, 1u, __ATOMIC_RELAXED);
  SetStatus(protocol::kRunning);
  g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_camera_tool_runtime_arm_v2() {
  return -200;
}
