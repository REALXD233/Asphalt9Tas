// Read-only client for Stage 2 of the nitro RPC gate. This binary can only
// send activations=0 with kFlagObserveOnly; it has no execute mode.

#include "nitro_rpc_protocol_v1.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool ParseHex(const char* text, std::uint64_t* value) {
    if (!text || !*text) return false;
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 16);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0) return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

std::uint64_t MonotonicNsLocal() {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 1;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

bool WriteAll(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = send(fd, bytes + done, size - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool ReadAll(int fd, void* data, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = recv(fd, bytes + done, size - done, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

int Connect(const char* path) {
    if (!path || std::strlen(path) >= sizeof(sockaddr_un{}.sun_path)) return -1;
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    timeval timeout{};
    timeout.tv_usec = 250000;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) !=
            0 ||
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) !=
            0) {
        close(fd);
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s RPC_SOCKET_PATH VEHICLE_OWNER_HEX\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t owner = 0;
    if (!ParseHex(argv[2], &owner)) return 2;
    const int fd = Connect(argv[1]);
    if (fd < 0) {
        std::perror("connect nitro RPC");
        return 3;
    }

    a9tas::nitro_rpc_v1::Request request{};
    std::memcpy(request.magic, a9tas::nitro_rpc_v1::kRequestMagic,
                sizeof(request.magic));
    request.version = a9tas::nitro_rpc_v1::kVersion;
    request.size = sizeof(request);
    request.sequence = MonotonicNsLocal();
    request.vehicle_owner = owner;
    request.activations = 0;
    request.flags = a9tas::nitro_rpc_v1::kFlagObserveOnly;

    a9tas::nitro_rpc_v1::Response response{};
    const bool transport_ok = WriteAll(fd, &request, sizeof(request)) &&
                              ReadAll(fd, &response, sizeof(response));
    close(fd);
    if (!transport_ok) {
        std::fprintf(stderr, "nitro RPC transport failed errno=%d\n", errno);
        return 4;
    }
    const bool protocol_ok =
        std::memcmp(response.magic, a9tas::nitro_rpc_v1::kResponseMagic,
                    sizeof(response.magic)) == 0 &&
        response.version == a9tas::nitro_rpc_v1::kVersion &&
        response.size == sizeof(response) &&
        response.sequence == request.sequence &&
        response.result ==
            static_cast<std::int32_t>(a9tas::nitro_rpc_v1::Result::kOk) &&
        response.calls_completed == 0 && response.vehicle_owner == owner;
    std::printf(
        "NITRO_RPC_OBSERVE_V1 protocol_ok=%u seq=%" PRIu64
        " result=%d calls=%u base=0x%" PRIx64 " owner=0x%" PRIx64
        " service=0x%" PRIx64 " function=0x%" PRIx64
        " optional=%u optional_value=%u active=%u mode=%u"
        " gates=%u%u%u%u%u\n",
        protocol_ok ? 1u : 0u, response.sequence, response.result,
        response.calls_completed, response.guest_base,
        response.vehicle_owner, response.service_object,
        response.dispatch_function,
        static_cast<unsigned>(response.before.optional_180),
        response.before.optional_value_184,
        static_cast<unsigned>(response.before.active_188),
        response.before.mode_18c,
        static_cast<unsigned>(response.before.gates_1b8_1bc[0]),
        static_cast<unsigned>(response.before.gates_1b8_1bc[1]),
        static_cast<unsigned>(response.before.gates_1b8_1bc[2]),
        static_cast<unsigned>(response.before.gates_1b8_1bc[3]),
        static_cast<unsigned>(response.before.gates_1b8_1bc[4]));
    return protocol_ok ? 0 : 5;
}
