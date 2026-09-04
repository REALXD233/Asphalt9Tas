#pragma once

#include "native_arm64_remote_call_v1.h"
#include "remote_command_call_contract_v1.h"

namespace a9tas::native_arm64_command_backend_v1 {

struct Report {
  native_arm64_remote_call_v1::CallReport call{};
  remote_command_call_contract_v1::DecodedReturn decoded{};
  bool contract_match{};
};

#if defined(__aarch64__) && defined(__ANDROID__)
// tid must already be stopped by ptrace. target is the direct ARM64 payload
// export (not a NativeBridge trampoline). trap is either an immutable ARM64
// BRK or zero for the exact SIGSEGV/SEGV_MAPERR null-return fallback.
bool InvokeStopped(
    int tid,
    const native_arm64_remote_call_v1::RegisterImage& original,
    uint64_t target, uint64_t trap,
    const remote_command_call_contract_v1::Request& request,
    Report* report);
#endif

}  // namespace a9tas::native_arm64_command_backend_v1
