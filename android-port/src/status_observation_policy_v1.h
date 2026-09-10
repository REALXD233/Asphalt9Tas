#pragma once
#include <cstdint>

namespace a9tas::status_observation_v1 {
constexpr bool ShouldRelocateLifecycle(bool active, bool readable,
                                       std::uint32_t state) {
  // Countdown already proves the next anchor. A running, bound session does
  // not need a heap scan to prove that no countdown object exists elsewhere.
  return !readable || (state != 2u && !(active && state == 3u));
}
static_assert(!ShouldRelocateLifecycle(true, true, 3), "running observation is bounded");
static_assert(!ShouldRelocateLifecycle(true, true, 2), "Retry countdown is directly visible");
static_assert(!ShouldRelocateLifecycle(false, true, 2), "sealed countdown is directly visible");
static_assert(ShouldRelocateLifecycle(false, true, 3), "sealed stale race must allow relocation");
static_assert(ShouldRelocateLifecycle(true, false, 3), "unreadable active object must recover");
static_assert(ShouldRelocateLifecycle(false, false, 0), "retired object must recover");
static_assert(ShouldRelocateLifecycle(true, true, 9), "race finish must allow relocation");
static_assert(ShouldRelocateLifecycle(true, true, 0xffffffffu), "unknown state must not suppress recovery");
}
