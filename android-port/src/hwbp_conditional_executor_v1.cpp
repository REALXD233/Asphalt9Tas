// BUILD-ONLY A9NPS1 conditional transform/linear executor.
//
// This stage implements the original AluTasV2 final-correction contract:
// compare 16 transform floats plus 3 linear-velocity floats component-wise;
// perform zero gameplay writes when all compare equal; otherwise write both
// exact recorded ranges (64+12) together at the certified Gate 2 boundary.
//
// No deployment runner is provided. Different-value runtime use remains
// unauthorized until a separate user decision after offline review.

#define A9TAS_SAME_BYTES_EXECUTOR_NO_MAIN
#include "hwbp_same_bytes_executor_v1.cpp"

#include <vector>

namespace {

constexpr char kConditionalMagic[8] = {
    'A', '9', 'C', 'D', 'T', '1', '\0', '\0',
};
constexpr std::uint32_t kConditionalVersion = 1;
constexpr std::size_t kMaximumConditionalFrames = 3600;
constexpr char kConditionalAcknowledgement[] =
    "I_ACCEPT_CONDITIONAL_A9NPS1_DIFFERENT_VALUES_V1";

enum ConditionalHeaderFlags : std::uint32_t {
    kConditionalTargetVerified = 1u << 0,
    kConditionalIdentityVerified = 1u << 1,
    kConditionalRecordingValidated = 1u << 2,
    kConditionalCleanDetach = 1u << 3,
};

enum ConditionalFrameFlags : std::uint32_t {
    kConditionalFrameEqual = 1u << 0,
    kConditionalFrameCorrected = 1u << 1,
    kConditionalFrameAuditExact = 1u << 2,
};

#pragma pack(push, 1)
struct ConditionalReportHeaderV1 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t frame_audit_size;
    std::uint32_t flags;
    std::uint32_t recording_frames;
    std::uint32_t processed_frames;
    std::uint32_t equal_frames;
    std::uint32_t corrected_frames;
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
    std::uint64_t rollback_attempts;
    std::uint64_t rollback_failures;
    std::uint64_t thread_additions;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
};

struct ConditionalFrameAuditV1 {
    std::uint64_t recording_tick;
    std::uint64_t recording_monotonic_ns;
    std::uint32_t flags;
    std::uint32_t reserved;
    std::uint8_t recorded_transform[64];
    std::uint8_t recorded_linear[12];
    AuditSnapshotV1 before;
    AuditSnapshotV1 immediate;
};
#pragma pack(pop)

static_assert(sizeof(ConditionalReportHeaderV1) == 208,
              "conditional report header ABI");
static_assert(sizeof(ConditionalFrameAuditV1) == 1708,
              "conditional frame audit ABI");

bool LoadConditionalRecording(
    const char* path,
    std::vector<a9tas::native_physics_v1::RecordingFrameV1>* frames) {
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    a9tas::native_physics_v1::RecordingHeaderV1 header{};
    const bool header_ok =
        std::fread(&header, sizeof(header), 1, file) == 1 &&
        std::memcmp(header.magic, a9tas::native_physics_v1::kMagic,
                    sizeof(header.magic)) == 0 &&
        header.version == a9tas::native_physics_v1::kVersion &&
        header.header_size == sizeof(header) &&
        header.frame_size ==
            sizeof(a9tas::native_physics_v1::RecordingFrameV1) &&
        header.frame_count > 0 &&
        header.frame_count <= kMaximumConditionalFrames &&
        std::memcmp(header.build_id,
                    a9tas::native_physics_v1::kSupportedBuildId,
                    sizeof(header.build_id)) == 0 &&
        header.transform_offset ==
            a9tas::native_physics_v1::kTransformOffset &&
        header.linear_velocity_offset ==
            a9tas::native_physics_v1::kLinearVelocityOffset &&
        header.transform_size == a9tas::native_physics_v1::kTransformSize &&
        header.linear_velocity_size ==
            a9tas::native_physics_v1::kLinearVelocitySize &&
        header.flags == a9tas::native_physics_v1::kRequiredHeaderFlags;
    if (!header_ok) {
        std::fclose(file);
        return false;
    }
    frames->resize(header.frame_count);
    const bool body_ok =
        std::fread(frames->data(), sizeof(frames->front()), frames->size(),
                   file) == frames->size() &&
        std::fgetc(file) == EOF;
    std::fclose(file);
    if (!body_ok) return false;

    std::uint64_t previous_tick = 0;
    std::uint64_t previous_time = 0;
    bool have_previous = false;
    for (const auto& frame : *frames) {
        if (frame.flags !=
            a9tas::native_physics_v1::kRequiredFrameFlags)
            return false;
        float payload[19]{};
        std::memcpy(payload, frame.transform_bits,
                    sizeof(frame.transform_bits));
        std::memcpy(payload + 16, frame.linear_velocity_bits,
                    sizeof(frame.linear_velocity_bits));
        if (!a9tas::vehicle_state_v1::Finite(payload, 19)) return false;
        if (have_previous &&
            (frame.tick != previous_tick + 1 ||
             frame.monotonic_ns < previous_time))
            return false;
        previous_tick = frame.tick;
        previous_time = frame.monotonic_ns;
        have_previous = true;
    }
    return true;
}

