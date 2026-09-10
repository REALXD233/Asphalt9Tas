#include "../src/physics_initial_phase_v1.h"
#include <cstdio>
#include <map>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
namespace phase = a9tas::physics_initial_phase_v1;
#define CHECK(e) do { if (!(e)) { std::printf("FAIL %d %s\n", __LINE__, #e); return 1; } } while (0)
int main(int argc, char** argv) {
  if (argc == 3) {
    char path[64];
    std::snprintf(path,sizeof(path),"/proc/%d/mem",std::atoi(argv[1]));
    int fd=open(path,O_RDONLY);
    CHECK(fd>=0);
    auto readonly=[&](std::uintptr_t a,void* data,std::size_t size) {
      return pread(fd,data,size,static_cast<off_t>(a))==static_cast<ssize_t>(size);
    };
    std::uintptr_t context{};
    CHECK(phase::At(readonly,std::strtoull(argv[2],nullptr,0),64,&context));
    phase::Binding a{},b{};
    CHECK(phase::Resolve(readonly,context,&a));
    CHECK(phase::Resolve(readonly,context,&b));
    CHECK(a.world==b.world && a.update==b.update &&
          std::memcmp(&a.phase,&b.phase,sizeof(a.phase))==0);
    std::printf("INITIAL_PHASE_READONLY passed=1 context=0x%llx world=0x%llx update=0x%llx residual_bits=0x%x interval_bits=0x%x residual=%.9g interval=%.9g game_writes=0\n",
      (unsigned long long)context,(unsigned long long)a.world,(unsigned long long)a.update,
      a.phase.residual_bits,a.phase.last_interval_bits,phase::Float(a.phase.residual_bits),
      phase::Float(a.phase.last_interval_bits));
    close(fd);
    return 0;
  }
  std::map<std::uintptr_t,std::vector<std::uint8_t>> mem;
  for (auto a : {0x1000u,0x2000u,0x3000u,0x4000u,0x5000u,0x6000u,0x7000u})
    mem[a].resize(512);
  auto read = [&](std::uintptr_t a, void* out, std::size_t n) {
    for (auto& [base, bytes] : mem) if (a >= base && a-base <= bytes.size() && n <= bytes.size()-(a-base)) {
      std::memcpy(out, bytes.data()+a-base, n); return true;
    }
    return false;
  };
  unsigned writes=0;
  auto write = [&](std::uintptr_t a, const void* in, std::size_t n) {
    ++writes;
    for (auto& [base, bytes] : mem) if (a >= base && a-base <= bytes.size() && n <= bytes.size()-(a-base)) {
      std::memcpy(bytes.data()+a-base, in, n); return true;
    }
    return false;
  };
  auto put = [&](std::uintptr_t a, auto value) { write(a, &value, sizeof(value)); };
  put(0x1120, std::uintptr_t{0x2000}); put(0x2000, std::uintptr_t{0x3000});
  put(0x3060, std::uintptr_t{0x4000}); put(0x2120, std::uintptr_t{0x5000});
  put(0x5000, std::uintptr_t{0x6000}); put(0x6060, std::uintptr_t{0x7000});
  std::uint32_t forward[]={0xf9409000,0xf9400008,0xf9403103,0xd61f0060};
  write(0x4000,forward,sizeof(forward));
  std::uint32_t update[]={0xbd418a61,0x1e212800,0xbd018a60,0x1e213800,0xb9018e68,0xbd018a60};
  write(0x7000,update,sizeof(update));
  float values[]={-0.009f,1.f/60}; phase::Snapshot source;
  std::memcpy(&source, values, 8); put(0x5188,source);
  phase::Binding bound{};
  CHECK(phase::Resolve(read,0x1000,&bound)); CHECK(bound.world==0x5000);
  values[0]=-0.014f; phase::Snapshot different; std::memcpy(&different,values,8);
  put(0x5188,different); writes=0;
  CHECK(phase::Restore(read,write,bound,source)); CHECK(writes==1);
  phase::Snapshot restored{}; CHECK(read(0x5188,&restored,8));
  CHECK(std::memcmp(&restored,&source,8)==0);
  auto invalid=source; invalid.residual_bits=0x7fc00000; writes=0;
  CHECK(!phase::Restore(read,write,bound,invalid)); CHECK(writes==0);
  put(0x2120,std::uintptr_t{0x2000}); writes=0;
  CHECK(!phase::Restore(read,write,bound,source)); CHECK(writes==0);
  put(0x2120,std::uintptr_t{0x5000}); put(0x7000,std::uint32_t{0}); writes=0;
  CHECK(!phase::Restore(read,write,bound,source)); CHECK(writes==0);
  // Native-style float accumulator model; verifies phase restoration, not
  // game visual correctness. A dynamic interval remains native, not forced.
  for (const auto deltaUs : {16667u,8333u,6944u}) {
    float sourceResidual=-0.009f, replayResidual=-0.014f;
    float sourceStep=1.f/60, replayStep=1.f/60;
    unsigned mismatches=0;
    for (unsigned tick=0;tick<24000;++tick) {
      auto advance=[&](float& residual,float& last) {
        residual+=deltaUs/1000000.f;
        unsigned count=0;
        while(residual>0) {
          last=tick%97<9 ? 1.f/120 : 1.f/60;
          residual-=last; ++count;
        }
        return count;
      };
      mismatches+=advance(sourceResidual,sourceStep)!=advance(replayResidual,replayStep);
    }
    sourceResidual=replayResidual=-0.009f;
    sourceStep=replayStep=1.f/60;
    for (unsigned tick=0;tick<24000;++tick) {
      auto advance=[&](float& residual,float& last) {
        residual+=deltaUs/1000000.f;
        unsigned count=0;
        while(residual>0) { last=tick%97<9?1.f/120:1.f/60; residual-=last; ++count; }
        return count;
      };
      CHECK(advance(sourceResidual,sourceStep)==advance(replayResidual,replayStep));
      CHECK(std::memcmp(&sourceResidual,&replayResidual,4)==0);
      CHECK(1.f+sourceResidual/sourceStep == 1.f+replayResidual/replayStep);
    }
    std::printf("INITIAL_PHASE_MODEL delta=%u ticks=24000 before_mismatches=%u after_mismatches=0\n",
        deltaUs,mismatches);
  }
  std::puts("INITIAL_PHASE passed=1 resolve exact_restore invalid_float stale_chain wrong_code");
}
