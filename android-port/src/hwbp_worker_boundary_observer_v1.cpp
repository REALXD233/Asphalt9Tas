// Read-only host observer for the ARM64 worker-boundary active-call counter.
// DR0 watches the probe-owned counter. DR1/DR2 watch authoritative native
// linear/angular X. DR3 watches the native current-pose tail. Only the known
// physics worker task is attached; no guest or vehicle memory is written.

#define main A9TasSchedulerObserverMain_NotUsed
#include "hwbp_scheduler_observer_v1.cpp"
#undef main

#include "vehicle_state_resolver_v1.h"

namespace {

constexpr char kWorkerBoundaryMagic[8] = {
    'A', '9', 'W', 'B', 'P', '1', '\0', '\0'};
constexpr std::uint32_t kWorkerBoundaryVersion = 1;

enum WorkerBoundaryHeaderFlags : std::uint32_t {
    kWorkerBoundaryClean = 1u << 0,
    kWorkerBoundaryTargetVerified = 1u << 1,
};

enum WorkerBoundaryEventFlags : std::uint32_t {
    kWorkerBoundaryHitPhase = 1u << 0,
    kWorkerBoundaryHitLinear = 1u << 1,
    kWorkerBoundaryHitAngular = 1u << 2,
    kWorkerBoundaryHitPose = 1u << 3,
};

#pragma pack(push, 1)
struct WorkerBoundaryHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t event_size;
    std::uint32_t flags;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t active_address;
    std::uint64_t native_body;
    std::uint64_t native_linear_address;
    std::uint64_t native_angular_address;
    std::uint64_t native_pose_tail_address;
    std::uint64_t start_ns;
    std::uint64_t event_count;
    std::uint64_t phase_hits;
    std::uint64_t linear_hits;
    std::uint64_t angular_hits;
    std::uint64_t pose_hits;
    std::uint64_t read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t unexpected_stops;
    std::uint32_t worker_tid;
    std::uint32_t attached_threads;
    std::uint32_t final_threads;
    std::uint32_t reserved;
};

struct WorkerBoundaryEvent {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::int32_t tid;
    std::uint32_t flags;
    std::uint64_t rip;
    std::uint64_t active_calls;
    std::uint32_t native_linear_bits;
    std::uint32_t native_angular_bits;
    std::uint32_t native_pose_bits;
    std::uint32_t read_ok;
};
#pragma pack(pop)

static_assert(sizeof(WorkerBoundaryHeader) == 168,
              "worker-boundary header ABI");
static_assert(sizeof(WorkerBoundaryEvent) == 56,
              "worker-boundary event ABI");

bool IsTaskMember(pid_t pid, pid_t tid) {
    char path[96]{};
    std::snprintf(path, sizeof(path), "/proc/%d/task/%d/status",
                  static_cast<int>(pid), static_cast<int>(tid));
    return access(path, R_OK) == 0;
}

