// Low-overhead, observation-only proof for the candidate physics zero gap.
//
// DR0 watches the validated main-loop accumulator/token. DR1 and DR2 watch the
// player native body's authoritative linear/angular X components. DR3 watches
// one downstream racer-pose component. Unlike the discovery observer, this
// tool never captures stack words and reads only the four watched values.

#define main A9TasSchedulerObserverMain_NotUsed
#include "hwbp_scheduler_observer_v1.cpp"
#undef main

#include "vehicle_state_resolver_v1.h"

namespace {

constexpr char kZeroGapMagic[8] = {'A', '9', 'Z', 'G', 'B', '1', '\0', '\0'};
constexpr std::uint32_t kZeroGapVersion = 1;

enum ZeroGapHeaderFlags : std::uint32_t {
    kZeroGapClean = 1u << 0,
    kZeroGapTargetVerified = 1u << 1,
    kZeroGapRotationWatch = 1u << 2,
};

enum ZeroGapEventFlags : std::uint32_t {
    kZeroGapHitAccumulator = 1u << 0,
    kZeroGapHitNativeLinear = 1u << 1,
    kZeroGapHitNativeAngular = 1u << 2,
    kZeroGapHitDownstreamPose = 1u << 3,
};

#pragma pack(push, 1)
struct ZeroGapHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t event_size;
    std::uint32_t flags;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t main_object;
    std::uint64_t accumulator_address;
    std::uint64_t native_body;
    std::uint64_t native_linear_address;
    std::uint64_t native_angular_address;
    std::uint64_t downstream_pose_address;
    std::uint64_t downstream_rotation_address;
    std::uint64_t start_ns;
    std::uint64_t event_count;
    std::uint64_t accumulator_hits;
    std::uint64_t native_linear_hits;
    std::uint64_t native_angular_hits;
    std::uint64_t downstream_pose_hits;
    std::uint64_t read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t unexpected_stops;
    std::uint32_t attached_threads;
    std::uint32_t final_threads;
};

struct ZeroGapEvent {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::int32_t tid;
    std::uint32_t flags;
    std::uint64_t rip;
    std::int64_t accumulator;
    std::uint32_t native_linear_bits;
    std::uint32_t native_angular_bits;
    std::uint32_t downstream_pose_bits;
    std::uint32_t read_ok;
};
#pragma pack(pop)

static_assert(sizeof(ZeroGapHeader) == 176, "zero-gap header ABI");
static_assert(sizeof(ZeroGapEvent) == 56, "zero-gap event ABI");

struct ZeroGapThread {
    pid_t tid{};
    bool live{};
    bool stopped{};
};

ZeroGapThread* FindZeroGapThread(std::vector<ZeroGapThread>* threads,
                                pid_t tid) {
    for (auto& thread : *threads)
        if (thread.tid == tid) return &thread;
    return nullptr;
}

bool IsTaskMember(pid_t pid, pid_t tid) {
    char path[96]{};
    std::snprintf(path, sizeof(path), "/proc/%d/task/%d/status",
                  static_cast<int>(pid), static_cast<int>(tid));
    return access(path, R_OK) == 0;
}

bool ValidateWatchAddress(const std::vector<Mapping>& maps,
                          std::uintptr_t address, std::size_t size) {
    const Mapping* map = FindMapping(maps, address, size);
    return map && map->perms[0] == 'r' && map->perms[1] == 'w';
}

