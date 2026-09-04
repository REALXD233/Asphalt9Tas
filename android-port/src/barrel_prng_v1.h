#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::barrel_prng_v1 {

// Exact std::mt19937 engine used by upstream AsphaltTas for barrel-related
// random choices.  The algorithm and seed are standardized; keeping the small
// engine here avoids a runtime C++ library dependency in the injected payload.
inline constexpr std::uint32_t kSeed = 0;
inline constexpr std::size_t kStateWords = 624;
inline constexpr std::size_t kMiddleWord = 397;

struct State {
  std::uint32_t words[kStateWords]{};
  std::size_t index{kStateWords};
};

inline constexpr void Reset(State* state) noexcept {
  if (state == nullptr) return;
  state->words[0] = kSeed;
  for (std::size_t index = 1; index < kStateWords; ++index) {
    const std::uint32_t previous = state->words[index - 1];
    state->words[index] =
        1812433253u * (previous ^ (previous >> 30u)) +
        static_cast<std::uint32_t>(index);
  }
  state->index = kStateWords;
}

inline constexpr void Twist(State* state) noexcept {
  for (std::size_t index = 0; index < kStateWords; ++index) {
    const std::uint32_t combined =
        (state->words[index] & 0x80000000u) |
        (state->words[(index + 1) % kStateWords] & 0x7fffffffu);
    state->words[index] =
        state->words[(index + kMiddleWord) % kStateWords] ^
        (combined >> 1u) ^ ((combined & 1u) != 0 ? 0x9908b0dfu : 0u);
  }
  state->index = 0;
}

inline constexpr std::uint32_t NextWord(State* state) noexcept {
  if (state == nullptr) return 0;
  if (state->index >= kStateWords) Twist(state);
  std::uint32_t value = state->words[state->index++];
  value ^= value >> 11u;
  value ^= (value << 7u) & 0x9d2c5680u;
  value ^= (value << 15u) & 0xefc60000u;
  value ^= value >> 18u;
  return value;
}

inline constexpr float NextFloat01(State* state) noexcept {
  return static_cast<float>(NextWord(state)) / 4294967296.0f;
}

inline constexpr double NextDouble01(State* state) noexcept {
  const std::int32_t low = static_cast<std::int32_t>(NextWord(state));
  const std::int32_t high = static_cast<std::int32_t>(NextWord(state));
  constexpr double kRange = 4294967296.0;
  const double combined = static_cast<double>(low) +
                          static_cast<double>(high) * kRange;
  return combined / (kRange * kRange);
}

inline constexpr bool NextBool(State* state) noexcept {
  return NextDouble01(state) < 0.5;
}

inline constexpr bool SelfTest() noexcept {
  State state{};
  Reset(&state);
  constexpr std::uint32_t expected[] = {
      2357136044u, 2546248239u, 3071714933u,
      3626093760u, 2588848963u,
  };
  for (const std::uint32_t value : expected)
    if (NextWord(&state) != value) return false;
  Reset(&state);
  return NextWord(&state) == expected[0];
}

static_assert(SelfTest(), "fixed-seed MT19937 compatibility");

}  // namespace a9tas::barrel_prng_v1
