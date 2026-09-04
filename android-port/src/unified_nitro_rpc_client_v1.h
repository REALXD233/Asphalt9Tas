#pragma once

// Host-side client used only by the certified unified tick executor.  The
// request is synchronous: the HWBP-owned physics thread remains stopped from
// the fixed-delta event until the guest ARM64 service has replied.  This gives
// the caller a fail-closed delta -> nitro RPC -> real physics ordering.

#include "nitro_rpc_protocol_v1.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace a9tas::unified_nitro_rpc_v1 {

inline constexpr char kSocketPath[] =
    "/data/user/0/com.aligames.kuang.kybc.aligames/files/"
    "a9tas-nitro-rpc-v1.sock";

inline bool WriteAll(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t count =
            send(fd, bytes + done, size - done, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

inline bool ReadAll(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t count = recv(fd, bytes + done, size - done, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

inline int Connect() {
    static_assert(sizeof(kSocketPath) <= sizeof(sockaddr_un{}.sun_path),
                  "nitro RPC socket path is too long");
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    timeval timeout{};
    timeout.tv_usec = 250000;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
        close(fd);
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s",
                  kSocketPath);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

inline bool Request(int fd, std::uint64_t sequence,
                    std::uintptr_t vehicle_owner,
                    std::uint32_t activations,
                    nitro_rpc_v1::Response* response) {
    if (fd < 0 || sequence == 0 || vehicle_owner == 0 || !response ||
        activations > nitro_rpc_v1::kMaxActivations)
        return false;
    nitro_rpc_v1::Request request{};
    std::memcpy(request.magic, nitro_rpc_v1::kRequestMagic,
                sizeof(request.magic));
    request.version = nitro_rpc_v1::kVersion;
    request.size = sizeof(request);
    request.sequence = sequence;
    request.vehicle_owner = vehicle_owner;
    request.activations = activations;
    request.flags = activations == 0
                        ? nitro_rpc_v1::kFlagObserveOnly
                        : nitro_rpc_v1::kFlagExternalTickStopped;
    std::memset(response, 0, sizeof(*response));
    if (!WriteAll(fd, &request, sizeof(request)) ||
        !ReadAll(fd, response, sizeof(*response)))
        return false;
    return std::memcmp(response->magic, nitro_rpc_v1::kResponseMagic,
                       sizeof(response->magic)) == 0 &&
           response->version == nitro_rpc_v1::kVersion &&
           response->size == sizeof(*response) &&
           response->sequence == sequence &&
           response->result ==
               static_cast<std::int32_t>(nitro_rpc_v1::Result::kOk) &&
           response->vehicle_owner == vehicle_owner &&
           response->calls_completed == activations;
}

}  // namespace a9tas::unified_nitro_rpc_v1
