#pragma once

// Shared fixed-width ABI for the build-only controller-shadow coordinator and
// its future host transaction.  This header is layout/constants only: it has
// no process access, hook installation or game call.

#include "final_writer_replay_protocol_v1.h"
#include "unified_tick_recording_v1.h"

#include <cstddef>
#include <cstdint>
#include <cmath>

namespace a9tas::controller_shadow_coordinator_v1 {

inline constexpr char kControlMagic[8] = {
    'A', '9', 'I', 'P', 'T', 'C', '1', 0,
};
inline constexpr char kEvidenceMagic[8] = {
    'A', '9', 'I', 'P', 'T', 'E', '1', 0,
};
inline constexpr std::uint32_t kProtocolVersion = 1;
inline constexpr std::int32_t kRejectedBuildOnly = -100;

inline constexpr std::uintptr_t kControllerPrefixRva = 0x80C4D88;
inline constexpr std::uintptr_t kControllerAddressPointRva = 0x80C4DA8;
inline constexpr std::uintptr_t kControllerNextGroupRva = 0x80C4E88;
inline constexpr std::uintptr_t kControllerUpdateRva = 0x386B1E0;
inline constexpr std::uintptr_t kKeyboardSourceAddressPointRva = 0x80BF1C0;
inline constexpr std::size_t kControllerPrefixSize = 0x20;
inline constexpr std::size_t kControllerPrimaryTableSize = 0xE0;
inline constexpr std::size_t kControllerShadowSize =
    kControllerPrefixSize + kControllerPrimaryTableSize;
inline constexpr std::size_t kControllerUpdateSlotIndex = 14;
inline constexpr std::size_t kControllerUpdateSlotOffset =
    kControllerUpdateSlotIndex * sizeof(void*);
inline constexpr std::size_t kControllerSourceOffset = 0x48;
inline constexpr std::uint32_t kMaximumFrames =
    final_writer_replay_v1::kMaximumFrames;

enum ControlFlag : std::uint32_t {
  kConfigured = 1u << 0,
  kFramesLoaded = 1u << 1,
};

enum Status : std::int32_t {
  kPassive = 0,
  kRunning = 1,
  kComplete = 2,
  kUnexpectedObject = -201,
  kUnexpectedVptr = -202,
  kInvalidControl = -203,
  kWrongThread = -204,
  kSourceIdentityMismatch = -205,
  kSourceSwapFailure = -206,
  kCoordinatorFailure = -207,
  kRecursiveEntry = -208,
};

struct alignas(64) Control {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t frame_count;
  std::uintptr_t expected_controller;
  std::uintptr_t original_controller_vptr;
  std::uintptr_t shadow_controller_vptr;
  std::uintptr_t original_update;
  std::uintptr_t expected_source_vptr;
  std::uintptr_t writer_control;
  std::uintptr_t writer_evidence;
  std::uintptr_t action_mailbox;
  std::uintptr_t vehicle_owner;
  std::uint32_t session_id;
  std::uint32_t producer_tid;
  std::uint8_t recording_sha256[32];
  std::uint64_t reserved[2];
};

struct alignas(64) Evidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t wrapper_entries;
  std::uint64_t original_calls;
  std::uint64_t original_returns;
  std::uint64_t source_swaps;
  std::uint64_t source_restores;
  std::uint64_t selected_frames;
  std::uint64_t completed_frames;
  std::uint64_t failures;
  std::uint64_t recursive_entries;
  std::uintptr_t last_controller;
  std::uintptr_t last_frame_token;
  std::uintptr_t last_source;
  std::uintptr_t observed_controller_vptr;
  std::uintptr_t observed_source_vptr;
  std::uint32_t last_selected_frame;
  std::uint32_t last_tid;
  std::int32_t last_status;
  std::int32_t last_coordinator_result;
  std::uint32_t coordinator_phase;
  std::uint32_t next_frame;
  std::uint64_t reserved[5];
};

static_assert(kControllerAddressPointRva - kControllerPrefixRva ==
                  kControllerPrefixSize,
              "controller prefix boundary");
static_assert(kControllerNextGroupRva - kControllerPrefixRva ==
                  kControllerShadowSize,
              "controller primary group boundary");
static_assert(kControllerShadowSize == 0x100,
              "controller primary shadow extent");
static_assert(kControllerPrefixSize + kControllerUpdateSlotOffset +
                      sizeof(void*) <=
                  kControllerShadowSize,
              "UpdatePerTick slot lies in controller shadow");
static_assert(sizeof(Control) == 192,
              "controller coordinator control ABI");
static_assert(sizeof(Evidence) == 192,
              "controller coordinator evidence ABI");
static_assert(offsetof(Control, recording_sha256) == 104,
              "recording identity offset");
static_assert(offsetof(Evidence, last_status) == 136,
              "evidence status offset");

inline bool FrameInputSupported(
    const unified_tick_v1::RecordingFrameV1& frame,
    std::uint32_t index) noexcept {
  const std::uint32_t required_skips =
      unified_tick_v1::kSkipAccelerator |
      unified_tick_v1::kSkipBarrelAngular |
      unified_tick_v1::kSkipBarrelRbx |
      unified_tick_v1::kSkipRespawnButton;
  return frame.tick == index &&
         frame.flags == unified_tick_v1::kRequiredFrameFlags &&
         frame.reserved == 0 &&
         (frame.skip_override_flags &
          ~unified_tick_v1::kSupportedSkipMask) == 0 &&
         (frame.skip_override_flags &
          (unified_tick_v1::kSkipSteer | unified_tick_v1::kSkipBrake)) == 0 &&
         (frame.skip_override_flags & required_skips) == required_skips &&
         std::isfinite(frame.steering) && std::isfinite(frame.brake) &&
         frame.steering >= -1.0001f && frame.steering <= 1.0001f &&
         frame.brake >= -1.0001f && frame.brake <= 1.0001f;
}

}  // namespace a9tas::controller_shadow_coordinator_v1