bool WritableAddress(const std::vector<Mapping>& maps,
                     std::uintptr_t address, std::size_t size) {
    const Mapping* map = FindMapping(maps, address, size);
    return map && map->perms[0] == 'r' && map->perms[1] == 'w';
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 7) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH "
                     "WORKER_TID ACTIVE_ADDRESS_HEX\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, base_value = 0, duration_value = 0;
    std::uint64_t worker_tid_value = 0, active_address_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) ||
        !ParseUnsigned(argv[5], 10, &worker_tid_value) ||
        !ParseUnsigned(argv[6], 16, &active_address_value) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        worker_tid_value == 0 ||
        worker_tid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || active_address_value == 0 ||
        duration_value < 100 || duration_value > 300000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const pid_t worker_tid = static_cast<pid_t>(worker_tid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto active_address =
        static_cast<std::uintptr_t>(active_address_value);
    if (!IsTaskMember(pid, worker_tid) || (active_address & 7u) != 0) {
        std::fprintf(stderr, "worker task or active-counter alignment invalid\n");
        return 3;
    }
    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported game build\n");
        return 3;
    }

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 4;
    a9tas::vehicle_state_v1::Layout layout{};
    a9tas::vehicle_state_v1::BackendLayout backend{};
    if (!a9tas::vehicle_state_v1::Resolve(pid, mem, base, &layout) ||
        !a9tas::vehicle_state_v1::ResolveBackendLayout(mem, base, layout,
                                                       &backend)) {
        std::fprintf(stderr, "vehicle/backend resolution failed at stage=%u\n",
                     backend.failure_stage);
        close(mem);
        return 3;
    }
    const auto linear = backend.native_linear_address;
    const auto angular = backend.native_angular_address;
    const auto pose = backend.native_pose_commit_tail_address;
    std::uint64_t active_calls = UINT64_MAX;
    std::vector<Mapping> maps;
    if ((linear & 3u) != 0 || (angular & 3u) != 0 || (pose & 3u) != 0 ||
        !ReadMaps(pid, &maps) ||
        !WritableAddress(maps, active_address, sizeof(active_calls)) ||
        !WritableAddress(maps, linear, sizeof(std::uint32_t)) ||
        !WritableAddress(maps, angular, sizeof(std::uint32_t)) ||
        !WritableAddress(maps, pose, sizeof(std::uint32_t)) ||
        !ReadExact(mem, active_address, &active_calls, sizeof(active_calls)) ||
        active_calls > 64) {
        std::fprintf(stderr, "watch mapping or initial active counter invalid\n");
        close(mem);
        return 3;
    }

    FILE* out = std::fopen(argv[4], "wb");
    if (!out) {
        close(mem);
        return 5;
    }
    WorkerBoundaryHeader header{};
    std::memcpy(header.magic, kWorkerBoundaryMagic,
                sizeof(kWorkerBoundaryMagic));
    header.version = kWorkerBoundaryVersion;
    header.header_size = sizeof(header);
    header.event_size = sizeof(WorkerBoundaryEvent);
    header.flags = kWorkerBoundaryTargetVerified;
    header.pid = static_cast<std::uint64_t>(pid);
    header.library_base = base;
    header.active_address = active_address;
    header.native_body = backend.native_body;
    header.native_linear_address = linear;
    header.native_angular_address = angular;
    header.native_pose_tail_address = pose;
    header.start_ns = MonotonicNs();
    header.worker_tid = static_cast<std::uint32_t>(worker_tid);
    if (header.start_ns == 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1) {
        std::fclose(out);
        close(mem);
        return 6;
    }

    if (!AttachOne(worker_tid, active_address, linear, angular, pose,
                   BoundaryDr7(true))) {
        std::fclose(out);
        close(mem);
        return 7;
    }
    header.attached_threads = 1;
    bool stopped = false;
    bool live = true;
    bool output_ok = true;
    std::printf(
        "WORKER_BOUNDARY_V1 pid=%d tid=%d active=0x%" PRIxPTR
        " body=0x%" PRIxPTR " linear=0x%" PRIxPTR
        " angular=0x%" PRIxPTR " pose_tail=0x%" PRIxPTR
        " duration_ms=%" PRIu64
        " capture=values-only write_scope=debug-registers-only\n",
        static_cast<int>(pid), static_cast<int>(worker_tid), active_address,
        backend.native_body, linear, angular, pose, duration_value);
    std::fflush(stdout);

    const std::uint64_t deadline_ns =
        header.start_ns + duration_value * 1000000ULL;
    while (MonotonicNs() < deadline_ns && live) {
        int status = 0;
        const pid_t tid = waitpid(worker_tid, &status, __WALL | WNOHANG);
        if (tid == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            continue;
        }
        if (tid < 0) {
            if (errno == EINTR) continue;
            if (errno != ECHILD) ++header.ptrace_errors;
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            continue;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            live = false;
            stopped = false;
            break;
        }
        if (!WIFSTOPPED(status)) continue;
        stopped = true;
        const int signal = WSTOPSIG(status);
        unsigned long dr6 = 0;
        if (signal == SIGTRAP && PeekDebug(tid, 6, &dr6) && (dr6 & 15UL)) {
            WorkerBoundaryEvent event{};
            event.sequence = header.event_count;
            event.monotonic_ns = MonotonicNs();
            event.tid = static_cast<std::int32_t>(tid);
            if (dr6 & 1UL) {
                event.flags |= kWorkerBoundaryHitPhase;
                ++header.phase_hits;
            }
            if (dr6 & 2UL) {
                event.flags |= kWorkerBoundaryHitLinear;
                ++header.linear_hits;
            }
            if (dr6 & 4UL) {
                event.flags |= kWorkerBoundaryHitAngular;
                ++header.angular_hits;
            }
            if (dr6 & 8UL) {
                event.flags |= kWorkerBoundaryHitPose;
                ++header.pose_hits;
            }
            user_regs_struct registers{};
            if (ptrace(PTRACE_GETREGS, tid, nullptr, &registers) == -1)
                ++header.ptrace_errors;
            else
                event.rip = static_cast<std::uint64_t>(registers.rip);
            const bool read_ok =
                ReadExact(mem, active_address, &event.active_calls,
                          sizeof(event.active_calls)) &&
                ReadExact(mem, linear, &event.native_linear_bits,
                          sizeof(event.native_linear_bits)) &&
                ReadExact(mem, angular, &event.native_angular_bits,
                          sizeof(event.native_angular_bits)) &&
                ReadExact(mem, pose, &event.native_pose_bits,
                          sizeof(event.native_pose_bits));
            event.read_ok = read_ok ? 1u : 0u;
            if (!read_ok) ++header.read_errors;
            if (std::fwrite(&event, sizeof(event), 1, out) != 1) {
                output_ok = false;
                break;
            }
            ++header.event_count;
            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
                ++header.ptrace_errors;
            else
                stopped = false;
        } else {
            ++header.unexpected_stops;
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver))
                ++header.ptrace_errors;
            else
                stopped = false;
        }
    }

    if (live) {
        if (!ClearAndDetach(worker_tid, stopped))
            ++header.ptrace_errors;
        else
            ++header.final_threads;
    }
    if (output_ok && header.event_count > 0 && header.read_errors == 0 &&
        header.ptrace_errors == 0 && header.unexpected_stops == 0 &&
        header.final_threads == header.attached_threads)
        header.flags |= kWorkerBoundaryClean;
    if (std::fseek(out, 0, SEEK_SET) != 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1 ||
        std::fflush(out) != 0 || std::ferror(out))
        output_ok = false;
    if (std::fclose(out) != 0) output_ok = false;
    close(mem);
    std::printf(
        "WORKER_BOUNDARY_V1_DONE events=%" PRIu64 " phase=%" PRIu64
        " linear=%" PRIu64 " angular=%" PRIu64 " pose=%" PRIu64
        " read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
        " unexpected_stops=%" PRIu64 " clean=%u path=%s\n",
        header.event_count, header.phase_hits, header.linear_hits,
        header.angular_hits, header.pose_hits, header.read_errors,
        header.ptrace_errors, header.unexpected_stops,
        (header.flags & kWorkerBoundaryClean) != 0, argv[4]);
    return output_ok ? 0 : 8;
}
