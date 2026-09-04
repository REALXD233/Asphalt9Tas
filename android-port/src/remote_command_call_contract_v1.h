#pragma once

#include <stddef.h>
#include <stdint.h>

namespace a9tas::remote_command_call_contract_v1 {

struct Request {
  uint64_t command{};
  uint32_t expected_tag{};
  uint32_t expected_tid{};
};

struct DecodedReturn {
  uint32_t tid{};
  uint32_t tag{};
};

constexpr bool Valid(const Request& request) {
  return request.command != 0 && request.expected_tag != 0 &&
         request.expected_tid != 0;
}

constexpr bool BuildJniArguments(const Request& request,
                                 uint64_t arguments[6]) {
  if (!arguments || !Valid(request)) return false;
  arguments[0] = 0;  // JNIEnv* is unused by the payload command.
  arguments[1] = 0;  // jobject is unused by the payload command.
  arguments[2] = request.command;
  arguments[3] = 0;
  arguments[4] = 0;
  arguments[5] = 0;
  return true;
}

constexpr DecodedReturn Decode(uint64_t value) {
  return {static_cast<uint32_t>(value >> 32),
          static_cast<uint32_t>(value)};
}

constexpr bool Matches(uint64_t value, const Request& request) {
  const DecodedReturn decoded = Decode(value);
  return Valid(request) && decoded.tid == request.expected_tid &&
         decoded.tag == request.expected_tag;
}

}  // namespace a9tas::remote_command_call_contract_v1
