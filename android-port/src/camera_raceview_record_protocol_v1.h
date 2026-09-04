#pragma once

// Fixed-width ABI for the record-only RaceView callback sidecar. The payload
// records exact post-original manager, shape and FOV state; it has no camera
// replay branch and no installer.

#include <cstddef>
#include <cstdint>

namespace a9tas::camera_raceview_record_v1 {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr char kControlMagic[8] = {'A', '9', 'C', 'R', 'C', '1', 0, 0};
inline constexpr char kEvidenceMagic[8] = {'A', '9', 'C', 'R', 'E', '1', 0, 0};
inline constexpr std::uint32_t kMaximumFrames = 3600;
inline constexpr std::uint64_t kPermitDisarmed = 0;
inline constexpr std::uint64_t kContinuousPermit = UINT64_MAX;

inline constexpr std::uint64_t FramePermit(std::uint32_t frame_index) {
  return static_cast<std::uint64_t>(frame_index) + 1u;
}

enum ControlFlag : std::uint32_t {
  kConfigured = 1u << 0,
  // Record every post-original RaceView callback until frame_count is reached.
  // This matches upstream Detour_CameraUpdate's continuous observation path.
  kContinuousCapture = 1u << 1,
};

enum FrameFlag : std::uint32_t {
  kOriginalReturned = 1u << 0,
  kManagerIdentity = 1u << 1,
  kNodeIdentity = 1u << 2,
  kShapeIdentity = 1u << 3,
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
  kUnexpectedThread = -205,
  kFrameOverflow = -206,
  kRecursiveEntry = -207,
};

struct alignas(16) Frame {
  std::uint32_t frame_index;
  std::uint32_t producer_tid;
  std::uint64_t claimed_permit;
  std::uint32_t flags;
  std::uint32_t reserved;
  float manager_local[7];
  float manager_world[7];
  float shape_final[7];
  float fov_radians;
};

struct alignas(64) Control {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t frame_count;
  std::uintptr_t expected_manager;
  std::uintptr_t expected_node;
  std::uintptr_t expected_shape;
  std::uintptr_t original_callback;
  std::uintptr_t expected_node_vptr;
  std::uint64_t frame_permit;
  std::uint32_t expected_producer_tid;
  std::uint32_t reserved0;
  std::uint64_t reserved[6];
};

struct alignas(64) Evidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t wrapper_entries;
  std::uint64_t original_calls;
  std::uint64_t original_returns;
  std::uint64_t idle_entries;
  std::uint64_t claimed_permits;
  std::uint64_t recorded_frames;
  std::uint64_t failures;
  std::uint64_t recursive_entries;
  std::uint32_t processed_frames;
  std::int32_t last_status;
  std::uintptr_t last_manager;
  std::uintptr_t last_node;
  std::uintptr_t last_shape;
  std::uint32_t last_tid;
  std::uint32_t last_flags;
  std::uint64_t reserved;
};

static_assert(sizeof(Frame) == 112, "RaceView record frame ABI");
static_assert(sizeof(Control) == 128, "RaceView record control ABI");
static_assert(sizeof(Evidence) == 128, "RaceView record evidence ABI");
static_assert(offsetof(Control, frame_permit) == 64,
              "RaceView record permit offset");
static_assert(offsetof(Evidence, processed_frames) == 80,
              "RaceView record cursor offset");

}  // namespace a9tas::camera_raceview_record_v1