bool ComponentPayloadEqual(
    const AuditSnapshotV1& current,
    const a9tas::native_physics_v1::RecordingFrameV1& recorded) {
    float current_values[19]{};
    float recorded_values[19]{};
    std::memcpy(current_values,
                current.native_body +
                    a9tas::native_physics_v1::kTransformOffset,
                a9tas::native_physics_v1::kTransformSize);
    std::memcpy(current_values + 16,
                current.native_body +
                    a9tas::native_physics_v1::kLinearVelocityOffset,
                a9tas::native_physics_v1::kLinearVelocitySize);
    std::memcpy(recorded_values, recorded.transform_bits,
                sizeof(recorded.transform_bits));
    std::memcpy(recorded_values + 16, recorded.linear_velocity_bits,
                sizeof(recorded.linear_velocity_bits));
    for (std::size_t index = 0; index < 19; ++index)
        if (current_values[index] != recorded_values[index]) return false;
    return true;
}

AuditSnapshotV1 ExpectedImmediate(
    const AuditSnapshotV1& before,
    const a9tas::native_physics_v1::RecordingFrameV1& recorded,
    bool corrected) {
    AuditSnapshotV1 expected = before;
    if (corrected) {
        std::memcpy(expected.native_body +
                        a9tas::native_physics_v1::kTransformOffset,
                    recorded.transform_bits,
                    sizeof(recorded.transform_bits));
        std::memcpy(expected.native_body +
                        a9tas::native_physics_v1::kLinearVelocityOffset,
                    recorded.linear_velocity_bits,
                    sizeof(recorded.linear_velocity_bits));
    }
    return expected;
}

bool RollbackPayload(int mem, const a9tas::vehicle_state_v1::Layout& vehicle,
                     const a9tas::vehicle_state_v1::BackendLayout& backend,
                     std::uintptr_t library_base,
                     const AuditSnapshotV1& before) {
    // Attempt both restorations even if the first one fails: either original
    // pwrite may have transferred only a prefix before reporting failure.
    const bool pose_ok = WriteExactVerified(
        mem, backend.native_pose_address,
        before.native_body + a9tas::native_physics_v1::kTransformOffset,
        a9tas::native_physics_v1::kTransformSize);
    const bool linear_ok = WriteExactVerified(
        mem, backend.native_linear_address,
        before.native_body +
            a9tas::native_physics_v1::kLinearVelocityOffset,
        a9tas::native_physics_v1::kLinearVelocitySize);
    AuditSnapshotV1 restored{};
    const bool audit_ok =
        ReadAuditSnapshot(mem, vehicle, backend, library_base, &restored);
    return pose_ok && linear_ok && audit_ok &&
           std::memcmp(&restored, &before, sizeof(before)) == 0;
}

