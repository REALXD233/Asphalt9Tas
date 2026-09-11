#include "../src/realtime_tick_budget_v1.h"
#include <initializer_list>
using a9tas::realtime_tick_budget_v1::Budget;
constexpr bool SlowmoMatrix() {
  for (unsigned divisor : {2u, 4u, 8u}) {
    for (unsigned hz : {15u, 30u, 60u, 120u, 144u, 240u}) {
      for (unsigned long long step : {16667000ULL, 8333000ULL, 6944000ULL}) {
        Budget b{};
        (void)b.Plan(0, 1, step, 1, divisor);
        unsigned ticks = 0;
        for (unsigned i = 1; i <= hz * 10; ++i)
          ticks += b.Plan(i * 1000000000ULL / hz, 1, step, 1, divisor);
        if (ticks != (10000000000ULL / divisor + step / 2) / step) return false;
      }
    }
  }
  return true;
}
constexpr bool FractionalCredit() {
  Budget b{};
  (void)b.Plan(0,1,1000,1,3);
  unsigned ticks=0;
  for(unsigned i=1;i<=3000;++i)ticks+=b.Plan(i,1,1000,1,3);
  return ticks==1 && b.fractional_ns==0;
}
constexpr bool Boundaries() {
  Budget b{};
  if(b.Plan(0,1,16667000,1)!=1)return false;
  if(b.Plan(100,1,16667000,1,4)!=0)return false;
  if(b.Plan(200,1,16667000,1)!=0)return false;
  if(b.Plan(300,2,16667000,1,4)!=1)return false;
  if(b.Plan(10000000000ULL,2,16667000,1,4)!=1)return false;
  if(b.Plan(10000000100ULL,2,16667000,1,4)!=0)return false;
  b.Reset();
  return b.Plan(0,3,16667000,1,4)==1;
}
static_assert(SlowmoMatrix(), "slowmo must preserve tick count across callback rates");
static_assert(FractionalCredit(), "fractional wall time must not be lost per callback");
constexpr bool ThreeQuarter() {
  for (unsigned hz : {30u,60u,120u,144u,240u}) {
    Budget b{};
    (void)b.Plan(0,1,16667000,3,4);
    unsigned ticks=0;
    for(unsigned i=1;i<=hz*10;++i)
      ticks+=b.Plan(i*1000000000ULL/hz,1,16667000,3,4);
    if(ticks!=449)return false;
  }
  return true;
}
static_assert(ThreeQuarter(), "0.75x does not change simulation interval");
constexpr bool NineTenths() {
  for (unsigned hz : {15u,30u,60u,120u,144u,240u}) {
    for (unsigned long long step : {16667000ULL,8333000ULL,6944000ULL}) {
      Budget b{};
      (void)b.Plan(0,1,step,9,10);
      unsigned ticks=0;
      for(unsigned i=1;i<=hz*10;++i)
        ticks+=b.Plan(i*1000000000ULL/hz,1,step,9,10);
      if(ticks!=9000000000ULL/step)return false;
    }
  }
  return true;
}
static_assert(NineTenths(), "0.9x retains fractional credit across callback frequencies");
static_assert(Boundaries(), "no bonus tick on slowmo switch or pause catch-up debt");
int main() { return 0; }
