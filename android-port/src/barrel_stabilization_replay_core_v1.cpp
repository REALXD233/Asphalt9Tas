// Transport-independent AluTasV2 barrel stabilization replay tail.
//
// This file deliberately contains no hook installer, process access or thread
// control.  The G4/G6 in-process runtime calls the exact Android stabilization
// function first and invokes this semantic tail only after that call returns.

#include "barrel_stabilization_replay_core_v1.h"

#include <cstdio>
#include <cstring>

namespace a9tas::barrel_stabilization_replay_v1 {

namespace {

template <std::size_t Count>
void CopyBits(std::uint32_t (&destination)[Count],
              const std::uint32_t (&source)[Count]) noexcept {
  std::memcpy(destination, source, sizeof(destination));
}

template <std::size_t Count>
Result ApplyPostOriginal(const ActiveFrame& active,
                         const PostOriginalContext& context,
                         std::uint32_t (&live_bits)[Count],
                         const std::uint32_t (&target_bits)[Count],
                         std::uint32_t skip_flag,
                         std::uint32_t (&captured_bits)[Count]) noexcept {
  // The natural function must have completed before this semantic tail is
  // entered.  Refuse to manufacture an "original returned" receipt.
  if (!context.original_returned) return Result::kInvalidArgument;

  Result result = Result::kNaturalNoReplayFrame;
  if (active.has_value) {
    if (!SamePermit(active.permit, context.expected_permit)) {
      result = Result::kStalePermit;
    } else if ((active.skip_override_flags & skip_flag) != 0) {
      result = Result::kNaturalSkipped;
    } else {
      // Upstream assigns/copies unconditionally when the replay frame is
      // present and the independent skip bit is clear.  Do not replace this
      // with an epsilon comparison or a stunt classifier.
      CopyBits(live_bits, target_bits);
      result = Result::kOverridden;
    }
  }

  // Upstream captures the final post-natural/post-override value on every
  // stabilization call, including no-frame and skipped calls.
  CopyBits(captured_bits, live_bits);
  return result;
}

}  // namespace

bool SamePermit(const Permit& left, const Permit& right) noexcept {
  return left.generation != 0 && left.generation == right.generation &&
         left.frame_index == right.frame_index && left.reserved == 0 &&
         right.reserved == 0;
}

Result OnBarrelRollPostOriginal(const ActiveFrame& active,
                                const PostOriginalContext& context,
                                std::uint32_t live_rbx_bits[2],
                                Capture* capture) noexcept {
  if (live_rbx_bits == nullptr || capture == nullptr) {
    return Result::kInvalidArgument;
  }
  auto& live = *reinterpret_cast<std::uint32_t (*)[2]>(live_rbx_bits);
  const Result result = ApplyPostOriginal(
      active, context, live, active.rbx_bits,
      unified_tick_v1::kSkipBarrelRbx, capture->rbx_bits);
  if (result != Result::kInvalidArgument) ++capture->rbx_calls;
  capture->last_rbx_result = result;
  return result;
}

Result OnBarrelYawPostOriginal(const ActiveFrame& active,
                               const PostOriginalContext& context,
                               std::uint32_t live_angular_bits[3],
                               Capture* capture) noexcept {
  if (live_angular_bits == nullptr || capture == nullptr) {
    return Result::kInvalidArgument;
  }
  auto& live = *reinterpret_cast<std::uint32_t (*)[3]>(live_angular_bits);
  const Result result = ApplyPostOriginal(
      active, context, live, active.angular_bits,
      unified_tick_v1::kSkipBarrelAngular, capture->angular_bits);
  if (result != Result::kInvalidArgument) ++capture->angular_calls;
  capture->last_angular_result = result;
  return result;
}

}  // namespace a9tas::barrel_stabilization_replay_v1

#if defined(A9TAS_BARREL_STABILIZATION_REPLAY_SELFTEST)

