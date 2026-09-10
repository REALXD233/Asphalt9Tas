#pragma once
#include <cstdint>

namespace a9tas::realtime_tick_budget_v1 {
// Upstream-style elapsed-time accumulator. Bound each dispatch batch and drop
// suspension debt; neither high nor low render callback rates define TAS speed.
// At 1x, center the quantization window on the natural callback. A callback
// slightly earlier than its ideal deadline must not force a 0/2 dispatch pair.
struct Budget {
  std::uint64_t last_ns{}, credit_ns{};
  std::uint32_t generation{}, speed{};
  bool initialized{};
  constexpr void Reset() { *this = {}; }
  static constexpr std::uint64_t PhaseCredit(std::uint64_t interval_ns,
                                              std::uint32_t multiplier) {
    // A constant half-tick phase, not recurring bonus time. Keep real-time
    // accounting across callbacks so 120/144/150Hz cannot become double speed.
    return multiplier == 1 ? interval_ns / 2 : 0;
  }
  constexpr std::uint32_t Plan(std::uint64_t now, std::uint32_t gen,
                               std::uint64_t interval_ns, std::uint32_t multiplier) {
    if (!interval_ns || multiplier < 1 || multiplier > 8) { Reset(); return 1; }
    if (!initialized || generation != gen || speed != multiplier || now < last_ns) {
      initialized = true; generation = gen; speed = multiplier; last_ns = now;
      credit_ns = PhaseCredit(interval_ns, multiplier);
      return 1;
    }
    const auto elapsed = now - last_ns;
    last_ns = now;
    if (elapsed > 250000000ULL) {
      credit_ns = PhaseCredit(interval_ns, multiplier); // no suspension debt
      return 1;
    }
    credit_ns += elapsed * multiplier;
    const auto ticks = credit_ns / interval_ns;
    credit_ns %= interval_ns;
    return static_cast<std::uint32_t>(ticks > 32 ? 32 : ticks);
  }
};
}
