#pragma once

// Phase-aligned Camera Tool runtime.  The UI publishes input state; the ARM64
// CameraUpdate callback computes the camera in-process.  Normal TAS replay data
// remains completely separate.

#include <cstddef>
#include <cstdint>

namespace a9tas::camera_tool_runtime_v2 {

inline constexpr std::uint32_t kVersion = 2;
inline constexpr char kControlMagic[8] = {'A','9','C','T','C','2',0,0};
inline constexpr char kEvidenceMagic[8] = {'A','9','C','T','E','2',0,0};

enum ControlFlag : std::uint32_t {
  kConfigured = 1u << 0,
  kActive = 1u << 1,
};

enum class Mode : std::uint32_t {
  kAbsolute = 1,
  kFreeFlight = 2,
  kOrbital = 3,
};

enum InputBit : std::uint32_t {
  kForward = 1u << 0,
  kBackward = 1u << 1,
  kLeft = 1u << 2,
  kRight = 1u << 3,
  kUp = 1u << 4,
  kDown = 1u << 5,
  kZoomIn = 1u << 6,
  kZoomOut = 1u << 7,
};

inline constexpr std::uint32_t kKnownInputMask =
    kForward | kBackward | kLeft | kRight | kUp | kDown |
    kZoomIn | kZoomOut;

enum OverrideFlag : std::uint32_t {
  kOverridePosition = 1u << 0,
  kOverrideRotation = 1u << 1,
  kOverrideFov = 1u << 2,
};
inline constexpr std::uint32_t kAbsoluteOverrideMask =
    kOverridePosition | kOverrideRotation | kOverrideFov;

enum Status : std::int32_t {
  kPassive = 0,
  kRunning = 1,
  kInactive = 2,
  kCommandBusy = 3,
  kInvalidControl = -501,
  kUnexpectedIdentity = -502,
  kUnexpectedThread = -503,
  kInvalidMode = -504,
  kInvalidInput = -505,
  kNonFiniteCommand = -506,
  kUnexpectedVehicle = -507,
  kSourceReadbackMismatch = -508,
  kUnexpectedHudIdentity = -509,
};

struct alignas(64) Control {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t reserved0;
  std::uintptr_t expected_manager;
  std::uintptr_t expected_node;
  std::uintptr_t expected_shape;
  std::uintptr_t original_callback;
  std::uintptr_t expected_node_vptr;
  std::uintptr_t expected_embedded_vptr;
  std::uintptr_t expected_combined_function;
  std::uintptr_t expected_source;
  std::uintptr_t expected_source_vptr;
  std::uintptr_t expected_position_setter;
  std::uintptr_t expected_rotation_setter;
  std::uintptr_t expected_fov_setter;
  std::uint32_t expected_producer_tid;
  std::uint32_t reserved1;
  std::uintptr_t vehicle_physics_base;
  std::uintptr_t vehicle_backend_interface;
  std::uintptr_t vehicle_velocity_interface;
  std::uintptr_t vehicle_native_body;
  std::uintptr_t vehicle_native_vptr;
  std::uint64_t published_command;
  struct Command {
    Mode mode;
    std::uint32_t input_bits;
    float yaw_degrees;
    float pitch_degrees;
    float move_speed;
    float sensitivity;
    float orbital_distance;
    float orbital_zoom_speed;
    float fov_radians;
    std::uint32_t absolute_override_flags;
    float absolute_position[3];
    float absolute_rotation[4];
    std::uint32_t reserved[3];
  } commands[2];
  std::uintptr_t expected_hud_root;
  std::uintptr_t expected_hud_root_vptr;
  std::uintptr_t expected_hud_interface;
  std::uintptr_t expected_hud_interface_vptr;
  // Exact shared HUD visibility query.  Its first instruction is patched with
  // one aligned ARM64 branch.  The payload emulates the original LDRB/RET for
  // every non-target object and overrides only expected_hud_root.
  std::uintptr_t expected_hud_hidden_getter;
  // Payload wrapper reached by the temporary entry branch above.
  std::uintptr_t expected_hud_visibility_wrapper;
  // Bits 63..2 are a monotonic ordinal, bit 1 means the override is active,
  // and bit 0 is the upstream RenderGUIToggle visible result.
  std::uint64_t published_hud_visibility;
  std::uint64_t hud_original_code;
  std::uint32_t hud_patch_instruction;
  // 0=settled, 1=flush installed branch, 2=flush restored instruction.
  std::uint32_t hud_cache_flush_pending;
  std::uintptr_t hud_cache_flush_target;
  std::uint64_t hud_cache_flush_sequence;
  std::uint8_t hud_direct_reserved[24];
  // Kept at the old offset/size so the current v2 control layout and payload
  // storage extent remain stable while the rejected shadow-vtable route is
  // removed.  It is never copied to or published as a vtable.
  alignas(64) std::uint8_t hud_reserved[0x1000];
};

inline constexpr std::uintptr_t kHudHiddenSlot = 0x170;
inline constexpr std::uint32_t kHudFlushSettled = 0;
inline constexpr std::uint32_t kHudFlushInstall = 1;
inline constexpr std::uint32_t kHudFlushRestore = 2;
inline constexpr std::uint64_t kHudPublicationActive = 1u << 1;

static_assert(sizeof(Control::Command) == 80);

/*
 * published_command encodes a monotonic publication ordinal in bits 63..1
 * and the active command slot in bit 0.  The host writes the inactive 80-byte
 * slot completely, verifies it, then publishes this single 8-byte word.
 */

struct alignas(64) Evidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t wrapper_entries;
  std::uint64_t original_calls;
  std::uint64_t original_returns;
  std::uint64_t active_entries;
  std::uint64_t inactive_entries;
  std::uint64_t recursive_entries;
  std::uint64_t source_calls;
  std::uint64_t source_returns;
  std::uint64_t source_readback_passes;
  std::uint64_t combined_calls;
  std::uint64_t combined_returns;
  std::uint64_t failures;
  std::uint64_t command_busy_entries;
  std::uint64_t runtime_steps;
  std::uint64_t mode_transitions;
  std::uint64_t vehicle_reads;
  std::uint64_t input_snapshots;
  std::uint64_t last_command_sequence;
  std::uint32_t producer_tid;
  std::int32_t last_status;
  Mode last_mode;
  std::uint32_t last_input_bits;
  float natural_transform[7];
  float applied_transform[7];
  float natural_fov;
  float applied_fov;
  float runtime_move_speed;
  float runtime_orbital_distance;
  float last_dt_seconds;
  float max_dt_seconds;
  float last_vehicle_target_game[3];
  std::uint32_t reserved0;
  std::uint64_t hud_calls;
  std::uint64_t hud_returns;
  std::uint64_t hud_failures;
  std::uint64_t last_hud_publication;
  std::uint32_t last_hud_visible;
  std::uint32_t reserved1;
  std::uint64_t hud_cache_flushes;
};

static_assert(sizeof(Control) == 4544);
static_assert(offsetof(Control, published_command) == 168);
static_assert(offsetof(Control, commands) == 176);
static_assert(offsetof(Control, hud_original_code) == 392);
static_assert(offsetof(Control, hud_cache_flush_pending) == 404);
static_assert(offsetof(Control, hud_reserved) == 448);
static_assert(sizeof(Evidence) == 320);

}  // namespace a9tas::camera_tool_runtime_v2
