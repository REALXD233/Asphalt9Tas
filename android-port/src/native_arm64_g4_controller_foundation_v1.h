#pragma once

#include "native_arm64_command_backend_v1.h"
#include "native_arm64_g4_payload_resolver_v1.h"
#include "native_arm64_immutable_trap_resolver_v1.h"
#include "ptrace_stable_freeze_v1.h"

namespace a9tas::native_arm64_g4_controller_foundation_v1 {

struct Prepared {
  pid_t pid{};
  pid_t stopped_tid{};
  native_arm64_g4_payload_resolver_v1::Layout payload{};
  native_arm64_immutable_trap_resolver_v1::Report trap{};
};

struct Report {
  Prepared prepared{};
  native_arm64_command_backend_v1::Report command{};
  bool frozen_set_complete{};
  bool payload_resolved{};
  bool immutable_trap_resolved{};
  bool command_completed{};
};

#if defined(__aarch64__) && defined(__ANDROID__)
// The caller owns the full-process stopped transaction. This layer only
// resolves the exact already-loaded G4 payload and immutable return trap.
bool PrepareStopped(
    pid_t pid, int mem,
    const ptrace_stable_freeze_v1::FrozenSet& frozen,
    Prepared* prepared, const char** failure_reason = nullptr);

// InvokeStopped preserves the shared JNI-shaped command ABI and requires the
// native remote-call layer to restore the complete register image itself.
bool InvokePreparedStopped(
    const Prepared& prepared,
    const ptrace_stable_freeze_v1::FrozenSet& frozen,
    const native_arm64_remote_call_v1::RegisterImage& original,
    const remote_command_call_contract_v1::Request& request,
    Report* report);
#endif

}  // namespace a9tas::native_arm64_g4_controller_foundation_v1
