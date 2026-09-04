// BUILD-ONLY same-bytes direct-data transport gate.
//
// This executable is intentionally limited to one operation: at a certified
// four-address Gate 2 callback-close boundary, read the current native 64+12
// payload and write those exact bytes back to the same two addresses. It has
// no recording input and therefore cannot apply a different state.
//
// Before and immediately after the two writes it compares the complete
// 0x290-byte NativePhysicsBody, the wrapper cache/native-pointer range, the
// high-level position/rotation/velocity mirrors, and the source velocity
// mirrors. Any changed byte fails closed. A second complete certified cycle
// is then observed read-only before a success report can be emitted.
//
// Do not deploy without separate, explicit user authorization.

#define A9TAS_PIPELINE_ORDER_NO_MAIN
#include "hwbp_pipeline_order_observer_v1.cpp"

#include "native_physics_recording_v1.h"

namespace {

constexpr char kAuditMagic[8] = {'A', '9', 'S', 'B', 'T', '1', '\0', '\0'};
constexpr std::uint32_t kAuditVersion = 1;
constexpr std::size_t kNativeBodyAuditSize = 0x290;
constexpr std::uintptr_t kWrapperAuditOffset = 0x50;
constexpr std::size_t kWrapperAuditSize = 0x48;
constexpr char kRequiredAcknowledgement[] =
    "I_ACCEPT_ONE_SAME_BYTES_64_12_WRITE_V1";

enum AuditFlags : std::uint32_t {
    kAuditTargetVerified = 1u << 0,
    kAuditIdentityVerified = 1u << 1,
    kAuditExactPrePostEqual = 1u << 2,
    kAuditNextCycleObserved = 1u << 3,
    kAuditCleanDetach = 1u << 4,
};

enum class ExecutorState {
    kWaiting,
    kCompleted,
    kCallbackOpen,
    kPublished,
    kCallbackClosed,
};

#pragma pack(push, 1)
struct AuditSnapshotV1 {
    std::uint8_t native_body[kNativeBodyAuditSize];
    std::uint8_t wrapper_cache_and_native[kWrapperAuditSize];
    float position[3];
    float rotation[4];
    float linear[3];
    float angular[3];
    std::uint8_t source_linear[12];
    std::uint8_t source_angular[12];
};

struct AuditReportHeaderV1 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t snapshot_size;
    std::uint32_t flags;
    std::uint8_t build_id[20];
    std::uint32_t reserved0;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t physics_context;
    std::uint64_t car_physics_state;
    std::uint64_t wrapper;
    std::uint64_t native_body;
    std::uint64_t transform_address;
    std::uint64_t linear_address;
    std::uint64_t event_count;
    std::uint64_t read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t semantic_errors;
    std::uint64_t write_attempts;
    std::uint64_t write_failures;
    std::uint64_t thread_additions;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
};
#pragma pack(pop)

static_assert(sizeof(AuditSnapshotV1) == 804, "same-bytes snapshot ABI");
static_assert(sizeof(AuditReportHeaderV1) == 176,
              "same-bytes report header ABI");

[[maybe_unused]] const char* ExecutorStateName(ExecutorState state) {
    switch (state) {
        case ExecutorState::kWaiting:
            return "waiting";
        case ExecutorState::kCompleted:
            return "completed";
        case ExecutorState::kCallbackOpen:
            return "callback_open";
        case ExecutorState::kPublished:
            return "published";
        case ExecutorState::kCallbackClosed:
            return "callback_closed";
    }
    return "unknown";
}

bool ExecutorIdentityAlive(
    int mem, const a9tas::vehicle_state_v1::Layout& vehicle,
    const a9tas::vehicle_state_v1::BackendLayout& backend,
    std::uintptr_t base) {
    std::uintptr_t interface_pointer = 0;
    std::uintptr_t native_pointer = 0;
    std::uintptr_t native_vtable = 0;
    return ReadExact(mem, vehicle.physics_base + 0x30, &interface_pointer,
                     sizeof(interface_pointer)) &&
           interface_pointer == backend.interface &&
           ReadExact(mem, backend.physics_velocity_interface + 0x90,
                     &native_pointer, sizeof(native_pointer)) &&
           native_pointer == backend.native_body &&
           ReadExact(mem, backend.native_body, &native_vtable,
                     sizeof(native_vtable)) &&
           native_vtable == base +
                                a9tas::vehicle_state_v1::
                                    kNativePhysicsBodyVtableRva;
}

