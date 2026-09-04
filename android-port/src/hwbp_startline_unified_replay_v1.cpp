// Resident start-line wrapper for the live-proven unified replay core.
// READY_NO_ATTACH is published only after read-only full address validation
// and a stable paused-zero delta baseline. The wrapped executor starts only
// after the first sane positive resume delta, and retains its proven
// steering+brake+transform/linear phase implementation.

#define A9TAS_UNIFIED_BRAKE_V1 1
#define A9TAS_UNIFIED_TICK_EXECUTOR_MAIN_NAME a9tas_unified_replay_main_v1
#include "hwbp_unified_tick_executor_v1.cpp"
#undef A9TAS_UNIFIED_TICK_EXECUTOR_MAIN_NAME

#include "startline_prearm_protocol_v1.h"

namespace {

constexpr char kStartlineUnifiedAcknowledgement[] =
    "I_ACCEPT_STARTLINE_UNIFIED_STEERING_BRAKE_PHYSICS_V1";

bool CreateUnifiedReadyMarker(const char* path, pid_t pid,
                              std::uintptr_t delta_address) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    char payload[160]{};
    const int length = std::snprintf(
        payload, sizeof(payload),
        "READY_NO_ATTACH_V1 pid=%d delta=0x%" PRIxPTR
        " target_threads_attached=0 game_writes=0 scope=unified"
        " host_resume_gate=marker_removal\n",
        static_cast<int>(pid), delta_address);
    bool ok = length > 0 && static_cast<std::size_t>(length) < sizeof(payload);
    std::size_t written = 0;
    while (ok && written < static_cast<std::size_t>(length)) {
        const ssize_t count =
            write(fd, payload + written,
                  static_cast<std::size_t>(length) - written);
        if (count <= 0) {
            ok = false;
        } else {
            written += static_cast<std::size_t>(count);
        }
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok) unlink(path);
    return ok;
}

