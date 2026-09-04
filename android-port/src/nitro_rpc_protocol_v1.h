#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::nitro_rpc_v1 {

constexpr char kRequestMagic[8] = {'A', '9', 'N', 'R', 'Q', '1', '\0', '\0'};
constexpr char kResponseMagic[8] = {'A', '9', 'N', 'R', 'S', '1', '\0', '\0'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kFlagObserveOnly = 1u << 0;
constexpr std::uint32_t kFlagExternalTickStopped = 1u << 1;
constexpr std::uint32_t kMaxActivations = 2;

enum class Result : std::int32_t {
    kOk = 0,
    kBadRequest = -1,
    kReplaySequence = -2,
    kGameModuleUnavailable = -3,
    kUnsupportedBuild = -4,
    kUnreadableVehicle = -5,
    kInvalidService = -6,
    kInvalidDispatch = -7,
    kUnreadableState = -8,
    kActivationGateClosed = -9,
    kInternalError = -10,
};

#pragma pack(push, 1)
struct StateSnapshot {
    std::uint8_t optional_180;
    std::uint8_t active_188;
    std::uint8_t gates_1b8_1bc[5];
    std::uint8_t reserved;
    std::uint32_t optional_value_184;
    std::uint32_t mode_18c;
};

struct Request {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint64_t sequence;
    std::uint64_t vehicle_owner;
    std::uint32_t activations;
    std::uint32_t flags;
};

struct Response {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint64_t sequence;
    std::int32_t result;
    std::uint32_t calls_completed;
    std::uint64_t guest_base;
    std::uint64_t vehicle_owner;
    std::uint64_t service_object;
    std::uint64_t dispatch_function;
    StateSnapshot before;
    StateSnapshot after;
};
#pragma pack(pop)

static_assert(sizeof(StateSnapshot) == 16, "nitro RPC state ABI");
static_assert(sizeof(Request) == 40, "nitro RPC request ABI");
static_assert(sizeof(Response) == 96, "nitro RPC response ABI");

}  // namespace a9tas::nitro_rpc_v1
