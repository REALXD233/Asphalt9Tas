#pragma once

// Android carrier for the standalone AluTasV2 Camera Tool.  This protocol is
// deliberately separate from the TAS replay data: upstream .REPLAY files
// do not contain a camera track.

#include <cstddef>
#include <cstdint>

namespace a9tas::camera_tool_v1 {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr char kControlMagic[8] = {'A','9','C','T','C','1',0,0};
inline constexpr char kEvidenceMagic[8] = {'A','9','C','T','E','1',0,0};

enum ControlFlag : std::uint32_t {
  kConfigured = 1u << 0,
  kActive = 1u << 1,
};

// These bit values intentionally match upstream WriteCameraState.
enum OverrideFlag : std::uint32_t {
  kOverridePosition = 1u << 0,
  kOverrideRotation = 1u << 1,
  kOverrideFov = 1u << 2,
  kOverrideRelativeToCar = 1u << 3,
};

inline constexpr std::uint32_t kAbsoluteOverrideMask =
    kOverridePosition | kOverrideRotation | kOverrideFov;
inline constexpr std::uint32_t kKnownOverrideMask =
    kAbsoluteOverrideMask | kOverrideRelativeToCar;

enum Status : std::int32_t {
  kPassive = 0,
  kRunning = 1,
  kInactive = 2,
  kCommandBusy = 3,
  kInvalidControl = -401,
  kRecursiveEntry = -402,
  kUnexpectedThread = -403,
  kUnexpectedNode = -404,
  kUnexpectedShape = -405,
  kUnexpectedEmbeddedVptr = -406,
  kUnexpectedCombinedFunction = -407,
  kInvalidOverrideFlags = -408,
  kUnsupportedRelativeMode = -409,
  kNonFiniteCommand = -410,
  kUnexpectedSource = -411,
  kUnexpectedSourceVptr = -412,
  kUnexpectedSourceSetter = -413,
  kSourceReadbackMismatch = -414,
};

// The host publishes a command with a seqlock: odd sequence, command bytes,
// then the next even sequence.  The callback never consumes a torn command.
struct alignas(64) Control {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t override_flags;
  std::uintptr_t expected_manager;
  std::uintptr_t expected_node;
  std::uintptr_t expected_shape;
  std::uintptr_t original_callback;
  std::uintptr_t expected_node_vptr;
  std::uintptr_t expected_embedded_vptr;
  std::uintptr_t expected_combined_function;
  std::uint32_t expected_producer_tid;
  std::uint32_t reserved0;
  std::uint64_t command_sequence;
  float position[3];
  float rotation[4];
  float fov_radians;
  float offset_relative_to_car[3];
  std::uint32_t look_backwards;
  std::uintptr_t expected_source;
  std::uintptr_t expected_source_vptr;
  std::uintptr_t expected_position_setter;
  std::uintptr_t expected_rotation_setter;
  std::uintptr_t expected_fov_setter;
  std::uint64_t reserved;
};

struct alignas(64) Evidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t wrapper_entries;
  std::uint64_t original_calls;
  std::uint64_t original_returns;
  std::uint64_t active_entries;
  std::uint64_t inactive_entries;
  std::uint64_t combined_calls;
  std::uint64_t combined_returns;
  std::uint64_t fov_writes;
  std::uint64_t command_busy_entries;
  std::uint64_t failures;
  std::uint64_t recursive_entries;
  std::uint32_t producer_tid;
  std::int32_t last_status;
  std::uint64_t last_command_sequence;
  std::uintptr_t last_manager;
  std::uintptr_t last_node;
  std::uintptr_t last_shape;
  std::uint32_t last_override_flags;
  std::uint32_t reserved0;
  float natural_transform[7];
  float applied_transform[7];
  float natural_fov;
  float applied_fov;
  std::uint64_t source_override_calls;
  std::uint64_t source_override_returns;
  std::uint64_t source_readback_passes;
  std::uintptr_t last_source;
  std::uintptr_t last_source_vptr;
};

static_assert(sizeof(Control) == 192);
static_assert(offsetof(Control, command_sequence) == 88);
static_assert(offsetof(Control, position) == 96);
static_assert(sizeof(Evidence) == 256);

}  // namespace a9tas::camera_tool_v1
