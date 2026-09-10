#include "../src/realtime_tick_budget_v1.h"
#include <initializer_list>
using a9tas::realtime_tick_budget_v1::Budget;
constexpr int Count(int hz, int seconds, int speed = 1) {
  Budget b{};
  (void)b.Plan(0, 1, 16667000, speed);
  int count = 0;
  for (int i = 1; i <= hz * seconds; ++i)
    count += b.Plan(static_cast<unsigned long long>(i)*1000000000ULL/hz, 1, 16667000, speed);
  return count;
}
static_assert(Count(144, 10) == 600, "144Hz real-time budget");
static_assert(Count(150, 10) == 600, "150Hz real-time budget");
static_assert(Count(120, 10) == 600, "120Hz");
static_assert(Count(60, 10) == 600, "60Hz retained");
static_assert(Count(30, 10) == 600, "30Hz callbacks must not halve simulation speed");
static_assert(Count(15, 10) == 600, "bounded multi-tick catch-up");
static_assert(Count(30, 10, 2) == 1199, "2x at 30Hz");
static_assert(Count(144, 10, 4) == 2399, "4x at 144Hz");
static_assert(Count(30, 10, 8) == 4799, "8x at 30Hz");
constexpr bool Transitions() {
  Budget b{};
  if (b.Plan(0,1,16667000,8) != 1 || b.Plan(100,1,16667000,8) != 0) return false;
  if (b.Plan(200,2,16667000,1) != 1) return false; // replay -> record
  if (b.Plan(10000000000ULL,2,16667000,1) != 1) return false; // long pause
  if (b.Plan(10000000100ULL,2,16667000,1) != 0) return false; // no debt
  if (b.Plan(10000000200ULL,2,16667000,8) != 1) return false; // speed switch
  if (b.Plan(10200000200ULL,2,16667000,8) != 32) return false; // bounded
  if (b.Plan(10200000300ULL,2,16667000,8) > 1) return false; // no burst backlog
  b.Reset();
  return b.Plan(5,2,16667000,1) == 1 && b.Plan(4,2,16667000,1) == 1;
}
static_assert(Transitions(), "pause/retry/clock boundaries");
constexpr bool SmoothNormalSpeed() {
  constexpr unsigned long long interval = 16667000;
  constexpr int amplitudes[] = {1000000, 2000000, 6000000};
  for (int amplitude : amplitudes) {
    Budget b{};
    (void)b.Plan(1000000000ULL, 1, interval, 1);
    for (unsigned i = 1; i <= 600; ++i) {
      const auto ideal = 1000000000ULL + i * interval;
      const auto now = i % 2 ? ideal + amplitude : ideal - amplitude;
      if (b.Plan(now, 1, interval, 1) != 1) return false;
    }
  }
  return true;
}
constexpr bool NoRecurringCredit() {
  Budget b{};
  (void)b.Plan(0, 1, 16667000, 1);
  unsigned count = 0;
  for (unsigned i = 1; i <= 6000; ++i)
    count += b.Plan(i * 1000000ULL, 1, 16667000, 1);
  // Six seconds at 1000Hz: still 360 ticks, not one tick per callback.
  return count == 360 && b.Plan(6000000000ULL, 1, 16667000, 1) == 0;
}
static_assert(SmoothNormalSpeed(), "1x jitter must not alternate skipped/double dispatches");
static_assert(NoRecurringCredit(), "phase credit must not be regranted on each callback");
constexpr bool HighFrequencyMatrix() {
  constexpr unsigned long long intervals[] = {8333000, 6944000};
  constexpr int callbacks[] = {15, 30, 60, 120, 144, 240};
  for (auto step : intervals) {
    for (int hz : callbacks) {
      for (unsigned speed : {1u, 2u, 4u, 8u}) {
        // At 15Hz/8x the deliberate 32-tick batch ceiling is reached;
        // it is not a supported no-loss real-time cadence.
        if (1000000000ULL * speed / hz / step > 32) continue;
        Budget b{};
        (void)b.Plan(0, 1, step, speed);
        unsigned actual = 0;
        for (int i = 1; i <= hz * 10; ++i)
          actual += b.Plan(static_cast<unsigned long long>(i) *
                            1000000000ULL / hz, 1, step, speed);
        const auto expected = (10000000000ULL * speed +
                               Budget::PhaseCredit(step, speed)) / step;
        if (actual != expected) return false;
      }
    }
  }
  return true;
}
static_assert(HighFrequencyMatrix(), "120/144 tick rates with mixed render cadences");
int main() { return 0; }
