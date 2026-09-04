#include "native_arm64_g4_payload_resolver_v1.h"

extern "C" bool a9tas_native_arm64_g4_payload_resolve_build_only_v1(
    int pid,int mem,
    a9tas::native_arm64_g4_payload_resolver_v1::Layout* output) {
  return a9tas::native_arm64_g4_payload_resolver_v1::Resolve(
      pid,mem,output,nullptr);
}