bool SnapshotFinite(const AuditSnapshotV1& snapshot) {
    if (!a9tas::vehicle_state_v1::Finite(snapshot.position, 3) ||
        !a9tas::vehicle_state_v1::Finite(snapshot.rotation, 4) ||
        !a9tas::vehicle_state_v1::Finite(snapshot.linear, 3) ||
        !a9tas::vehicle_state_v1::Finite(snapshot.angular, 3))
        return false;
    const double rotation_norm =
        static_cast<double>(snapshot.rotation[0]) * snapshot.rotation[0] +
        static_cast<double>(snapshot.rotation[1]) * snapshot.rotation[1] +
        static_cast<double>(snapshot.rotation[2]) * snapshot.rotation[2] +
        static_cast<double>(snapshot.rotation[3]) * snapshot.rotation[3];
    if (rotation_norm < 0.25 || rotation_norm > 2.25) return false;
    constexpr std::size_t kTransformOffset =
        a9tas::native_physics_v1::kTransformOffset;
    constexpr std::size_t kLinearOffset =
        a9tas::native_physics_v1::kLinearVelocityOffset;
    float payload[19]{};
    std::memcpy(payload, snapshot.native_body + kTransformOffset,
                a9tas::native_physics_v1::kTransformSize);
    std::memcpy(payload + 16, snapshot.native_body + kLinearOffset,
                a9tas::native_physics_v1::kLinearVelocitySize);
    float source_velocities[6]{};
    std::memcpy(source_velocities, snapshot.source_linear,
                sizeof(snapshot.source_linear));
    std::memcpy(source_velocities + 3, snapshot.source_angular,
                sizeof(snapshot.source_angular));
    return a9tas::vehicle_state_v1::Finite(payload, 19) &&
           a9tas::vehicle_state_v1::Finite(source_velocities, 6);
}

bool ReadAuditSnapshot(
    int mem, const a9tas::vehicle_state_v1::Layout& vehicle,
    const a9tas::vehicle_state_v1::BackendLayout& backend,
    std::uintptr_t base, AuditSnapshotV1* output) {
    AuditSnapshotV1 snapshot{};
    if (!ExecutorIdentityAlive(mem, vehicle, backend, base) ||
        !ReadExact(mem, backend.native_body, snapshot.native_body,
                   sizeof(snapshot.native_body)) ||
        !ReadExact(mem, backend.base + kWrapperAuditOffset,
                   snapshot.wrapper_cache_and_native,
                   sizeof(snapshot.wrapper_cache_and_native)) ||
        !ReadExact(mem, vehicle.position_address, snapshot.position,
                   sizeof(snapshot.position)) ||
        !ReadExact(mem, vehicle.rotation_address, snapshot.rotation,
                   sizeof(snapshot.rotation)) ||
        !ReadExact(mem, vehicle.linear_address, snapshot.linear,
                   sizeof(snapshot.linear)) ||
        !ReadExact(mem, vehicle.angular_address, snapshot.angular,
                   sizeof(snapshot.angular)) ||
        !ReadExact(mem, backend.linear_source_address,
                   snapshot.source_linear, sizeof(snapshot.source_linear)) ||
        !ReadExact(mem, backend.angular_source_address,
                   snapshot.source_angular, sizeof(snapshot.source_angular)) ||
        !SnapshotFinite(snapshot))
        return false;
    *output = snapshot;
    return true;
}

bool WriteExactVerified(int mem, std::uintptr_t address, const void* data,
                        std::size_t size) {
    if (pwrite(mem, data, size, static_cast<off_t>(address)) !=
        static_cast<ssize_t>(size))
        return false;
    std::uint8_t verify[a9tas::native_physics_v1::kTransformSize]{};
    if (size > sizeof(verify) || !ReadExact(mem, address, verify, size))
        return false;
    return std::memcmp(verify, data, size) == 0;
}

[[maybe_unused]] bool WriteAuditReport(
    const char* path, const AuditReportHeaderV1& header,
    const AuditSnapshotV1& before, const AuditSnapshotV1& immediate,
    const AuditSnapshotV1& next_cycle) {
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const bool ok =
        std::fwrite(&header, sizeof(header), 1, file) == 1 &&
        std::fwrite(&before, sizeof(before), 1, file) == 1 &&
        std::fwrite(&immediate, sizeof(immediate), 1, file) == 1 &&
        std::fwrite(&next_cycle, sizeof(next_cycle), 1, file) == 1 &&
        std::fflush(file) == 0 && std::ferror(file) == 0;
    const bool close_ok = std::fclose(file) == 0;
    if (!ok || !close_ok) {
        std::remove(path);
        return false;
    }
    return true;
}

}  // namespace

