#include "native_arm64_command_backend_v1.h"

#if defined(__aarch64__) && defined(__ANDROID__)

namespace a9tas::native_arm64_command_backend_v1 {

bool InvokeStopped(
    int tid,
    const native_arm64_remote_call_v1::RegisterImage& original,
    uint64_t target, uint64_t trap,
    const remote_command_call_contract_v1::Request& request,
    Report* report) {
  if (!report) return false;
  *report = {};
  uint64_t arguments[6]{};
  if (tid <= 0 || request.expected_tid != static_cast<uint32_t>(tid) ||
      !remote_command_call_contract_v1::BuildJniArguments(request, arguments))
    return false;
  const bool called = native_arm64_remote_call_v1::CallStoppedThread(
      tid, original, target, trap, arguments, &report->call);
  report->decoded =
      remote_command_call_contract_v1::Decode(report->call.return_value);
  report->contract_match = called && report->call.expected_trap &&
      report->call.rollback_succeeded && report->call.detach_safe &&
      remote_command_call_contract_v1::Matches(report->call.return_value,
                                               request);
  return report->contract_match;
}

}  // namespace a9tas::native_arm64_command_backend_v1

#endif
