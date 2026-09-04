#pragma once

// Shared fixed-width ABI for a bounded dual-callback phase observer.  The
// optional payload records final-racer and RaceView callback ordering in one
// monotonic event stream.  It never supplies a camera replay state.

#include <cstddef>
#include <cstdint>

namespace a9tas::vehicle_camera_phase_observer_v1 {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr char kControlMagic[8] = {'A', '9', 'V', 'C', 'P', 'C', '1', 0};
inline constexpr char kEvidenceMagic[8] = {'A', '9', 'V', 'C', 'P', 'E', '1', 0};
inline constexpr std::uint32_t kMaximumEvents = 16384;

enum ControlFlag : std::uint32_t {
  kConfigured = 1u << 0,
  kObserveVehicle = 1u << 1,
  kObserveCamera = 1u << 2,
};

enum EventKind : std::uint32_t {
  // These two events bracket the complete proven final-writer wrapper.  The
  // inner wrapper itself calls the game original and applies any 64+12
  // correction before returning.
  kVehicleBeforeFinalWriter = 1,
  kVehicleAfterFinalWriter = 2,
  kReservedVehicleInnerPhase = 3,
  kCameraAfterOriginal = 4,
};

enum EventFlag : std::uint32_t {
  kClockValid = 1u << 0,
  kOriginalReturned = 1u << 1,
  kVehicleEqual = 1u << 2,
  kVehicleCorrected = 1u << 3,
  kVehicleImmediateExact = 1u << 4,
  kManagerIdentity = 1u << 5,
  kNodeIdentity = 1u << 6,
  kShapeIdentity = 1u << 7,
  // Camera event also contains a read-only snapshot hash of the final-writer
  // native 64+12 target after the original RaceView callback returned.
  kVehicleSnapshotPresent = 1u << 8,
};

enum Status : std::int32_t {
  kPassive = 0,
  kRecording = 1,
  kComplete = 2,
  kRejectedBuildOnly = -100,
  kInvalidControl = -201,
  kUnexpectedManager = -202,
  kUnexpectedNode = -203,
  kUnexpectedShape = -204,
  kRecursiveCameraEntry = -205,
  kEventOverflow = -206,
};

struct alignas(16) Event {
  std::uint64_t sequence;
  std::uint64_t monotonic_ns;
  std::uint32_t kind;
  std::uint32_t flags;
  std::uint32_t producer_tid;
  std::uint32_t vehicle_frame;
  std::uint32_t camera_frame;
  std::uint32_t reserved0;
  std::uint64_t vehicle_pose_hash;
  std::uint64_t vehicle_linear_hash;
  std::uint64_t camera_world_hash;
  std::uint64_t camera_shape_hash;
  std::uint32_t camera_fov_bits;
  std::uint32_t reserved1;
};

struct alignas(64) Control {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t maximum_events;
  std::uintptr_t expected_manager;
  std::uintptr_t expected_node;
  std::uintptr_t expected_shape;
  std::uintptr_t original_camera_callback;
  std::uintptr_t expected_node_vptr;
  std::uint64_t reserved[8];
};

struct alignas(64) Evidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t next_sequence;
  std::uint64_t committed_events;
  std::uint64_t dropped_events;
  std::uint64_t camera_entries;
  std::uint64_t camera_original_calls;
  std::uint64_t camera_original_returns;
  std::uint64_t camera_failures;
  std::uint64_t camera_recursive_entries;
  std::uint64_t vehicle_before_events;
  std::uint64_t vehicle_after_events;
  std::uint64_t vehicle_corrected_events;
  std::uint32_t camera_frames;
  std::int32_t last_status;
  std::uint32_t last_tid;
  std::uint32_t reserved0;
  std::uintptr_t last_manager;
  std::uintptr_t last_node;
  std::uintptr_t last_shape;
  std::uint64_t reserved[5];
};

static_assert(sizeof(Event) == 80, "vehicle/camera phase event ABI");
static_assert(sizeof(Control) == 128, "vehicle/camera phase control ABI");
static_assert(sizeof(Evidence) == 192, "vehicle/camera phase evidence ABI");
static_assert(offsetof(Control, expected_manager) == 24,
              "vehicle/camera phase manager offset");
static_assert(offsetof(Evidence, camera_frames) == 104,
              "vehicle/camera phase camera cursor offset");

}  // namespace a9tas::vehicle_camera_phase_observer_v1
