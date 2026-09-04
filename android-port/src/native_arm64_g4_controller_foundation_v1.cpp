#include "native_arm64_g4_controller_foundation_v1.h"

#if defined(__aarch64__) && defined(__ANDROID__)

namespace a9tas::native_arm64_g4_controller_foundation_v1 {

bool PrepareStopped(pid_t pid, int mem,
                    const ptrace_stable_freeze_v1::FrozenSet& frozen,
                    Prepared* prepared, const char** failure_reason) {
  const auto fail=[&](const char* reason) {
    if (failure_reason) *failure_reason=reason;
    return false;
  };
  if (failure_reason) *failure_reason="invalid_arguments";
  if (pid<=0 || mem<0 || !prepared || frozen.call_tid<=0 ||
      !ptrace_stable_freeze_v1::Complete(frozen,pid))
    return fail("frozen_set");
  native_arm64_g4_payload_resolver_v1::Layout payload{};
  const char* payload_failure="none";
  if (!native_arm64_g4_payload_resolver_v1::Resolve(
          pid,mem,&payload,&payload_failure)) {
    if (failure_reason) *failure_reason=payload_failure;
    return false;
  }
  native_arm64_immutable_trap_resolver_v1::Report trap{};
  if (!native_arm64_immutable_trap_resolver_v1::Resolve(pid,&trap) ||
      trap.address==0 || trap.instruction==0)
    return fail("immutable_trap");
  prepared->pid=pid;
  prepared->stopped_tid=frozen.call_tid;
  prepared->payload=payload;
  prepared->trap=trap;
  if (failure_reason) *failure_reason="none";
  return true;
}

bool InvokePreparedStopped(
    const Prepared& prepared,
    const ptrace_stable_freeze_v1::FrozenSet& frozen,
    const native_arm64_remote_call_v1::RegisterImage& original,
    const remote_command_call_contract_v1::Request& request,
    Report* report) {
  if (!report) return false;
  *report={};
  report->prepared=prepared;
  report->frozen_set_complete=
      prepared.pid>0 && prepared.stopped_tid>0 &&
      frozen.call_tid==prepared.stopped_tid &&
      ptrace_stable_freeze_v1::Complete(frozen,prepared.pid);
  report->payload_resolved=prepared.payload.command!=0 &&
      prepared.payload.load_bias!=0 && prepared.payload.control!=0 &&
      prepared.payload.evidence!=0;
  report->immutable_trap_resolved=
      prepared.trap.address!=0 && prepared.trap.instruction!=0;
  if (!report->frozen_set_complete || !report->payload_resolved ||
      !report->immutable_trap_resolved ||
      request.expected_tid!=static_cast<std::uint32_t>(prepared.stopped_tid))
    return false;
  report->command_completed=native_arm64_command_backend_v1::InvokeStopped(
      prepared.stopped_tid,original,prepared.payload.command,
      prepared.trap.address,request,&report->command);
  return report->command_completed;
}

}  // namespace a9tas::native_arm64_g4_controller_foundation_v1

#endif
