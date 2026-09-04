#pragma once

// Fixed-width ABI for the five-callback RaceView same-state writer gate.

#include <cstddef>
#include <cstdint>

namespace a9tas::camera_raceview_same_state_v1 {

inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaximumFrames = 5;
inline constexpr char kControlMagic[8] = {'A','9','C','S','C','1',0,0};
inline constexpr char kEvidenceMagic[8] = {'A','9','C','S','E','1',0,0};

enum ControlFlag : std::uint32_t {
  kConfigured = 1u << 0,
};

enum FrameFlag : std::uint32_t {
  kOriginalReturned = 1u << 0,
  kManagerIdentity = 1u << 1,
  kNodeIdentity = 1u << 2,
  kShapeIdentity = 1u << 3,
  kEmbeddedVptrIdentity = 1u << 4,
  kCombinedFunctionIdentity = 1u << 5,
  kCombinedReturned = 1u << 6,
  kFovSameBytes = 1u << 7,
};

enum Status : std::int32_t {
  kPassive = 0,
  kRunning = 1,
  kComplete = 2,
  kRejectedBuildOnly = -100,
  kInvalidControl = -301,
  kRecursiveEntry = -302,
  kUnexpectedThread = -303,
  kUnexpectedNode = -304,
  kUnexpectedShape = -305,
  kUnexpectedEmbeddedVptr = -306,
  kUnexpectedCombinedFunction = -307,
  kFrameOverflow = -308,
  kFovChanged = -309,
};

struct alignas(16) Frame {
  std::uint32_t frame_index;
  std::uint32_t producer_tid;
  std::uint32_t flags;
  std::uint32_t reserved0;
  float manager_world_before[7];
  float shape_before[7];
  float shape_after[7];
  float fov_before;
  float fov_after;
  std::uint32_t reserved1[3];
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
  std::uintptr_t expected_embedded_vptr;
  std::uintptr_t expected_combined_function;
  std::uint32_t expected_producer_tid;
  std::uint32_t reserved0;
  std::uint64_t reserved[4];
};

struct alignas(64) Evidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t wrapper_entries;
  std::uint64_t original_calls;
  std::uint64_t original_returns;
  std::uint64_t combined_calls;
  std::uint64_t combined_returns;
  std::uint64_t fov_same_writes;
  std::uint64_t recorded_frames;
  std::uint64_t idle_entries;
  std::uint64_t failures;
  std::uint64_t recursive_entries;
  std::uint32_t processed_frames;
  std::int32_t last_status;
  std::uintptr_t last_manager;
  std::uintptr_t last_node;
  std::uintptr_t last_shape;
};

static_assert(sizeof(Frame) == 128);
static_assert(sizeof(Control) == 128);
static_assert(sizeof(Evidence) == 128);
static_assert(offsetof(Evidence, processed_frames) == 96);

}  // namespace a9tas::camera_raceview_same_state_v1
