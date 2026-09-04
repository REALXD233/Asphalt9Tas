// Host-only, observation-only proof for the native physics worker scope.
//
// The observer attaches only the already identified physics worker task and
// uses x86_64 data hardware breakpoints on the player's native linear velocity,
// angular velocity and integrated-pose tail. On every write it copies the top
// 32 host stack words. Houdini exposes the active guest return PCs in this
// stack view, so the offline parser can verify that every authoritative write
// is nested under one of PhysicsWorld_run_three_phase_update's three virtual
// phases. No guest code is patched or single-stepped and no game value is
// written.

#define main A9TasSchedulerObserverMain_NotUsed
#include "hwbp_scheduler_observer_v1.cpp"
#undef main

#include "vehicle_state_resolver_v1.h"

#ifndef A9TAS_EXECUTOR_AFFINITY_STACK
#define A9TAS_EXECUTOR_AFFINITY_STACK 0
#endif

#if A9TAS_EXECUTOR_AFFINITY_STACK
constexpr std::size_t kCapturedStackWords = 256;
#else
constexpr std::size_t kCapturedStackWords = 32;
#endif

namespace {

#if A9TAS_EXECUTOR_AFFINITY_STACK
constexpr char kStackScopeMagic[8] = {
    'A', '9', 'E', 'S', 'A', '1', '\0', '\0'};
#else
constexpr char kStackScopeMagic[8] = {
    'A', '9', 'W', 'S', 'S', '1', '\0', '\0'};
#endif
constexpr std::uint32_t kStackScopeVersion = 1;
constexpr std::uintptr_t kWorkerUpdateRva = 0x5E08050;
constexpr std::uintptr_t kPhaseReturnRvas[3] = {
    0x5E080A0, 0x5E080AC, 0x5E080B8};
constexpr std::uint8_t kWorkerUpdateSignature[16] = {
    0xff, 0xc3, 0x01, 0xd1, 0xfc, 0x6f, 0x02, 0xa9,
    0xfa, 0x67, 0x03, 0xa9, 0xf8, 0x5f, 0x04, 0xa9,
};
#if A9TAS_EXECUTOR_AFFINITY_STACK
constexpr std::uintptr_t kPhysicsExecutorRva = 0x38B74DC;
constexpr std::uint8_t kPhysicsExecutorSignature[16] = {
    0xff, 0x83, 0x01, 0xd1, 0xf5, 0x53, 0x04, 0xa9,
    0xf3, 0x7b, 0x05, 0xa9, 0xf4, 0x03, 0x08, 0xaa,
};
#endif

enum StackScopeHeaderFlags : std::uint32_t {
    kStackScopeClean = 1u << 0,
    kStackScopeTargetVerified = 1u << 1,
};

enum StackScopeEventFlags : std::uint32_t {
    kStackScopeHitLinear = 1u << 0,
    kStackScopeHitAngular = 1u << 1,
    kStackScopeHitPose = 1u << 2,
};

#pragma pack(push, 1)
struct StackScopeHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t event_size;
    std::uint32_t flags;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t native_body;
    std::uint64_t native_linear_address;
    std::uint64_t native_angular_address;
    std::uint64_t native_pose_tail_address;
    std::uint64_t phase_return_0;
    std::uint64_t phase_return_1;
    std::uint64_t phase_return_2;
    std::uint64_t start_ns;
    std::uint64_t event_count;
    std::uint64_t linear_hits;
    std::uint64_t angular_hits;
    std::uint64_t pose_hits;
    std::uint64_t value_read_errors;
    std::uint64_t stack_read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t unexpected_stops;
    std::uint32_t worker_tid;
    std::uint32_t attached_threads;
    std::uint32_t final_threads;
    std::uint32_t reserved;
};

struct StackScopeEvent {
    std::uint64_t sequence;
    std::uint64_t monotonic_ns;
    std::int32_t tid;
    std::uint32_t flags;
    std::uint64_t rip;
    std::uint64_t rsp;
    std::uint32_t native_linear_bits;
    std::uint32_t native_angular_bits;
    std::uint32_t native_pose_bits;
    std::uint32_t value_read_ok;
    std::uint32_t stack_read_ok;
    std::uint32_t reserved;
    std::uint64_t stack_words[kCapturedStackWords];
};
#pragma pack(pop)

static_assert(sizeof(StackScopeHeader) == 184, "stack-scope header ABI");
static_assert(sizeof(StackScopeEvent) == 64 + 8 * kCapturedStackWords,
              "stack-scope event ABI");

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

#if !A9TAS_EXECUTOR_AFFINITY_STACK
unsigned long ThreeWrite4Dr7() {
    // Local DR0/DR1/DR2, write-only, four-byte length. DR3 is disabled.
    return 1UL | (1UL << 16) | (3UL << 18) |
           (1UL << 2) | (1UL << 20) | (3UL << 22) |
           (1UL << 4) | (1UL << 24) | (3UL << 26);
}
#endif

