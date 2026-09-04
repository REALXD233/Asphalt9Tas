// External tick replay with a synchronized, real nitro activation call.
//
// The two continuous control fields retain the proven HWBP post-write model.
// At completion of a selected tick pair, while the writer thread is still
// stopped, this controller sends an authenticated request to the normal
// guest-loaded ARM64 nitro RPC payload and waits for its response. The payload
// calls the game's existing NitroService vslot +0x108 once or twice. No game
// instruction is patched and no ARM64 trampoline is installed.

#define main A9TasObserverV5Main_NotUsed_Nitro
#include "hwbp_event_observer_v5.cpp"
#undef main

#include "nitro_rpc_protocol_v1.h"

#include <sys/socket.h>
#include <sys/un.h>

namespace {

using a9tas::nitro_rpc_v1::Request;
using a9tas::nitro_rpc_v1::Response;
using a9tas::nitro_rpc_v1::Result;

constexpr char kReplayMagic[8] = {'A', '9', 'S', 'P', 'R', '1', '\0', '\0'};
constexpr std::uint32_t kReplayVersion = 1;
constexpr std::uint32_t kRequiredFrameFlags = 0x7;
constexpr std::size_t kMaxReplayFrames = 36000;
constexpr std::uint8_t kReplayBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

#pragma pack(push, 1)
struct ReplayHeaderV1 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t frame_size;
    std::uint32_t frame_count;
    std::uint8_t build_id[20];
    std::uint8_t reserved[20];
};

struct ReplayFrameV1 {
    float steering;
    float longitudinal;
    float value_c;
    std::uint32_t flags;
};
#pragma pack(pop)

static_assert(sizeof(ReplayHeaderV1) == 64, "replay header ABI");
static_assert(sizeof(ReplayFrameV1) == 16, "replay frame ABI");

bool LoadReplayV1(const char* path, std::vector<ReplayFrameV1>* frames) {
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        std::perror("open replay");
        return false;
    }
    ReplayHeaderV1 header{};
    const bool header_ok =
        std::fread(&header, sizeof(header), 1, file) == 1 &&
        std::memcmp(header.magic, kReplayMagic, sizeof(kReplayMagic)) == 0 &&
        header.version == kReplayVersion &&
        header.header_size == sizeof(ReplayHeaderV1) &&
        header.frame_size == sizeof(ReplayFrameV1) &&
        header.frame_count > 0 && header.frame_count <= kMaxReplayFrames &&
        std::memcmp(header.build_id, kReplayBuildId,
                    sizeof(kReplayBuildId)) == 0;
    if (!header_ok) {
        std::fprintf(stderr, "invalid A9SPR1 header\n");
        std::fclose(file);
        return false;
    }
    frames->resize(header.frame_count);
    if (std::fread(frames->data(), sizeof(ReplayFrameV1), frames->size(),
                   file) != frames->size() ||
        std::fgetc(file) != EOF) {
        std::fprintf(stderr, "invalid A9SPR1 length\n");
        std::fclose(file);
        return false;
    }
    std::fclose(file);
    for (std::size_t index = 0; index < frames->size(); ++index) {
        const ReplayFrameV1& frame = (*frames)[index];
        if (frame.flags != kRequiredFrameFlags ||
            !std::isfinite(frame.steering) ||
            !std::isfinite(frame.longitudinal) ||
            !std::isfinite(frame.value_c) ||
            std::fabs(frame.steering) > 8.0f ||
            std::fabs(frame.longitudinal) > 8.0f ||
            std::fabs(frame.value_c) > 8.0f) {
            std::fprintf(stderr, "invalid replay frame index=%zu\n", index);
            return false;
        }
    }
    return true;
}

std::uint64_t FramePair(const ReplayFrameV1& frame) {
    std::uint32_t c98 = 0, c9c = 0;
    std::memcpy(&c98, &frame.longitudinal, sizeof(c98));
    std::memcpy(&c9c, &frame.steering, sizeof(c9c));
    return static_cast<std::uint64_t>(c98) |
           (static_cast<std::uint64_t>(c9c) << 32);
}

bool WritePairExact(int mem, std::uintptr_t address, std::uint64_t pair) {
    if (pwrite(mem, &pair, sizeof(pair), static_cast<off_t>(address)) !=
        static_cast<ssize_t>(sizeof(pair)))
        return false;
    std::uint64_t verify = 0;
    return ReadExact(mem, address, &verify, sizeof(verify)) && verify == pair;
}

