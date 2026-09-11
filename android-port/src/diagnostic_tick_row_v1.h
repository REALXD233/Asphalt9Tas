#pragma once
#include <cstdint>

namespace a9tas::diagnostic_tick_row_v1 {
// Called only after full session/receipt validation. High-frequency logic ticks
// may be interpolation-only; exporting them must not require a physics substep.
constexpr bool Valid(std::uint64_t tick, std::uint64_t expected_tick,
                     std::uint32_t interval_calls, std::uint32_t delta_us) {
  return tick == expected_tick &&
      (interval_calls > 0 || delta_us == 8333 || delta_us == 6944);
}
}
