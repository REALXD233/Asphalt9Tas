// PT-NB1 build-only payload.  It reuses the exact game-action semantic
// validator but creates no worker/socket thread and compiles out all nonzero
// action execution.  NativeBridge invokes this export on the caller's thread.

#include <jni.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

#define A9TAS_GAME_ACTION_RPC_V1 1
#define A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE 0
#define A9TAS_NITRO_RPC_ENABLE_SERVER_THREAD 0
#define A9TAS_NITRO_RPC_ENABLE_STATUS_WRITES 0
#include "payload_nitro_rpc_v1.cpp"

extern "C" __attribute__((visibility("default"))) jlong
a9tas_producer_action_observe_v1(JNIEnv*, jobject, jlong vehicle_owner) {
    Request request{};
    std::memcpy(request.magic, a9tas::nitro_rpc_v1::kRequestMagic,
                sizeof(request.magic));
    request.version = a9tas::nitro_rpc_v1::kVersion;
    request.size = sizeof(request);
    request.sequence = 1;
    request.vehicle_owner = static_cast<std::uint64_t>(vehicle_owner);
    request.activations = 0;
    request.flags = a9tas::nitro_rpc_v1::kFlagObserveOnly;

    Response response{};
    InitializeResponse(request, &response);
    const Result result = HandleRequest(request, &response);
    const auto tid = static_cast<std::uint32_t>(syscall(__NR_gettid));
    const auto result_bits = static_cast<std::uint32_t>(
        static_cast<std::int32_t>(result));
    return static_cast<jlong>((static_cast<std::uint64_t>(tid) << 32) |
                              result_bits);
}

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_producer_action_observe_protocol_v1() {
    return 1;
}