namespace {

using namespace a9tas::barrel_stabilization_replay_v1;
using namespace a9tas::unified_tick_v1;

bool Equal(const std::uint32_t* left, const std::uint32_t* right,
           std::size_t count) {
  return std::memcmp(left, right, count * sizeof(*left)) == 0;
}

bool SelfTest() {
  ActiveFrame frame{};
  frame.has_value = true;
  frame.permit = {7, 42, 0};
  frame.angular_bits[0] = 0x3f800000u;
  frame.angular_bits[1] = 0x80000000u;
  frame.angular_bits[2] = 0x7fc12345u;
  frame.rbx_bits[0] = 0x40400000u;
  frame.rbx_bits[1] = 0xbf000000u;
  PostOriginalContext context{{7, 42, 0}, true, {}};
  Capture capture{};

  std::uint32_t rbx[2] = {0x11111111u, 0x22222222u};
  if (OnBarrelRollPostOriginal(frame, context, rbx, &capture) !=
          Result::kOverridden ||
      !Equal(rbx, frame.rbx_bits, 2) ||
      !Equal(capture.rbx_bits, frame.rbx_bits, 2) ||
      capture.rbx_calls != 1) {
    return false;
  }

  // A second natural call in the same tick reuses the same packet; no cursor
  // is consumed by the first call.
  rbx[0] = 0x33333333u;
  rbx[1] = 0x44444444u;
  if (OnBarrelRollPostOriginal(frame, context, rbx, &capture) !=
          Result::kOverridden ||
      !Equal(rbx, frame.rbx_bits, 2) || capture.rbx_calls != 2) {
    return false;
  }

  std::uint32_t angular[3] = {9, 8, 7};
  frame.skip_override_flags = kSkipBarrelAngular;
  if (OnBarrelYawPostOriginal(frame, context, angular, &capture) !=
          Result::kNaturalSkipped ||
      capture.angular_bits[0] != 9 || capture.angular_bits[1] != 8 ||
      capture.angular_bits[2] != 7 || capture.angular_calls != 1) {
    return false;
  }

  frame.skip_override_flags = kSkipBarrelRbx;
  rbx[0] = 0xaabbccddu;
  rbx[1] = 0x55667788u;
  if (OnBarrelRollPostOriginal(frame, context, rbx, &capture) !=
          Result::kNaturalSkipped ||
      capture.rbx_bits[0] != rbx[0] || capture.rbx_bits[1] != rbx[1]) {
    return false;
  }

  frame.has_value = false;
  angular[0] = 1;
  angular[1] = 2;
  angular[2] = 3;
  if (OnBarrelYawPostOriginal(frame, context, angular, &capture) !=
          Result::kNaturalNoReplayFrame ||
      !Equal(angular, capture.angular_bits, 3)) {
    return false;
  }

  frame.has_value = true;
  frame.skip_override_flags = 0;
  const PostOriginalContext stale{{8, 42, 0}, true, {}};
  angular[0] = 0x10;
  angular[1] = 0x20;
  angular[2] = 0x30;
  if (OnBarrelYawPostOriginal(frame, stale, angular, &capture) !=
          Result::kStalePermit ||
      angular[0] != 0x10 || angular[1] != 0x20 || angular[2] != 0x30 ||
      !Equal(angular, capture.angular_bits, 3)) {
    return false;
  }

  const PostOriginalContext before_original{{7, 42, 0}, false, {}};
  const std::uint64_t calls_before = capture.angular_calls;
  if (OnBarrelYawPostOriginal(frame, before_original, angular, &capture) !=
          Result::kInvalidArgument ||
      capture.angular_calls != calls_before) {
    return false;
  }

  return true;
}

}  // namespace

extern "C" int a9tas_barrel_stabilization_replay_selftest_v1() {
  return SelfTest() ? 0 : 1;
}

#endif

#if !defined(A9TAS_BARREL_STABILIZATION_REPLAY_NO_MAIN)
int main() {
  std::puts(
      "BARREL_STABILIZATION_REPLAY_BUILD_ONLY runtime=disabled "
      "hook_installer=0 device_access=0 game_writes=0");
  return 0;
}
#endif
