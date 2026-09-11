#include "../src/diagnostic_tick_row_v1.h"
using a9tas::diagnostic_tick_row_v1::Valid;
static_assert(Valid(1, 1, 0, 6944));
static_assert(Valid(1, 1, 0, 8333));
static_assert(!Valid(1, 1, 0, 16667));
static_assert(!Valid(1, 1, 0, 0));
static_assert(!Valid(2, 1, 0, 6944));
static_assert(!Valid(0, 1, 2, 8333));
static_assert(Valid(1, 1, 2, 16667));
constexpr bool MixedRows() {
  unsigned rows=0, physical_calls=0;
  for (unsigned tick=0; tick<645; ++tick) {
    // Synthetic sparse fixture matching observed totals, not the actual trace.
    const unsigned calls=tick%645 < 269 ? 1 : 0;
    if (!Valid(tick,tick,calls,6944)) return false;
    ++rows;
    physical_calls+=calls;
  }
  return rows==645 && physical_calls==269;
}
static_assert(MixedRows(), "all sparse diagnostic rows must be exported");
int main() { return 0; }