#ifndef A9TAS_SAME_BYTES_EXECUTOR_NO_MAIN
int main(int argc, char** argv) {
    if (argc < 6 || argc > 7) {
        std::fprintf(
            stderr,
            "usage: %s PID LIB_BASE_HEX TIMEOUT_MS REPORT_PATH "
            "ACKNOWLEDGEMENT [PHYSICS_CONTEXT_HEX]\n",
            argv[0]);
        return 2;
    }
    if (std::strcmp(argv[5], kRequiredAcknowledgement) != 0) {
        std::fprintf(stderr,
                     "exact acknowledgement missing; no process opened\n");
        return 2;
    }
    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t timeout_value = 0;
    std::uint64_t context_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &timeout_value) ||
        (argc == 7 && !ParseUnsigned(argv[6], 16, &context_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || timeout_value < 1000 || timeout_value > 120000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto timeout_ms = static_cast<std::uint64_t>(timeout_value);
    if (access(argv[4], F_OK) == 0) {
        std::fprintf(stderr,
                     "report path already exists; refusing to overwrite it\n");
        return 2;
    }

    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported build; no thread was attached\n");
        return 3;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem < 0) return 4;

    std::uintptr_t context = 0;
    std::uintptr_t adapter = 0;
    std::uintptr_t world = 0;
    a9tas::vehicle_state_v1::Layout vehicle{};
    a9tas::vehicle_state_v1::BackendLayout backend{};
    if (!ResolvePhysicsContext(pid, mem, base,
                               static_cast<std::uintptr_t>(context_value),
                               &context, &adapter, &world) ||
        !a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle) ||
        !a9tas::vehicle_state_v1::ResolveBackendLayout(mem, base, vehicle,
                                                       &backend) ||
        !ExecutorIdentityAlive(mem, vehicle, backend, base)) {
        std::fprintf(stderr, "exact object resolution failed stage=%u\n",
                     backend.failure_stage);
        close(mem);
        return 3;
    }

    const std::uintptr_t completion =
        context + kContextCompletionTokenOffset;
    const std::uintptr_t callback_flags =
        context + kContextCallbackFlagsOffset;
    const std::uintptr_t f64 = vehicle.physics_base + kCarPhysicsF64Offset;
    const std::uintptr_t accumulator = world + kWorldAccumulatorOffset;
    std::vector<Mapping> maps;
    if (!ReadMaps(pid, &maps) ||
        !PipelineWritable(maps, completion, 8) ||
        !PipelineWritable(maps, callback_flags, 2) ||
        !PipelineWritable(maps, f64, 4) ||
        !PipelineWritable(maps, accumulator, 4) ||
        !PipelineWritable(maps, backend.native_body,
                          kNativeBodyAuditSize) ||
        !PipelineWritable(maps, backend.base + kWrapperAuditOffset,
                          kWrapperAuditSize)) {
        std::fprintf(stderr, "audit address validation failed\n");
        close(mem);
        return 3;
    }

    AuditReportHeaderV1 report{};
    std::memcpy(report.magic, kAuditMagic, sizeof(report.magic));
    report.version = kAuditVersion;
    report.header_size = sizeof(report);
    report.snapshot_size = sizeof(AuditSnapshotV1);
    report.flags = kAuditTargetVerified | kAuditIdentityVerified;
    std::memcpy(report.build_id,
                a9tas::native_physics_v1::kSupportedBuildId,
                sizeof(report.build_id));
    report.pid = static_cast<std::uint64_t>(pid);
    report.library_base = base;
    report.physics_context = context;
    report.car_physics_state = vehicle.physics_base;
    report.wrapper = backend.base;
    report.native_body = backend.native_body;
    report.transform_address = backend.native_pose_address;
    report.linear_address = backend.native_linear_address;

    std::vector<TracedThread> threads;
    std::uint64_t attach_failures = 0;
    const std::size_t initial = AttachNewThreads(
        pid, completion, callback_flags, f64, accumulator, &threads,
        &attach_failures, PipelineDr7());
    report.initial_threads = static_cast<std::uint32_t>(initial);
    report.ptrace_errors += attach_failures;
    if (initial == 0) {
        close(mem);
        return 7;
    }

    std::printf(
        "SAME_BYTES_EXECUTOR_V1_BUILD_ONLY pid=%d context=0x%" PRIxPTR
        " car_state=0x%" PRIxPTR " wrapper=0x%" PRIxPTR
        " native=0x%" PRIxPTR " transform=0x%" PRIxPTR
        " linear=0x%" PRIxPTR " threads=%zu"
        " mode=one-same-bytes-64+12-only\n",
        static_cast<int>(pid), context, vehicle.physics_base, backend.base,
        backend.native_body, backend.native_pose_address,
        backend.native_linear_address, threads.size());
    std::fflush(stdout);

    AuditSnapshotV1 before{};
    AuditSnapshotV1 immediate{};
    AuditSnapshotV1 next_cycle{};
    ExecutorState state = ExecutorState::kWaiting;
    pid_t owner_tid = 0;
    bool write_cycle_pending = false;
    bool write_cycle_committed = false;
    bool next_cycle_pending = false;
    bool complete = false;
    std::uint32_t accounted_threads = 0;
    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + timeout_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 250000000ULL;

    while (MonotonicNs() < deadline_ns && report.semantic_errors == 0 &&
           report.read_errors == 0 && report.ptrace_errors == 0 &&
           !complete) {
        const std::uint64_t now_ns = MonotonicNs();
        if (now_ns >= next_rescan_ns) {
            std::uint64_t failures = 0;
            const std::size_t added = AttachNewThreads(
                pid, completion, callback_flags, f64, accumulator, &threads,
                &failures, PipelineDr7());
            report.thread_additions += added;
            report.ptrace_errors += failures;
            next_rescan_ns = now_ns + 250000000ULL;
        }
        int status = 0;
        const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        if (tid < 0) {
            if (errno == EINTR) continue;
            if (errno != ECHILD) ++report.ptrace_errors;
            continue;
        }
        TracedThread* tracked = FindThread(&threads, tid);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (tracked && tracked->live) {
                tracked->live = false;
                tracked->stopped = false;
                ++accounted_threads;
            }
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        if (tracked) tracked->stopped = true;
        const int signal = WSTOPSIG(status);
        unsigned long dr6 = 0;
        const bool have_dr6 = PeekDebug(tid, 6, &dr6);
        if (signal == SIGTRAP && have_dr6 && (dr6 & 15UL)) {
            ++report.event_count;
            std::uint16_t flags_value = 0;
            std::uint32_t f64_bits = 0;
            std::uint32_t accumulator_bits = 0;
            if (!ReadExact(mem, callback_flags, &flags_value,
                           sizeof(flags_value)) ||
                !ReadExact(mem, f64, &f64_bits, sizeof(f64_bits)) ||
                !ReadExact(mem, accumulator, &accumulator_bits,
                           sizeof(accumulator_bits))) {
                ++report.read_errors;
            } else {
                const auto fail = [&](const char* reason) {
                    std::fprintf(
                        stderr,
                        "same_bytes_fault reason=%s event=%" PRIu64
                        " state=%s dr6=0x%lx tid=%d owner=%d flags=0x%04x\n",
                        reason, report.event_count, ExecutorStateName(state),
                        dr6, static_cast<int>(tid),
                        static_cast<int>(owner_tid), flags_value);
                    ++report.semantic_errors;
                };
                if (dr6 & 1UL) {
                    if (state != ExecutorState::kWaiting)
                        fail("completion_before_commit");
                    else {
                        state = ExecutorState::kCompleted;
                        owner_tid = tid;
                    }
                }
                if ((dr6 & 2UL) && report.semantic_errors == 0) {
                    const std::uint8_t dispatching =
                        static_cast<std::uint8_t>(flags_value & 0xFFu);
                    if (state == ExecutorState::kWaiting) {
                        // Ignore incomplete prefix.
                    } else if (state == ExecutorState::kCompleted &&
                               dispatching == 1 && tid == owner_tid) {
                        state = ExecutorState::kCallbackOpen;
                    } else if (state == ExecutorState::kPublished &&
                               dispatching == 0 && tid == owner_tid) {
                        if (!write_cycle_committed) {
                            if (!ReadAuditSnapshot(mem, vehicle, backend, base,
                                                   &before)) {
                                ++report.read_errors;
                            } else {
                                ++report.write_attempts;
                                const bool pose_ok = WriteExactVerified(
                                    mem, backend.native_pose_address,
                                    before.native_body +
                                        a9tas::native_physics_v1::
                                            kTransformOffset,
                                    a9tas::native_physics_v1::
                                        kTransformSize);
                                const bool linear_ok = WriteExactVerified(
                                    mem, backend.native_linear_address,
                                    before.native_body +
                                        a9tas::native_physics_v1::
                                            kLinearVelocityOffset,
                                    a9tas::native_physics_v1::
                                        kLinearVelocitySize);
                                if (!pose_ok || !linear_ok) {
                                    ++report.write_failures;
                                } else if (!ReadAuditSnapshot(
                                               mem, vehicle, backend, base,
                                               &immediate)) {
                                    ++report.read_errors;
                                } else if (std::memcmp(
                                               &before, &immediate,
                                               sizeof(before)) != 0) {
                                    fail("immediate_audit_changed");
                                } else {
                                    report.flags |=
                                        kAuditExactPrePostEqual;
                                    write_cycle_pending = true;
                                }
                            }
                        } else {
                            if (!ReadAuditSnapshot(mem, vehicle, backend, base,
                                                   &next_cycle)) {
                                ++report.read_errors;
                            } else {
                                next_cycle_pending = true;
                            }
                        }
                        state = ExecutorState::kCallbackClosed;
                    } else if (state == ExecutorState::kCallbackClosed &&
                               dispatching == 0) {
                        // Expected deferred clear.
                    } else {
                        fail("unexpected_callback_transition");
                    }
                }
                if ((dr6 & 4UL) && report.semantic_errors == 0) {
                    float value = 0.0f;
                    std::memcpy(&value, &f64_bits, sizeof(value));
                    if (state == ExecutorState::kWaiting) {
                        // Ignore incomplete prefix.
                    } else if (state != ExecutorState::kCallbackOpen ||
                               tid != owner_tid || !std::isfinite(value) ||
                               std::fabs(value) > 1000000.0f) {
                        fail("unexpected_f64");
                    } else {
                        state = ExecutorState::kPublished;
                    }
                }
                if ((dr6 & 8UL) && report.semantic_errors == 0) {
                    float value = 0.0f;
                    std::memcpy(&value, &accumulator_bits, sizeof(value));
                    if (!std::isfinite(value) || std::fabs(value) > 60.0f) {
                        fail("invalid_accumulator");
                    } else if (state == ExecutorState::kCallbackClosed) {
                        if (write_cycle_pending) {
                            write_cycle_pending = false;
                            write_cycle_committed = true;
                        } else if (next_cycle_pending) {
                            next_cycle_pending = false;
                            report.flags |= kAuditNextCycleObserved;
                            complete = true;
                        } else {
                            fail("closed_cycle_without_snapshot");
                        }
                        state = ExecutorState::kWaiting;
                        owner_tid = 0;
                    } else if (state != ExecutorState::kWaiting) {
                        fail("accumulator_before_close");
                    }
                }
            }
            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
                ++report.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        } else {
            ++report.semantic_errors;
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver))
                ++report.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        }
    }

    for (auto& thread : threads) {
        if (!thread.live) continue;
        if (!ClearAndDetach(thread.tid, thread.stopped))
            ++report.ptrace_errors;
        else
            ++accounted_threads;
        thread.live = false;
        thread.stopped = false;
    }
    report.final_threads = accounted_threads;
    if (accounted_threads == threads.size()) report.flags |= kAuditCleanDetach;
    close(mem);

    const std::uint32_t required =
        kAuditTargetVerified | kAuditIdentityVerified |
        kAuditExactPrePostEqual | kAuditNextCycleObserved |
        kAuditCleanDetach;
    const bool success = complete && report.flags == required &&
                         report.read_errors == 0 &&
                         report.ptrace_errors == 0 &&
                         report.semantic_errors == 0 &&
                         report.write_attempts == 1 &&
                         report.write_failures == 0 &&
                         report.final_threads ==
                             report.initial_threads +
                                 report.thread_additions;
    bool report_ok = false;
    if (success)
        report_ok = WriteAuditReport(argv[4], report, before, immediate,
                                     next_cycle);
    std::printf(
        "SAME_BYTES_EXECUTOR_V1_DONE complete=%u writes=%" PRIu64
        " write_failures=%" PRIu64 " read_errors=%" PRIu64
        " ptrace_errors=%" PRIu64 " semantic_errors=%" PRIu64
        " audit_exact=%u next_cycle=%u clean_detach=%u report=%s\n",
        complete, report.write_attempts, report.write_failures,
        report.read_errors, report.ptrace_errors, report.semantic_errors,
        (report.flags & kAuditExactPrePostEqual) != 0,
        (report.flags & kAuditNextCycleObserved) != 0,
        (report.flags & kAuditCleanDetach) != 0, argv[4]);
    return success && report_ok ? 0 : 8;
}
#endif
