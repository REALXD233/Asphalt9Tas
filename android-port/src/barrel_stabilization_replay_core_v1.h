#pragma once

// Transport-independent tail semantics for the two AluTasV2 barrel
// stabilization detours.  The caller is responsible for invoking the exact
// natural game function first and for serializing access to the active replay
// frame, exactly as the upstream current-state mutex does.

#include "unified_tick_recording_v1.h"

#include <cstdint>

namespace a9tas::barrel_stabilization_replay_v1 {

struct Permit {
  std::uint64_t generation;
  std::uint32_t frame_index;
  std::uint32_t reserved;
};

struct ActiveFrame {
  bool has_value;
  std::uint8_t padding[7];
  Permit permit;
  std::uint32_t skip_override_flags;
  std::uint32_t angular_bits[3];
  std::uint32_t rbx_bits[2];
};

struct PostOriginalContext {
  Permit expected_permit;
  bool original_returned;
  std::uint8_t padding[7];
};

enum class Result : std::uint32_t {
  kInvalidArgument = 0,
  kNaturalNoReplayFrame = 1,
  kNaturalSkipped = 2,
  kOverridden = 3,
  kStalePermit = 4,
};

struct Capture {
  std::uint32_t angular_bits[3];
  std::uint32_t rbx_bits[2];
  std::uint64_t angular_calls;
  std::uint64_t rbx_calls;
  Result last_angular_result;
  Result last_rbx_result;
};

bool SamePermit(const Permit& left, const Permit& right) noexcept;

// These functions implement only the code after the natural stabilization
// call.  They never consume or mutate ActiveFrame, so every real stabilization
// invocation in one tick observes the same replay packet, matching AluTasV2.
Result OnBarrelRollPostOriginal(const ActiveFrame& active,
                                const PostOriginalContext& context,
                                std::uint32_t live_rbx_bits[2],
                                Capture* capture) noexcept;

Result OnBarrelYawPostOriginal(const ActiveFrame& active,
                               const PostOriginalContext& context,
                               std::uint32_t live_angular_bits[3],
                               Capture* capture) noexcept;

}  // namespace a9tas::barrel_stabilization_replay_v1
