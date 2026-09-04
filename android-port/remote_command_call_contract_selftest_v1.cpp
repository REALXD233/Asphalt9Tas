#include "src/remote_command_call_contract_v1.h"

namespace contract = a9tas::remote_command_call_contract_v1;

constexpr bool Selftest() {
  constexpr contract::Request request{3, 0x47344903u, 12345};
  uint64_t arguments[6]{};
  if (!contract::BuildJniArguments(request, arguments) ||
      arguments[0] != 0 || arguments[1] != 0 || arguments[2] != 3 ||
      arguments[3] != 0 || arguments[4] != 0 || arguments[5] != 0)
    return false;
  constexpr uint64_t receipt =
      (static_cast<uint64_t>(12345) << 32) | 0x47344903u;
  const contract::DecodedReturn decoded = contract::Decode(receipt);
  if (decoded.tid != 12345 || decoded.tag != 0x47344903u ||
      !contract::Matches(receipt, request))
    return false;
  return !contract::Matches(receipt ^ (uint64_t{1} << 32), request) &&
         !contract::Matches(receipt ^ 1u, request) &&
         !contract::Valid({0, request.expected_tag, request.expected_tid});
}

static_assert(Selftest(), "remote command call contract selftest failed");

extern "C" int a9tas_remote_command_call_contract_selftest_v1() {
  return Selftest() ? 1 : 0;
}
