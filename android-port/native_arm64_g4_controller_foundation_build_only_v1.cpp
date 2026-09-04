#include "src/native_arm64_g4_controller_foundation_v1.h"

#if !defined(__aarch64__) || !defined(__ANDROID__)
#error "build-only native G4 controller probe requires Android AArch64"
#endif

extern "C" int a9tas_native_arm64_g4_controller_foundation_build_only_v1(
    pid_t pid, int mem,
    const a9tas::ptrace_stable_freeze_v1::FrozenSet* frozen) {
  if (!frozen) return 0;
  a9tas::native_arm64_g4_controller_foundation_v1::Prepared prepared{};
  const char* reason=nullptr;
  return a9tas::native_arm64_g4_controller_foundation_v1::PrepareStopped(
      pid,mem,*frozen,&prepared,&reason) ? 1 : 0;
}