void CleanupAll(std::vector<TracedThread>* threads) {
    for (auto& thread : *threads) {
        if (!thread.live) continue;
        ClearAndDetach(thread.tid, thread.stopped);
        thread.live = false;
        thread.stopped = false;
    }
}

constexpr std::size_t kMaxNitroEvents = 4096;

struct NitroEvent {
    std::size_t tick;
    std::uint32_t activations;
};

bool LoadNitroEvents(const char* path, std::size_t frame_count,
                     std::vector<NitroEvent>* events) {
    FILE* file = std::fopen(path, "r");
    if (!file) {
        std::perror("open nitro events");
        return false;
    }
    char magic[32]{};
    if (!std::fgets(magic, sizeof(magic), file) ||
        std::strcmp(magic, "A9NEV1\n") != 0) {
        std::fprintf(stderr, "invalid A9NEV1 header\n");
        std::fclose(file);
        return false;
    }
    std::size_t previous = 0;
    bool have_previous = false;
    while (true) {
        unsigned long long tick_value = 0;
        unsigned activation_value = 0;
        const int parsed = std::fscanf(file, "%llu %u", &tick_value,
                                       &activation_value);
        if (parsed == EOF) break;
        if (parsed != 2 || tick_value >= frame_count || activation_value == 0 ||
            activation_value > a9tas::nitro_rpc_v1::kMaxActivations ||
            (have_previous && tick_value <= previous) ||
            events->size() >= kMaxNitroEvents) {
            std::fprintf(stderr, "invalid nitro event index=%zu\n",
                         events->size());
            std::fclose(file);
            return false;
        }
        char tail[4]{};
        if (!std::fgets(tail, sizeof(tail), file) ||
            (std::strcmp(tail, "\n") != 0 &&
             std::strcmp(tail, "\r\n") != 0)) {
            std::fprintf(stderr, "invalid nitro event line ending\n");
            std::fclose(file);
            return false;
        }
        previous = static_cast<std::size_t>(tick_value);
        have_previous = true;
        events->push_back(
            {static_cast<std::size_t>(tick_value), activation_value});
    }
    std::fclose(file);
    return true;
}