bool PrearmUnifiedUntilResume(int argc, char** argv) {
    if (argc < 7 || argc > 10 ||
        std::strcmp(argv[6], kStartlineUnifiedAcknowledgement) != 0 ||
        access(argv[5], F_OK) == 0) {
        std::fprintf(stderr,
                     "startline unified authorization missing or report "
                     "exists; no process opened\n");
        return false;
    }
    RecordingHeaderV1 recording{};
    std::vector<RecordingFrameV1> frames;
    std::size_t unsupported = 0;
    if (!LoadRecording(argv[4], &recording, &frames) ||
        !RuntimeCapabilitiesSupported(frames, &unsupported)) {
        std::fprintf(stderr,
                     "startline unified recording/capability validation "
                     "failed; no process opened\n");
        return false;
    }

    std::uint64_t pid_value = 0, base_value = 0, timeout_value = 0;
    std::uint64_t context_value = 0, main_value = 0, owner_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &timeout_value) ||
        (argc >= 8 && !ParseUnsigned(argv[7], 16, &context_value)) ||
        (argc >= 9 && !ParseUnsigned(argv[8], 16, &main_value)) ||
        (argc == 10 && !ParseUnsigned(argv[9], 16, &owner_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || timeout_value < 1000 || timeout_value > 300000) {
        std::fprintf(stderr, "invalid startline unified arguments\n");
        return false;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    std::uintptr_t main_object = 0, final_owner = 0;
    if (!VerifyTargetBuild(pid, base) ||
        !ResolveMainObject(pid, base, static_cast<std::uintptr_t>(main_value),
                           &main_object) ||
        !ResolveFinalOwner(pid, base, static_cast<std::uintptr_t>(owner_value),
                           &final_owner)) {
        std::fprintf(stderr,
                     "startline unified scheduler resolution failed; "
                     "no attach\n");
        return false;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return false;

    std::uintptr_t context = 0, adapter = 0, world = 0;
    a9tas::vehicle_state_v1::Layout vehicle{};
    a9tas::vehicle_state_v1::BackendLayout backend{};
    const bool resolved =
        ResolvePhysicsContext(pid, mem, base,
                              static_cast<std::uintptr_t>(context_value),
                              &context, &adapter, &world) &&
        a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle) &&
        a9tas::vehicle_state_v1::ResolveBackendLayout(mem, base, vehicle,
                                                       &backend) &&
        ExecutorIdentityAlive(mem, vehicle, backend, base);
    const std::uintptr_t delta = main_object + kAccumulatorOffset;
    const std::uintptr_t c98 = final_owner + kC98Offset;
    const std::uintptr_t c9c = final_owner + kC9COffset;
    const std::uintptr_t completion = context + kContextCompletionTokenOffset;
    const std::uintptr_t callback_flags = context + kContextCallbackFlagsOffset;
    const std::uintptr_t f64 = vehicle.physics_base + kCarPhysicsF64Offset;
    const std::uintptr_t world_accumulator = world + kWorldAccumulatorOffset;
    std::vector<Mapping> maps;
    if (!resolved || (delta & 7u) != 0 || (c98 & 7u) != 0 || c9c != c98 + 4 ||
        !ReadMaps(pid, &maps) || !PipelineWritable(maps, delta, 8) ||
        !PipelineWritable(maps, c98, 8) ||
        !PipelineWritable(maps, completion, 8) ||
        !PipelineWritable(maps, callback_flags, 2) ||
        !PipelineWritable(maps, f64, 4) ||
        !PipelineWritable(maps, world_accumulator, 4) ||
        !PipelineWritable(maps, backend.native_body, kNativeBodyAuditSize) ||
        !PipelineWritable(maps, backend.base + kWrapperAuditOffset,
                          kWrapperAuditSize)) {
        std::fprintf(stderr,
                     "startline unified full address validation failed; "
                     "no attach\n");
        close(mem);
        return false;
    }

    a9tas::startline_prearm_v1::ProtocolV1 protocol{};
    std::int64_t delta_us = 0;
    if (!ReadExact(mem, delta, &delta_us, sizeof(delta_us)) ||
        !a9tas::startline_prearm_v1::Initialize(&protocol, delta_us).accepted) {
        std::fprintf(stderr,
                     "startline unified paused-zero baseline missing; "
                     "no attach\n");
        close(mem);
        return false;
    }
    for (int sample = 0; sample < 7; ++sample) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!ReadExact(mem, delta, &delta_us, sizeof(delta_us)) ||
            !a9tas::startline_prearm_v1::
                 ObserveBeforeAttach(&protocol, delta_us).accepted ||
            protocol.phase !=
                a9tas::startline_prearm_v1::PhaseV1::kReadyNoAttach) {
            std::fprintf(stderr,
                         "startline unified baseline changed; no attach\n");
            close(mem);
            return false;
        }
    }
    char ready_path[96]{};
    std::snprintf(ready_path, sizeof(ready_path),
                  "/data/local/tmp/a9tas_startline_ready_%d",
                  static_cast<int>(pid));
    if (access(ready_path, F_OK) == 0 ||
        !CreateUnifiedReadyMarker(ready_path, pid, delta)) {
        std::fprintf(stderr, "stale/unwritable unified ready marker\n");
        close(mem);
        return false;
    }
    std::printf(
        "STARTLINE_UNIFIED_READY_NO_ATTACH pid=%d ready=%s frames=%zu "
        "paused_zero_samples=%u target_threads_attached=0 game_writes=0\n",
        static_cast<int>(pid), ready_path, frames.size(),
        protocol.paused_zero_observations);
    std::fflush(stdout);
    const std::uint64_t deadline =
        MonotonicNs() + timeout_value * 1000000ULL;
    bool host_resume_acknowledged = false;
    while (MonotonicNs() < deadline) {
        if (access(ready_path, F_OK) != 0) {
            host_resume_acknowledged = errno == ENOENT;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    if (!host_resume_acknowledged) {
        unlink(ready_path);
        close(mem);
        std::fprintf(stderr,
                     "startline unified host resume acknowledgement missing; no attach\n");
        return false;
    }

    bool resume = false;
    while (MonotonicNs() < deadline) {
        if (!ReadExact(mem, delta, &delta_us, sizeof(delta_us))) break;
        const auto observed =
            a9tas::startline_prearm_v1::ObserveBeforeAttach(&protocol,
                                                             delta_us);
        if (!observed.accepted) break;
        if (observed.permit_attach) {
            resume = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    close(mem);
    if (!resume) {
        std::fprintf(stderr,
                     "startline unified resume edge missing; no attach\n");
        return false;
    }
    std::printf(
        "STARTLINE_UNIFIED_RESUME_OBSERVED delta_us=%" PRId64
        " attach_permission=1 attach_attempts=0 game_writes=0\n",
        protocol.resume_delta_us);
    std::fflush(stdout);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (!PrearmUnifiedUntilResume(argc, argv)) return 5;
    char* original_ack = argv[6];
    argv[6] = const_cast<char*>(kUnifiedAcknowledgement);
    const int result = a9tas_unified_replay_main_v1(argc, argv);
    argv[6] = original_ack;
    return result;
}