bool WriteConditionalReport(
    const char* path, const ConditionalReportHeaderV1& header,
    const std::vector<ConditionalFrameAuditV1>& audits) {
    const int report_fd =
        open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (report_fd < 0) return false;
    FILE* file = fdopen(report_fd, "wb");
    if (!file) {
        close(report_fd);
        std::remove(path);
        return false;
    }
    const bool ok =
        std::fwrite(&header, sizeof(header), 1, file) == 1 &&
        std::fwrite(audits.data(), sizeof(audits.front()), audits.size(),
                    file) == audits.size() &&
        std::fflush(file) == 0 && std::ferror(file) == 0;
    const bool close_ok = std::fclose(file) == 0;
    if (!ok || !close_ok) {
        std::remove(path);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 7 || argc > 8) {
        std::fprintf(
            stderr,
            "usage: %s PID LIB_BASE_HEX TIMEOUT_MS A9NPS1_PATH REPORT_PATH "
            "ACKNOWLEDGEMENT [PHYSICS_CONTEXT_HEX]\n",
            argv[0]);
        return 2;
    }
    if (std::strcmp(argv[6], kConditionalAcknowledgement) != 0) {
        std::fprintf(stderr,
                     "conditional acknowledgement missing; no process opened\n");
        return 2;
    }
    if (access(argv[5], F_OK) == 0) {
        std::fprintf(stderr,
                     "report path already exists; refusing to overwrite it\n");
        return 2;
    }
    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t timeout_value = 0;
    std::uint64_t context_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &timeout_value) ||
        (argc == 8 && !ParseUnsigned(argv[7], 16, &context_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || timeout_value < 1000 || timeout_value > 300000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    std::vector<a9tas::native_physics_v1::RecordingFrameV1> frames;
    if (!LoadConditionalRecording(argv[4], &frames)) {
        std::fprintf(stderr,
                     "invalid/non-finite/non-contiguous A9NPS1 recording\n");
        return 3;
    }

    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto timeout_ms = static_cast<std::uint64_t>(timeout_value);
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
        std::fprintf(stderr, "conditional address validation failed\n");
        close(mem);
        return 3;
    }

    ConditionalReportHeaderV1 report{};
    std::memcpy(report.magic, kConditionalMagic, sizeof(report.magic));
    report.version = kConditionalVersion;
    report.header_size = sizeof(report);
    report.frame_audit_size = sizeof(ConditionalFrameAuditV1);
    report.flags = kConditionalTargetVerified |
                   kConditionalIdentityVerified |
                   kConditionalRecordingValidated;
    report.recording_frames = static_cast<std::uint32_t>(frames.size());
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
        "CONDITIONAL_EXECUTOR_V1_BUILD_ONLY pid=%d frames=%zu"
        " native=0x%" PRIxPTR " transform=0x%" PRIxPTR
        " linear=0x%" PRIxPTR " threads=%zu\n",
        static_cast<int>(pid), frames.size(), backend.native_body,
        backend.native_pose_address, backend.native_linear_address,
        threads.size());
    std::fflush(stdout);

    ExecutorState state = ExecutorState::kWaiting;
    pid_t owner_tid = 0;
    std::size_t frame_index = 0;
    bool frame_pending = false;
    bool complete = false;
    std::uint32_t accounted_threads = 0;
    std::vector<ConditionalFrameAuditV1> audits;
    audits.reserve(frames.size());
    ConditionalFrameAuditV1 pending{};
    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + timeout_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 250000000ULL;

    while (MonotonicNs() < deadline_ns && report.read_errors == 0 &&
           report.ptrace_errors == 0 && report.semantic_errors == 0 &&
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
                        "conditional_fault reason=%s event=%" PRIu64
                        " frame=%zu state=%s dr6=0x%lx tid=%d owner=%d\n",
                        reason, report.event_count, frame_index,
                        ExecutorStateName(state), dr6,
                        static_cast<int>(tid), static_cast<int>(owner_tid));
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
                               dispatching == 0 && tid == owner_tid &&
                               frame_index < frames.size()) {
                        pending = {};
                        const auto& recorded = frames[frame_index];
                        pending.recording_tick = recorded.tick;
                        pending.recording_monotonic_ns =
                            recorded.monotonic_ns;
                        std::memcpy(pending.recorded_transform,
                                    recorded.transform_bits,
                                    sizeof(pending.recorded_transform));
                        std::memcpy(pending.recorded_linear,
                                    recorded.linear_velocity_bits,
                                    sizeof(pending.recorded_linear));
                        if (!ReadAuditSnapshot(mem, vehicle, backend, base,
                                               &pending.before)) {
                            ++report.read_errors;
                        } else {
                            const bool equal =
                                ComponentPayloadEqual(pending.before,
                                                      recorded);
                            const auto rollback = [&]() {
                                ++report.rollback_attempts;
                                if (!RollbackPayload(mem, vehicle, backend,
                                                     base, pending.before)) {
                                    ++report.rollback_failures;
                                    fail("rollback_failed");
                                    return false;
                                }
                                return true;
                            };
                            bool transport_ok = true;
                            if (equal) {
                                pending.flags |= kConditionalFrameEqual;
                                ++report.equal_frames;
                            } else {
                                pending.flags |=
                                    kConditionalFrameCorrected;
                                ++report.corrected_frames;
                                ++report.write_attempts;
                                const bool pose_ok = WriteExactVerified(
                                    mem, backend.native_pose_address,
                                    recorded.transform_bits,
                                    sizeof(recorded.transform_bits));
                                const bool linear_ok = WriteExactVerified(
                                    mem, backend.native_linear_address,
                                    recorded.linear_velocity_bits,
                                    sizeof(recorded.linear_velocity_bits));
                                transport_ok = pose_ok && linear_ok;
                                if (!transport_ok) {
                                    ++report.write_failures;
                                    const bool rollback_ok = rollback();
                                    fail(rollback_ok
                                             ? "transport_failed_rolled_back"
                                             : "transport_failed_rollback_failed");
                                }
                            }
                            if (transport_ok &&
                                !ReadAuditSnapshot(mem, vehicle, backend,
                                                   base,
                                                   &pending.immediate)) {
                                ++report.read_errors;
                                if (!equal) rollback();
                            } else if (transport_ok) {
                                const AuditSnapshotV1 expected =
                                    ExpectedImmediate(pending.before,
                                                      recorded, !equal);
                                if (std::memcmp(&expected,
                                                &pending.immediate,
                                                sizeof(expected)) != 0) {
                                    if (!equal) rollback();
                                    fail("immediate_audit_mismatch");
                                } else {
                                    pending.flags |=
                                        kConditionalFrameAuditExact;
                                    frame_pending = true;
                                }
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
                        if (!frame_pending)
                            fail("closed_cycle_without_frame_audit");
                        else {
                            audits.push_back(pending);
                            frame_pending = false;
                            ++frame_index;
                            report.processed_frames =
                                static_cast<std::uint32_t>(frame_index);
                            if (frame_index == frames.size()) complete = true;
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
    if (report.final_threads ==
        report.initial_threads + report.thread_additions)
        report.flags |= kConditionalCleanDetach;
    close(mem);

    const std::uint32_t required_flags =
        kConditionalTargetVerified | kConditionalIdentityVerified |
        kConditionalRecordingValidated | kConditionalCleanDetach;
    const bool success = complete && report.flags == required_flags &&
                         report.processed_frames == report.recording_frames &&
                         audits.size() == frames.size() &&
                         report.equal_frames + report.corrected_frames ==
                             report.recording_frames &&
                         report.read_errors == 0 &&
                         report.ptrace_errors == 0 &&
                         report.semantic_errors == 0 &&
                         report.write_attempts == report.corrected_frames &&
                         report.write_failures == 0 &&
                         report.rollback_attempts == 0 &&
                         report.rollback_failures == 0;
    bool report_ok = false;
    if (success) report_ok = WriteConditionalReport(argv[5], report, audits);
    std::printf(
        "CONDITIONAL_EXECUTOR_V1_DONE complete=%u processed=%u/%u"
        " equal=%u corrected=%u writes=%" PRIu64
        " write_failures=%" PRIu64 " rollbacks=%" PRIu64
        " rollback_failures=%" PRIu64 " read_errors=%" PRIu64
        " ptrace_errors=%" PRIu64 " semantic_errors=%" PRIu64
        " clean_detach=%u report=%s\n",
        complete, report.processed_frames, report.recording_frames,
        report.equal_frames, report.corrected_frames, report.write_attempts,
        report.write_failures, report.rollback_attempts,
        report.rollback_failures, report.read_errors, report.ptrace_errors,
        report.semantic_errors,
        (report.flags & kConditionalCleanDetach) != 0, argv[5]);
    return success && report_ok ? 0 : 8;
}
