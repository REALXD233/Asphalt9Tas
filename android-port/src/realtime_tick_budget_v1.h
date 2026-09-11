#pragma once
#include <cstdint>

namespace a9tas::realtime_tick_budget_v1 {
// Upstream-style elapsed-time accumulator. Bound each dispatch batch and drop
// suspension debt; neither high nor low render callback rates define TAS speed.
// At 1x, center the quantization window on the natural callback. A callback
// slightly earlier than its ideal deadline must not force a 0/2 dispatch pair.
struct Budget {
  std::uint64_t last_ns{}, credit_ns{};
  std::uint32_t generation{}, speed{}, divisor{1};
  std::uint64_t fractional_ns{};
  bool initialized{};
  constexpr void Reset() { *this = {}; }
  static constexpr std::uint64_t PhaseCredit(std::uint64_t interval_ns,
                                              std::uint32_t multiplier) {
    // A constant half-tick phase, not recurring bonus time. Keep real-time
    // accounting across callbacks so 120/144/150Hz cannot become double speed.
    return multiplier == 1 ? interval_ns / 2 : 0;
  }
  constexpr std::uint32_t Plan(std::uint64_t now, std::uint32_t gen,
                               std::uint64_t interval_ns, std::uint32_t multiplier,
                               std::uint32_t denominator = 1) {
    // Scale wall time, never the simulation interval or recorded packet data.
    // Integer remainder preserves sub-nanosecond credit at fractional speeds.
    // Existing callers retain their exact 1x/2x/4x/8x behavior.
    if (!interval_ns || multiplier < 1 || multiplier > 9 ||
        denominator < 1 || denominator > 10) { Reset(); return 1; }
    if (!initialized || generation != gen || speed != multiplier ||
        divisor != denominator || now < last_ns) {
      const bool rate_change_only = initialized && generation == gen &&
          now >= last_ns && (speed != multiplier || divisor != denominator);
      const bool fractional_transition = divisor != 1 || denominator != 1;
      initialized = true; generation = gen; speed = multiplier;
      divisor = denominator; last_ns = now; fractional_ns = 0;
      // Changing slowmo while running must not grant an extra physics tick.
      // Retain the unconsumed whole-nanosecond credit instead of resetting the
      // phase on every toggle. Only the sub-nanosecond division residue expires.
      if (rate_change_only && fractional_transition) return 0;
      credit_ns = PhaseCredit(interval_ns, multiplier);
      return 1;
    }
    const auto elapsed = now - last_ns;
    last_ns = now;
    if (elapsed > 250000000ULL) {
      credit_ns = PhaseCredit(interval_ns, multiplier); // no suspension debt
      fractional_ns = 0;
      return 1;
    }
    const auto scaled = elapsed * multiplier + fractional_ns;
    credit_ns += scaled / denominator;
    fractional_ns = scaled % denominator;
    const auto ticks = credit_ns / interval_ns;
    credit_ns %= interval_ns;
    return static_cast<std::uint32_t>(ticks > 32 ? 32 : ticks);
  }
};
}
