#include "src/native_arm64_remote_call_v1.h"

namespace call = a9tas::native_arm64_remote_call_v1;

constexpr bool SemanticSelftest() {
  call::RegisterImage original{};
  original.sp = 0x70001000;
  original.pc = 0x1000;
  original.pstate = 0x600003c5;
  original.x[19] = 0x1919;
  original.x[29] = 0x2929;
  original.x[30] = 0x3030;
  const uint64_t args[6]{1, 2, 3, 4, 5, 6};
  call::PreparedCall prepared{};
  if (!call::Prepare(original, 0x4000, 0x8000, args, &prepared)) return false;
  for (size_t index = 0; index < 6; ++index)
    if (prepared.registers.x[index] != args[index]) return false;
  if (prepared.registers.x[19] != original.x[19] ||
      prepared.registers.x[29] != original.x[29] ||
      prepared.registers.x[30] != 0x8000 ||
      prepared.registers.sp != original.sp ||
      prepared.registers.pc != 0x4000 ||
      prepared.registers.pstate != original.pstate)
    return false;
  if (call::Prepare(original, 0x4002, 0x8000, args, &prepared) ||
      call::Prepare(original, 0x4000, 0x8002, args, &prepared))
    return false;
  original.sp |= 8;
  if (call::Prepare(original, 0x4000, 0x8000, args, &prepared)) return false;
  call::RegisterImage returned{};
  returned.pc = 0x8000;
  return call::ReturnedAtExactTrap(returned, 0x8000) &&
         !call::ReturnedAtExactTrap(returned, 0x8004);
}

static_assert(SemanticSelftest(),
              "native ARM64 remote-call ABI transition selftest failed");

extern "C" int a9tas_native_arm64_remote_call_semantic_selftest_v1() {
  return SemanticSelftest() ? 1 : 0;
}