bool SocketWriteAll(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n =
            send(fd, bytes + done, size - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool SocketReadAll(int fd, void* data, std::size_t size) {
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

int ConnectNitroRpc(const char* path) {
    if (!path || std::strlen(path) >= sizeof(sockaddr_un{}.sun_path)) return -1;
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    timeval timeout{};
    timeout.tv_usec = 100000;
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

bool NitroRpcRequest(int fd, std::uint64_t sequence,
                     std::uintptr_t vehicle_owner,
                     std::uint32_t activations, Response* response) {
    Request request{};
    std::memcpy(request.magic, a9tas::nitro_rpc_v1::kRequestMagic,
                sizeof(request.magic));
    request.version = a9tas::nitro_rpc_v1::kVersion;
    request.size = sizeof(request);
    request.sequence = sequence;
    request.vehicle_owner = vehicle_owner;
    request.activations = activations;
    request.flags = activations == 0
                        ? a9tas::nitro_rpc_v1::kFlagObserveOnly
                        : a9tas::nitro_rpc_v1::kFlagExternalTickStopped;
    if (!SocketWriteAll(fd, &request, sizeof(request)) ||
        !SocketReadAll(fd, response, sizeof(*response)))
        return false;
    return std::memcmp(response->magic,
                       a9tas::nitro_rpc_v1::kResponseMagic,
                       sizeof(response->magic)) == 0 &&
           response->version == a9tas::nitro_rpc_v1::kVersion &&
           response->size == sizeof(*response) &&
           response->sequence == sequence &&
           response->result == static_cast<std::int32_t>(Result::kOk) &&
           response->vehicle_owner == vehicle_owner &&
           response->calls_completed == activations;
}

void PrintNitroState(const char* phase, std::size_t tick,
                     const Response& response) {
    std::printf(
        "HWBP_NITRO_RPC_V1_%s tick=%zu seq=%" PRIu64
        " calls=%u base=0x%" PRIx64 " owner=0x%" PRIx64
        " service=0x%" PRIx64 " function=0x%" PRIx64
        " before_active=%u before_mode=%u after_active=%u after_mode=%u"
        " gates=%u%u%u%u%u\n",
        phase, tick, response.sequence, response.calls_completed,
        response.guest_base, response.vehicle_owner,
        response.service_object, response.dispatch_function,
        static_cast<unsigned>(response.before.active_188),
        response.before.mode_18c,
        static_cast<unsigned>(response.after.active_188),
        response.after.mode_18c,
        static_cast<unsigned>(response.before.gates_1b8_1bc[0]),
        static_cast<unsigned>(response.before.gates_1b8_1bc[1]),
        static_cast<unsigned>(response.before.gates_1b8_1bc[2]),
        static_cast<unsigned>(response.before.gates_1b8_1bc[3]),
        static_cast<unsigned>(response.before.gates_1b8_1bc[4]));
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 8) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX REPLAY_PATH TIMEOUT_MS "
                     "FINAL_OWNER_HEX NITRO_EVENTS_PATH RPC_SOCKET_PATH\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, base_value = 0, timeout_value = 0;
    std::uint64_t owner_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[4], 10, &timeout_value) ||
        !ParseUnsigned(argv[5], 16, &owner_value) || pid_value == 0 ||
        pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || owner_value == 0 || timeout_value < 1000 ||
        timeout_value > 300000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto explicit_owner = static_cast<std::uintptr_t>(owner_value);
    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported build; no thread attached\n");
        return 3;
    }
    std::vector<ReplayFrameV1> frames;
    if (!LoadReplayV1(argv[3], &frames)) return 3;
    std::vector<NitroEvent> nitro_events;
    if (!LoadNitroEvents(argv[6], frames.size(), &nitro_events)) return 3;

    std::uintptr_t final_owner = 0;
    if (!ResolveFinalOwner(pid, base, explicit_owner, &final_owner)) return 3;
    if (final_owner > UINTPTR_MAX - kC9COffset - sizeof(std::uint32_t))
        return 3;
    const std::uintptr_t pair_address = final_owner + kC98Offset;
    const std::uintptr_t c9c_address = final_owner + kC9COffset;
    if ((pair_address & 7u) != 0 || c9c_address != pair_address + 4) {
        std::fprintf(stderr, "control pair is not an aligned adjacent qword\n");
        return 3;
    }

    const int rpc = ConnectNitroRpc(argv[7]);
    if (rpc < 0) {
        std::perror("connect nitro RPC");
        return 4;
    }
    std::uint64_t rpc_sequence = MonotonicNs();
    Response handshake{};
    if (!NitroRpcRequest(rpc, rpc_sequence++, final_owner, 0, &handshake)) {
        std::fprintf(stderr,
                     "nitro RPC read-only handshake failed result=%d\n",
                     handshake.result);
        close(rpc);
        return 4;
    }
    PrintNitroState("HANDSHAKE", 0, handshake);

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open mem for replay");
        close(rpc);
        return 4;
    }
    std::vector<TracedThread> threads;
    std::uint64_t attach_failures = 0;
    const std::size_t initial_added = AttachNewThreads(
        pid, pair_address, c9c_address, &threads, &attach_failures);
    if (initial_added == 0 || attach_failures != 0) {
        std::fprintf(stderr,
                     "failed to arm all initial threads added=%zu failures=%" PRIu64
                     "\n",
                     initial_added, attach_failures);
        CleanupAll(&threads);
        close(mem);
        close(rpc);
        return 5;
    }
    std::printf(
        "HWBP_TICK_REPLAY_NITRO_V1_ARMED pid=%d owner=0x%" PRIxPTR
        " frames=%zu nitro_events=%zu threads=%zu write_scope=C98_C9C "
        "nitro_scope=real_service_vfunc_108 single_step=none\n",
        static_cast<int>(pid), final_owner, frames.size(), nitro_events.size(),
        threads.size());
    std::fflush(stdout);

    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + timeout_value * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 250000000ULL;
    std::size_t tick = 0, nitro_cursor = 0;
    std::uint64_t events = 0, write_errors = 0, unexpected_stops = 0;
    std::uint64_t dynamic_attach_failures = 0, nitro_failures = 0;
    std::uint32_t first_hit = 0;
    pid_t pair_tid = 0;
    bool have_first = false, failed = false;

    auto complete_tick = [&]() -> bool {
        if (nitro_cursor < nitro_events.size() &&
            nitro_events[nitro_cursor].tick == tick) {
            Response response{};
            const std::uint32_t count =
                nitro_events[nitro_cursor].activations;
            if (!NitroRpcRequest(rpc, rpc_sequence++, final_owner, count,
                                 &response)) {
                ++nitro_failures;
                std::fprintf(stderr,
                             "nitro RPC failed tick=%zu result=%d calls=%u\n",
                             tick, response.result, response.calls_completed);
                return false;
            }
            PrintNitroState("ACTIVATION", tick, response);
            ++nitro_cursor;
        }
        ++tick;
        return true;
    };

    while (tick < frames.size() && MonotonicNs() < deadline_ns) {
        const std::uint64_t now_ns = MonotonicNs();
        if (now_ns >= next_rescan_ns) {
            std::uint64_t failures = 0;
            AttachNewThreads(pid, pair_address, c9c_address, &threads,
                             &failures);
            dynamic_attach_failures += failures;
            if (failures != 0) {
                failed = true;
                break;
            }
            next_rescan_ns = now_ns + 250000000ULL;
        }

        int status = 0;
        const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (tid < 0) {
            if (errno == EINTR) continue;
            failed = true;
            break;
        }
        TracedThread* tracked = FindThread(&threads, tid);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (tracked) {
                tracked->live = false;
                tracked->stopped = false;
            }
            failed = true;
            break;
        }
        if (!WIFSTOPPED(status)) continue;
        if (tracked) tracked->stopped = true;
        const int signal = WSTOPSIG(status);
        unsigned long dr6 = 0;
        const bool have_dr6 = PeekDebug(tid, 6, &dr6);
        const std::uint32_t hit = static_cast<std::uint32_t>(dr6 & 3UL);
        if (signal == SIGTRAP && have_dr6 && hit != 0) {
            ++events;
            if (!WritePairExact(mem, pair_address, FramePair(frames[tick]))) {
                ++write_errors;
                failed = true;
            } else if (hit == 3) {
                have_first = false;
                if (!complete_tick()) failed = true;
            } else if (!have_first) {
                first_hit = hit;
                pair_tid = tid;
                have_first = true;
            } else if (tid != pair_tid || hit == first_hit ||
                       (hit | first_hit) != 3) {
                std::fprintf(stderr,
                             "tick pair violation tick=%zu first=%u second=%u "
                             "first_tid=%d second_tid=%d\n",
                             tick, first_hit, hit, static_cast<int>(pair_tid),
                             static_cast<int>(tid));
                failed = true;
            } else {
                have_first = false;
                if (!complete_tick()) failed = true;
                if (!failed && (tick % 300 == 0 || tick == frames.size())) {
                    std::printf(
                        "HWBP_TICK_REPLAY_NITRO_V1_PROGRESS ticks=%zu/%zu "
                        "events=%" PRIu64 " nitro=%zu/%zu\n",
                        tick, frames.size(), events, nitro_cursor,
                        nitro_events.size());
                    std::fflush(stdout);
                }
            }
            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid)) {
                failed = true;
            } else if (tracked) {
                tracked->stopped = false;
            }
            if (failed) break;
        } else {
            ++unexpected_stops;
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver)) failed = true;
            if (tracked) tracked->stopped = false;
            if (failed) break;
        }
    }

    CleanupAll(&threads);
    close(mem);
    close(rpc);
    const bool complete =
        tick == frames.size() && nitro_cursor == nitro_events.size() &&
        !have_first && !failed && write_errors == 0 &&
        unexpected_stops == 0 && dynamic_attach_failures == 0 &&
        nitro_failures == 0;
    std::printf(
        "HWBP_TICK_REPLAY_NITRO_V1_DONE complete=%u ticks=%zu/%zu "
        "events=%" PRIu64 " nitro=%zu/%zu writes_failed=%" PRIu64
        " nitro_failed=%" PRIu64 " unexpected_stops=%" PRIu64
        " attach_failures=%" PRIu64 "\n",
        complete ? 1u : 0u, tick, frames.size(), events, nitro_cursor,
        nitro_events.size(), write_errors, nitro_failures, unexpected_stops,
        dynamic_attach_failures);
    return complete ? 0 : 6;
}
