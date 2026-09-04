#pragma once

// Shared, fixed-width ABI for the finite AluTasV2-style final-writer replay
// payload and its future host controller.  This header contains data layout
// only: no process access, guest calls, or installation primitives.

#include <cstddef>
#include <cstdint>

namespace a9tas::final_writer_replay_v1 {

inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr char kControlMagic[8] = {'A', '9', 'F', 'W', 'R', 'C', '1', 0};
inline constexpr char kEvidenceMagic[8] = {'A', '9', 'F', 'W', 'R', 'E', '1', 0};

inline constexpr std::uintptr_t kPrimaryPrefixRva = 0x7EE8CC0;
inline constexpr std::uintptr_t kPrimaryAddressPointRva = 0x7EE8D18;
inline constexpr std::uintptr_t kNextAddressPointRva = 0x7EE95C8;
inline constexpr std::uintptr_t kOriginalCallbackRva = 0x367D66C;
inline constexpr std::size_t kPrimaryPrefixSize = 0x58;
inline constexpr std::size_t kPrimaryShadowSize = 0x908;
inline constexpr std::size_t kCallbackSlotOffset = 0x10;
inline constexpr std::size_t kTransformSize = 64;
inline constexpr std::size_t kLinearSize = 12;
inline constexpr std::size_t kComponentCount = 19;
inline constexpr std::uint32_t kMaximumFrames = 3600;
inline constexpr std::uint64_t kFramePermitDisarmed = 0;

inline constexpr std::uint64_t FramePermit(std::uint32_t frame_index) {
  return static_cast<std::uint64_t>(frame_index) + 1u;
}

enum Status : std::int32_t {
  kStatusPassive = 0,
  kStatusComplete = 1,
  kStatusRejectedBuildOnly = -100,
  kStatusUnexpectedObject = -201,
  kStatusInvalidControl = -202,
  kStatusUnexpectedVptr = -203,
  kStatusFrameOverflow = -204,
  kStatusRecursiveEntry = -205,
  kStatusImmediateAuditMismatch = -206,
};

enum ControlFlag : std::uint32_t {
  kControlConfigured = 1u << 0,
  kControlTargetsLoaded = 1u << 1,
};

enum AuditFlag : std::uint32_t {
  kAuditOriginalReturned = 1u << 0,
  kAuditEqual = 1u << 1,
  kAuditCorrected = 1u << 2,
  kAuditImmediateExact = 1u << 3,
  kAuditFinalFrame = 1u << 4,
  kAuditVptrRestored = 1u << 5,
};

struct alignas(16) FrameTarget {
  std::uint8_t transform[kTransformSize];
  std::uint8_t linear[kLinearSize];
  std::uint32_t reserved;
};

struct alignas(16) FrameAudit {
  std::uint32_t frame_index;
  std::uint32_t flags;
  std::uint8_t before_transform[kTransformSize];
  std::uint8_t before_linear[kLinearSize];
  std::uint8_t immediate_transform[kTransformSize];
  std::uint8_t immediate_linear[kLinearSize];
};

struct alignas(64) Control {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t frame_count;
  std::uintptr_t expected_object;
  std::uintptr_t original_vptr;
  std::uintptr_t shadow_vptr;
  std::uintptr_t original_callback;
  std::uintptr_t native_pose;
  std::uintptr_t native_linear;
  std::uint8_t recording_sha256[32];
  std::uint64_t reserved[2];
};

struct alignas(64) Evidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t wrapper_entries;
  std::uint64_t original_calls;
  std::uint64_t clean_returns;
  std::uint64_t equal_frames;
  std::uint64_t corrected_frames;
  std::uint64_t correction_writes;
  std::uint64_t failures;
  std::uint64_t recursive_entries;
  std::uint32_t processed_frames;
  std::int32_t last_status;
  std::uintptr_t last_object;
  std::uintptr_t last_token;
  std::uintptr_t observed_vptr;
  std::uintptr_t final_vptr;
  std::int64_t last_result;
  std::uint64_t reserved[3];
};

static_assert(kPrimaryAddressPointRva - kPrimaryPrefixRva ==
                  kPrimaryPrefixSize,
              "final-writer primary prefix boundary");
static_assert(kNextAddressPointRva - kPrimaryPrefixRva ==
                  kPrimaryShadowSize,
              "final-writer primary vtable boundary");
static_assert(kPrimaryPrefixSize + kCallbackSlotOffset + sizeof(void*) <=
                  kPrimaryShadowSize,
              "final-writer callback slot lies inside primary shadow");
static_assert(kTransformSize + kLinearSize ==
                  kComponentCount * sizeof(float),
              "final-writer 19-float payload");
static_assert(sizeof(FrameTarget) == 80, "final-writer target ABI");
static_assert(sizeof(FrameAudit) == 160, "final-writer audit ABI");
static_assert(sizeof(Control) == 128, "final-writer control ABI");
static_assert(sizeof(Evidence) == 192, "final-writer evidence ABI");
static_assert(offsetof(Control, recording_sha256) == 72,
              "final-writer source identity offset");
static_assert(offsetof(Evidence, processed_frames) == 80,
              "final-writer cursor offset");
static_assert(offsetof(Control, reserved) == 104,
              "final-writer frame-permit offset");

}  // namespace a9tas::final_writer_replay_v1