bool AttachZeroGapThread(pid_t tid, std::uintptr_t accumulator,
                         std::uintptr_t linear, std::uintptr_t angular,
                         std::uintptr_t pose) {
    return AttachOne(tid, accumulator, linear, angular, pose,
                     BoundaryDr7(true));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 7 || argc > 8) {
        std::fprintf(
            stderr,
            "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH "
            "MAIN_TID WORKER_TID [POSE_KIND]\n"
            "POSE_KIND: position (default) or rotation\n",
            argv[0]);
        return 2;
    }

    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t duration_value = 0;
    std::uint64_t main_tid_value = 0;
    std::uint64_t worker_tid_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) ||
        !ParseUnsigned(argv[5], 10, &main_tid_value) ||
        !ParseUnsigned(argv[6], 10, &worker_tid_value) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        main_tid_value == 0 ||
        main_tid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        worker_tid_value == 0 ||
        worker_tid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || duration_value < 100 || duration_value > 300000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    bool watch_rotation = false;
    if (argc == 8) {
        if (std::strcmp(argv[7], "rotation") == 0)
            watch_rotation = true;
        else if (std::strcmp(argv[7], "position") != 0) {
            std::fprintf(stderr, "POSE_KIND must be position or rotation\n");
            return 2;
        }
    }

    const pid_t pid = static_cast<pid_t>(pid_value);
    const pid_t main_tid = static_cast<pid_t>(main_tid_value);
    const pid_t worker_tid = static_cast<pid_t>(worker_tid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto duration_ms = static_cast<std::uint64_t>(duration_value);
    if (!IsTaskMember(pid, main_tid) || !IsTaskMember(pid, worker_tid)) {
        std::fprintf(stderr, "requested TID is not a member of the target process\n");
        return 3;
    }
    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported build; no thread was attached\n");
        return 3;
    }

    std::uintptr_t main_object = 0;
    if (!ResolveMainObject(pid, base, 0, &main_object)) return 3;
    const std::uintptr_t accumulator = main_object + kAccumulatorOffset;

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

    const std::uintptr_t downstream_pose =
        watch_rotation ? layout.rotation_address : layout.position_address;
    if ((accumulator & 7u) != 0 ||
        (backend.native_linear_address & 3u) != 0 ||
        (backend.native_angular_address & 3u) != 0 ||
        (downstream_pose & 3u) != 0) {
        std::fprintf(stderr, "watch address alignment failure\n");
        close(mem);
        return 3;
    }
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps) ||
        !ValidateWatchAddress(maps, accumulator, sizeof(std::uint64_t)) ||
        !ValidateWatchAddress(maps, backend.native_linear_address,
                              sizeof(std::uint32_t)) ||
        !ValidateWatchAddress(maps, backend.native_angular_address,
                              sizeof(std::uint32_t)) ||
        !ValidateWatchAddress(maps, downstream_pose,
                              sizeof(std::uint32_t))) {
        std::fprintf(stderr, "watch address mapping validation failed\n");
        close(mem);
        return 3;
    }

    FILE* out = std::fopen(argv[4], "wb");
    if (!out) {
        close(mem);
        return 5;
    }
    ZeroGapHeader header{};
    std::memcpy(header.magic, kZeroGapMagic, sizeof(kZeroGapMagic));
    header.version = kZeroGapVersion;
    header.header_size = sizeof(header);
    header.event_size = sizeof(ZeroGapEvent);
    header.flags = kZeroGapTargetVerified;
    if (watch_rotation) header.flags |= kZeroGapRotationWatch;
    header.pid = static_cast<std::uint64_t>(pid);
    header.library_base = base;
    header.main_object = main_object;
    header.accumulator_address = accumulator;
    header.native_body = backend.native_body;
    header.native_linear_address = backend.native_linear_address;
    header.native_angular_address = backend.native_angular_address;
    header.downstream_pose_address = downstream_pose;
    header.downstream_rotation_address = layout.rotation_address;
    header.start_ns = MonotonicNs();
    if (header.start_ns == 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1) {
        std::fclose(out);
        close(mem);
        return 6;
    }

    std::vector<ZeroGapThread> threads;
    const pid_t requested[] = {main_tid, worker_tid};
    for (pid_t tid : requested) {
        if (FindZeroGapThread(&threads, tid)) continue;
        if (!AttachZeroGapThread(tid, accumulator,
                                 backend.native_linear_address,
                                 backend.native_angular_address,
                                 downstream_pose)) {
            ++header.ptrace_errors;
            continue;
        }
        threads.push_back({tid, true, false});
    }
    header.attached_threads = static_cast<std::uint32_t>(threads.size());
    if (threads.size() != (main_tid == worker_tid ? 1u : 2u)) {
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        std::fclose(out);
        close(mem);
        return 7;
    }

    std::printf(
        "ZERO_GAP_V1 pid=%d main_tid=%d worker_tid=%d object=0x%" PRIxPTR
        " accumulator=0x%" PRIxPTR " native_body=0x%" PRIxPTR
        " linear=0x%" PRIxPTR " angular=0x%" PRIxPTR
        " downstream_%s=0x%" PRIxPTR " duration_ms=%" PRIu64
        " capture=values-only write_scope=debug-registers-only\n",
        static_cast<int>(pid), static_cast<int>(main_tid),
        static_cast<int>(worker_tid), main_object, accumulator,
        backend.native_body, backend.native_linear_address,
        backend.native_angular_address,
        watch_rotation ? "rotation" : "position", downstream_pose,
        duration_ms);
    std::fflush(stdout);

    const std::uint64_t deadline_ns = header.start_ns + duration_ms * 1000000ULL;
    bool output_ok = true;
    while (MonotonicNs() < deadline_ns) {
        int status = 0;
        const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
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
        ZeroGapThread* tracked = FindZeroGapThread(&threads, tid);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (tracked) {
                tracked->live = false;
                tracked->stopped = false;
            }
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        if (tracked) tracked->stopped = true;
        const int signal = WSTOPSIG(status);
        unsigned long dr6 = 0;
        const bool have_dr6 = PeekDebug(tid, 6, &dr6);
        if (signal == SIGTRAP && have_dr6 && (dr6 & 15UL)) {
            ZeroGapEvent event{};
            event.sequence = header.event_count;
            event.monotonic_ns = MonotonicNs();
            event.tid = static_cast<std::int32_t>(tid);
            if (dr6 & 1UL) {
                event.flags |= kZeroGapHitAccumulator;
                ++header.accumulator_hits;
            }
            if (dr6 & 2UL) {
                event.flags |= kZeroGapHitNativeLinear;
                ++header.native_linear_hits;
            }
            if (dr6 & 4UL) {
                event.flags |= kZeroGapHitNativeAngular;
                ++header.native_angular_hits;
            }
            if (dr6 & 8UL) {
                event.flags |= kZeroGapHitDownstreamPose;
                ++header.downstream_pose_hits;
            }
            user_regs_struct regs{};
            if (ptrace(PTRACE_GETREGS, tid, nullptr, &regs) == -1)
                ++header.ptrace_errors;
            else
                event.rip = static_cast<std::uint64_t>(regs.rip);

            const bool read_ok =
                ReadExact(mem, accumulator, &event.accumulator,
                          sizeof(event.accumulator)) &&
                ReadExact(mem, backend.native_linear_address,
                          &event.native_linear_bits,
                          sizeof(event.native_linear_bits)) &&
                ReadExact(mem, backend.native_angular_address,
                          &event.native_angular_bits,
                          sizeof(event.native_angular_bits)) &&
                ReadExact(mem, downstream_pose, &event.downstream_pose_bits,
                          sizeof(event.downstream_pose_bits));
            event.read_ok = read_ok ? 1u : 0u;
            if (!read_ok) ++header.read_errors;
            if (std::fwrite(&event, sizeof(event), 1, out) != 1) {
                output_ok = false;
                break;
            }
            ++header.event_count;
            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
                ++header.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        } else {
            ++header.unexpected_stops;
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver))
                ++header.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        }
    }

    for (auto& thread : threads) {
        if (!thread.live) continue;
        if (!ClearAndDetach(thread.tid, thread.stopped))
            ++header.ptrace_errors;
        else {
            thread.live = false;
            thread.stopped = false;
        }
        ++header.final_threads;
    }
    if (output_ok && header.event_count > 0 && header.read_errors == 0 &&
        header.ptrace_errors == 0 && header.unexpected_stops == 0)
        header.flags |= kZeroGapClean;
    if (std::fseek(out, 0, SEEK_SET) != 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1 ||
        std::fflush(out) != 0 || std::ferror(out))
        output_ok = false;
    if (std::fclose(out) != 0) output_ok = false;
    close(mem);

    std::printf(
        "ZERO_GAP_V1_DONE events=%" PRIu64 " accumulator=%" PRIu64
        " linear=%" PRIu64 " angular=%" PRIu64 " pose=%" PRIu64
        " read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
        " unexpected_stops=%" PRIu64 " clean=%u path=%s\n",
        header.event_count, header.accumulator_hits,
        header.native_linear_hits, header.native_angular_hits,
        header.downstream_pose_hits, header.read_errors,
        header.ptrace_errors, header.unexpected_stops,
        (header.flags & kZeroGapClean) != 0, argv[4]);
    return output_ok ? 0 : 8;
}