#if A9TAS_EXECUTOR_AFFINITY_STACK
unsigned long OneWrite4Dr7() {
    // Local DR0, write-only, four-byte length. DR1..DR3 are disabled.
    return 1UL | (1UL << 16) | (3UL << 18);
}
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH "
                     "WORKER_TID\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0, base_value = 0, duration_value = 0;
    std::uint64_t worker_tid_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) ||
        !ParseUnsigned(argv[5], 10, &worker_tid_value) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        worker_tid_value == 0 ||
        worker_tid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || duration_value < 100 || duration_value > 300000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const pid_t worker_tid = static_cast<pid_t>(worker_tid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto duration_ms = static_cast<std::uint64_t>(duration_value);
    if (!IsTaskMember(pid, worker_tid)) {
        std::fprintf(stderr, "worker task is not a member of the process\n");
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
    std::uint8_t worker_signature[sizeof(kWorkerUpdateSignature)]{};
    if (base > UINTPTR_MAX - kWorkerUpdateRva ||
        !ReadExact(mem, base + kWorkerUpdateRva, worker_signature,
                   sizeof(worker_signature)) ||
        std::memcmp(worker_signature, kWorkerUpdateSignature,
                    sizeof(worker_signature)) != 0) {
        std::fprintf(stderr, "worker update signature mismatch\n");
        close(mem);
        return 3;
    }
#if A9TAS_EXECUTOR_AFFINITY_STACK
    std::uint8_t executor_signature[sizeof(kPhysicsExecutorSignature)]{};
    if (base > UINTPTR_MAX - kPhysicsExecutorRva ||
        !ReadExact(mem, base + kPhysicsExecutorRva, executor_signature,
                   sizeof(executor_signature)) ||
        std::memcmp(executor_signature, kPhysicsExecutorSignature,
                    sizeof(executor_signature)) != 0) {
        std::fprintf(stderr, "physics executor signature mismatch\n");
        close(mem);
        return 3;
    }
#endif

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
    std::vector<Mapping> maps;
    if ((linear & 3u) != 0 || (angular & 3u) != 0 || (pose & 3u) != 0 ||
        !ReadMaps(pid, &maps) ||
        !WritableAddress(maps, linear, sizeof(std::uint32_t)) ||
        !WritableAddress(maps, angular, sizeof(std::uint32_t)) ||
        !WritableAddress(maps, pose, sizeof(std::uint32_t))) {
        std::fprintf(stderr, "state watch address validation failed\n");
        close(mem);
        return 3;
    }

    FILE* out = std::fopen(argv[4], "wb");
    if (!out) {
        close(mem);
        return 5;
    }
    StackScopeHeader header{};
    std::memcpy(header.magic, kStackScopeMagic, sizeof(kStackScopeMagic));
    header.version = kStackScopeVersion;
    header.header_size = sizeof(header);
    header.event_size = sizeof(StackScopeEvent);
    header.flags = kStackScopeTargetVerified;
    header.pid = static_cast<std::uint64_t>(pid);
    header.library_base = base;
    header.native_body = backend.native_body;
    header.native_linear_address = linear;
    header.native_angular_address = angular;
    header.native_pose_tail_address = pose;
    header.phase_return_0 = base + kPhaseReturnRvas[0];
    header.phase_return_1 = base + kPhaseReturnRvas[1];
    header.phase_return_2 = base + kPhaseReturnRvas[2];
    header.start_ns = MonotonicNs();
    header.worker_tid = static_cast<std::uint32_t>(worker_tid);
    if (header.start_ns == 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1) {
        std::fclose(out);
        close(mem);
        return 6;
    }

#if A9TAS_EXECUTOR_AFFINITY_STACK
    if (!AttachOne(worker_tid, pose, 0, 0, 0, OneWrite4Dr7())) {
#else
    if (!AttachOne(worker_tid, linear, angular, pose, 0,
                   ThreeWrite4Dr7())) {
#endif
        std::fclose(out);
        close(mem);
        return 7;
    }
    header.attached_threads = 1;
    bool stopped = false;
    bool live = true;
    bool output_ok = true;
#if A9TAS_EXECUTOR_AFFINITY_STACK
    std::printf(
        "EXECUTOR_STACK_AFFINITY_V1 pid=%d tid=%d body=0x%" PRIxPTR
        " pose_tail=0x%" PRIxPTR " phase_returns=0x%" PRIx64
        ",0x%" PRIx64 ",0x%" PRIx64 " executor_return=0x%" PRIxPTR
        " duration_ms=%" PRIu64
        " capture=stack256 write_scope=debug-registers-only guest_patch=none\n",
        static_cast<int>(pid), static_cast<int>(worker_tid),
        backend.native_body, pose, header.phase_return_0,
        header.phase_return_1, header.phase_return_2,
        base + kPhysicsExecutorRva + 0x9C, duration_ms);
#else
    std::printf(
        "WORKER_STACK_SCOPE_V1 pid=%d tid=%d body=0x%" PRIxPTR
        " linear=0x%" PRIxPTR " angular=0x%" PRIxPTR
        " pose_tail=0x%" PRIxPTR " phase_returns=0x%" PRIx64
        ",0x%" PRIx64 ",0x%" PRIx64 " duration_ms=%" PRIu64
        " capture=stack32 write_scope=debug-registers-only guest_patch=none\n",
        static_cast<int>(pid), static_cast<int>(worker_tid),
        backend.native_body, linear, angular, pose, header.phase_return_0,
        header.phase_return_1, header.phase_return_2, duration_ms);
#endif
    std::fflush(stdout);

    const std::uint64_t deadline_ns =
        header.start_ns + duration_ms * 1000000ULL;
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
        if (signal == SIGTRAP && PeekDebug(tid, 6, &dr6) && (dr6 & 7UL)) {
            StackScopeEvent event{};
            event.sequence = header.event_count;
            event.monotonic_ns = MonotonicNs();
            event.tid = static_cast<std::int32_t>(tid);
#if A9TAS_EXECUTOR_AFFINITY_STACK
            if (dr6 & 1UL) {
                event.flags |= kStackScopeHitPose;
                ++header.pose_hits;
            }
#else
            if (dr6 & 1UL) {
                event.flags |= kStackScopeHitLinear;
                ++header.linear_hits;
            }
            if (dr6 & 2UL) {
                event.flags |= kStackScopeHitAngular;
                ++header.angular_hits;
            }
            if (dr6 & 4UL) {
                event.flags |= kStackScopeHitPose;
                ++header.pose_hits;
            }
#endif
            user_regs_struct registers{};
            const bool have_registers =
                ptrace(PTRACE_GETREGS, tid, nullptr, &registers) != -1;
            if (!have_registers) {
                ++header.ptrace_errors;
            } else {
                event.rip = static_cast<std::uint64_t>(registers.rip);
                event.rsp = static_cast<std::uint64_t>(registers.rsp);
            }
            const bool values_ok =
                ReadExact(mem, linear, &event.native_linear_bits,
                          sizeof(event.native_linear_bits)) &&
                ReadExact(mem, angular, &event.native_angular_bits,
                          sizeof(event.native_angular_bits)) &&
                ReadExact(mem, pose, &event.native_pose_bits,
                          sizeof(event.native_pose_bits));
            event.value_read_ok = values_ok ? 1u : 0u;
            if (!values_ok) ++header.value_read_errors;
            const bool stack_ok = have_registers &&
                ReadExact(mem, static_cast<std::uintptr_t>(registers.rsp),
                          event.stack_words, sizeof(event.stack_words));
            event.stack_read_ok = stack_ok ? 1u : 0u;
            if (!stack_ok) ++header.stack_read_errors;
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
    if (output_ok && header.event_count > 0 &&
        header.value_read_errors == 0 && header.stack_read_errors == 0 &&
        header.ptrace_errors == 0 && header.unexpected_stops == 0 &&
        header.final_threads == header.attached_threads)
        header.flags |= kStackScopeClean;
    if (std::fseek(out, 0, SEEK_SET) != 0 ||
        std::fwrite(&header, sizeof(header), 1, out) != 1 ||
        std::fflush(out) != 0 || std::ferror(out))
        output_ok = false;
    if (std::fclose(out) != 0) output_ok = false;
    close(mem);
#if A9TAS_EXECUTOR_AFFINITY_STACK
    std::printf(
        "EXECUTOR_STACK_AFFINITY_V1_DONE events=%" PRIu64
        " pose=%" PRIu64 " value_read_errors=%" PRIu64
        " stack_read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
        " unexpected_stops=%" PRIu64 " clean=%u path=%s\n",
        header.event_count, header.pose_hits, header.value_read_errors,
        header.stack_read_errors, header.ptrace_errors,
        header.unexpected_stops,
        (header.flags & kStackScopeClean) != 0, argv[4]);
#else
    std::printf(
        "WORKER_STACK_SCOPE_V1_DONE events=%" PRIu64
        " linear=%" PRIu64 " angular=%" PRIu64 " pose=%" PRIu64
        " value_read_errors=%" PRIu64 " stack_read_errors=%" PRIu64
        " ptrace_errors=%" PRIu64 " unexpected_stops=%" PRIu64
        " clean=%u path=%s\n",
        header.event_count, header.linear_hits, header.angular_hits,
        header.pose_hits, header.value_read_errors, header.stack_read_errors,
        header.ptrace_errors, header.unexpected_stops,
        (header.flags & kStackScopeClean) != 0, argv[4]);
#endif
    return output_ok ? 0 : 8;
}
