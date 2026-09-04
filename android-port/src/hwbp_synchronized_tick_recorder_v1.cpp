// Synchronized A9UTK1 recorder for the proven Android tick subset.
//
// This recorder reuses the live-proven Gate 6-9 phase machine. It writes only
// the fixed delta at the certified tick-opening boundary. Steering is observed
// after the game's C9C write; transform+linear are read after callback close;
// a frame is committed only at the independent world-accumulator boundary.
// The independent brake-capture build also records the low 32-bit raw value
// after proving that C98 and C9C published the same finite control bits.
// Accelerator and respawn remain explicitly skipped. Barrel fields remain
// skipped in every proven build; the review-only A9TAS_BARREL_CAPTURE_V1
// variant reads the statically and live-identified Android candidates without
// changing any existing recorder binary. The natural-action recording build
// additionally captures real NitroService calls with AluTasV2's
// count-before-original-call semantics.

#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
#include "race_lifecycle_object_resolver_v1.h"
#endif
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
#include "natural_action_recording_host_v1.h"
#endif

#define A9TAS_UNIFIED_TICK_EXECUTOR_NO_MAIN
#include "hwbp_unified_tick_executor_v1.cpp"
#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
#include "natural_preroll_anchor_v1.h"
#endif

#include <signal.h>
#include <sys/ptrace.h>

#if defined(A9TAS_SYNC_ACTION_WINDOW_V1) && \
    (!defined(A9TAS_SYNC_BRAKE_CAPTURE_V1) || \
     !defined(A9TAS_NATURAL_PREROLL_ANCHOR_V1))
#error "action-window recorder requires brake capture and natural anchor"
#endif
#if defined(A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1) && \
    !defined(A9TAS_SYNC_ACTION_WINDOW_V1)
#error "until-release recorder requires the action-window protocol"
#endif
#if defined(A9TAS_RACE_LIFECYCLE_SOURCE_V1) && \
    defined(A9TAS_NATURAL_PREROLL_ANCHOR_V1)
#error "lifecycle source v1 is the direct startline recorder, not natural preroll"
#endif
#if defined(A9TAS_NATURAL_ACTION_RECORDING_V1) && \
    (!defined(A9TAS_RACE_LIFECYCLE_SOURCE_V1) || \
     !defined(A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1) || \
     !defined(A9TAS_SYNC_BRAKE_CAPTURE_V1))
#error "natural-action recording requires lifecycle, startline and brake capture"
#endif

namespace {

#ifdef A9TAS_BARREL_CAPTURE_V1
constexpr std::uintptr_t kBarrelRbxOwnerOffset = 0x1968;
#endif

#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
constexpr char kSyncReportMagic[8] = {
    'A', '9', 'U', 'S', 'R', '6', '\0', '\0',
};
constexpr std::uint32_t kSyncReportVersion = 6;
constexpr char kSyncAcknowledgement[] =
    "I_ACCEPT_SYNC_RECORDER_NATURAL_ACTION_V1";
#elif defined(A9TAS_RACE_LIFECYCLE_SOURCE_V1)
constexpr char kSyncReportMagic[8] = {
    'A', '9', 'U', 'S', 'R', '5', '\0', '\0',
};
constexpr std::uint32_t kSyncReportVersion = 5;
constexpr char kSyncAcknowledgement[] =
    "I_ACCEPT_SYNC_RECORDER_RACE_LIFECYCLE_V1";
#elif defined(A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1)
constexpr char kSyncReportMagic[8] = {
    'A', '9', 'U', 'S', 'R', '4', '\0', '\0',
};
constexpr std::uint32_t kSyncReportVersion = 4;
constexpr char kSyncAcknowledgement[] =
    "I_ACCEPT_SYNC_ACTION_UNTIL_RELEASE_V1";
#elif defined(A9TAS_SYNC_ACTION_WINDOW_V1)
constexpr char kSyncReportMagic[8] = {
    'A', '9', 'U', 'S', 'R', '3', '\0', '\0',
};
constexpr std::uint32_t kSyncReportVersion = 3;
constexpr char kSyncAcknowledgement[] =
    "I_ACCEPT_SYNC_ACTION_WINDOW_HOST_TRIGGER_V1";
#elif defined(A9TAS_SYNC_BRAKE_CAPTURE_V1)
constexpr char kSyncReportMagic[8] = {
    'A', '9', 'U', 'S', 'R', '2', '\0', '\0',
};
constexpr std::uint32_t kSyncReportVersion = 2;
constexpr char kSyncAcknowledgement[] =
    "I_ACCEPT_SYNC_RECORDER_FIXED_DELTA_BRAKE_CAPTURE_V1";
#else
constexpr char kSyncReportMagic[8] = {
    'A', '9', 'U', 'S', 'R', '1', '\0', '\0',
};
constexpr std::uint32_t kSyncReportVersion = 1;
constexpr char kSyncAcknowledgement[] =
    "I_ACCEPT_SYNC_RECORDER_FIXED_DELTA_V1";
#endif
constexpr std::uint32_t kSyncRecordedSkipFlags =
#ifndef A9TAS_SYNC_BRAKE_CAPTURE_V1
    a9tas::unified_tick_v1::kSkipBrake |
#endif
#ifndef A9TAS_NATURAL_ACTION_RECORDING_V1
    a9tas::unified_tick_v1::kSkipNitroActivation |
#endif
    a9tas::unified_tick_v1::kSkipAccelerator |
#ifndef A9TAS_BARREL_CAPTURE_V1
    a9tas::unified_tick_v1::kSkipBarrelAngular |
    a9tas::unified_tick_v1::kSkipBarrelRbx |
#endif
    a9tas::unified_tick_v1::kSkipRespawnButton;
#if defined(A9TAS_SYNC_BRAKE_CAPTURE_V1) && \
    defined(A9TAS_NATURAL_ACTION_RECORDING_V1) && \
    defined(A9TAS_BARREL_CAPTURE_V1)
static_assert(kSyncRecordedSkipFlags == 0x48,
              "synchronized natural-action barrel recorder scope");
#elif defined(A9TAS_SYNC_BRAKE_CAPTURE_V1) && \
    defined(A9TAS_NATURAL_ACTION_RECORDING_V1)
static_assert(kSyncRecordedSkipFlags == 0x78,
              "synchronized natural-action recorder scope");
#elif defined(A9TAS_SYNC_BRAKE_CAPTURE_V1)
static_assert(kSyncRecordedSkipFlags == 0x7c,
              "synchronized brake recorder scope");
#else
static_assert(kSyncRecordedSkipFlags == 0x7e,
              "synchronized recorder scope");
#endif

enum SyncReportFlag : std::uint32_t {
    kSyncTargetVerified = 1u << 0,
    kSyncIdentityVerified = 1u << 1,
    kSyncFixedDeltaOnly = 1u << 2,
    kSyncSteeringCaptured = 1u << 3,
    kSyncPhysicsCaptured = 1u << 4,
    kSyncCleanDetach = 1u << 5,
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
    kSyncBrakeCaptured = 1u << 6,
#endif
#ifdef A9TAS_SYNC_ACTION_WINDOW_V1
    kSyncHostTriggerObserved = 1u << 7,
#endif
#ifdef A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1
    kSyncActionReleaseCompleted = 1u << 8,
#endif
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
    kSyncRaceLifecycleWitnessed = 1u << 9,
#elif defined(A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1)
    kSyncInputCycleAnchorWitnessed = 1u << 9,
#endif
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
    kSyncNitroCaptured = 1u << 10,
#endif
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
    kSyncCompletionWriteCertificate = 1u << 11,
#endif
};

#pragma pack(push, 1)
struct SyncReportHeaderV1 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t frame_audit_size;
    std::uint32_t flags;
    std::uint32_t target_frames;
    std::uint32_t captured_frames;
    std::uint8_t build_id[20];
    std::uint32_t reserved0;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t main_object;
    std::uint64_t final_owner;
    std::uint64_t physics_context;
    std::uint64_t completion_address;
    std::uint64_t callback_flags_address;
    std::uint64_t f64_address;
    std::uint64_t inner_world;
    std::uint64_t world_accumulator_address;
    std::uint64_t car_physics_state;
    std::uint64_t wrapper;
    std::uint64_t native_body;
    std::uint64_t transform_address;
    std::uint64_t linear_address;
    std::uint64_t event_count;
    std::uint64_t delta_writes;
    std::uint64_t read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t semantic_errors;
    std::uint64_t unexpected_stops;
    std::uint64_t thread_additions;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
    std::uint64_t action_payload_load_bias;
    std::uint64_t action_wrapper;
    std::uint64_t action_control;
    std::uint64_t action_evidence;
    std::uint64_t action_counts;
    std::uint64_t action_shadow;
    std::uint64_t action_service;
    std::uint64_t action_original_vptr;
    std::uint64_t action_wrapper_entries;
    std::uint64_t action_original_calls;
    std::uint64_t action_clean_returns;
    std::uint64_t action_counted_calls;
    std::uint64_t action_out_of_window_calls;
    std::uint64_t action_overflow_calls;
    std::uint64_t action_failures;
    std::uint64_t action_count_sum;
    std::uint32_t action_session_id;
    std::uint32_t action_cleanup_freeze_passes;
    std::int32_t action_last_status;
    std::uint32_t action_last_tid;
#endif
};

struct SyncFrameAuditV1 {
    std::uint64_t tick;
    std::uint64_t monotonic_ns;
    std::int32_t cycle_tid;
    std::int32_t commit_tid;
    std::int64_t original_delta_us;
    std::int64_t applied_delta_us;
    std::uint64_t delta_event;
    std::uint64_t c98_event;
    std::uint64_t c9c_prefix_event;
    std::uint64_t f64_event;
    std::uint64_t callback_close_event;
    std::uint64_t callback_deferred_clear_event;
    std::uint64_t world_commit_event;
    std::uint64_t completion_before;
    std::uint64_t completion_after;
    std::uint16_t callback_flags_at_c9c;
    std::uint8_t reserved[6];
    std::uint64_t c98_pair_after;
    std::uint64_t c9c_pair_after;
    std::uint8_t transform[64];
    std::uint8_t linear[12];
};
#pragma pack(pop)

#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
static_assert(sizeof(SyncReportHeaderV1) == 384,
              "natural-action synchronized report header ABI");
#else
static_assert(sizeof(SyncReportHeaderV1) == 240,
              "synchronized report header ABI");
#endif
static_assert(sizeof(SyncFrameAuditV1) == 212,
              "synchronized frame audit ABI");

bool ReadSynchronizedFrame(
    int mem, std::uint64_t tick, std::uint32_t steering_bits,
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
    std::uint32_t brake_bits,
#endif
    const a9tas::vehicle_state_v1::Layout& vehicle,
    const a9tas::vehicle_state_v1::BackendLayout& backend,
    std::uintptr_t base, RecordingFrameV1* output) {
    if (!output ||
        !ExecutorIdentityAlive(mem, vehicle, backend, base))
        return false;
    RecordingFrameV1 frame{};
    frame.tick = tick;
    frame.monotonic_ns = MonotonicNs();
    std::memcpy(&frame.steering, &steering_bits, sizeof(frame.steering));
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
    std::memcpy(&frame.brake, &brake_bits, sizeof(frame.brake));
#endif
    frame.skip_override_flags = kSyncRecordedSkipFlags;
    frame.flags = a9tas::unified_tick_v1::kRequiredFrameFlags;
    if (!std::isfinite(frame.steering) || std::fabs(frame.steering) > 1.05f ||
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
        !std::isfinite(frame.brake) || std::fabs(frame.brake) > 1.05f ||
#endif
        !ReadExact(mem, backend.native_pose_address, frame.transform_bits,
                   sizeof(frame.transform_bits)) ||
        !ReadExact(mem, backend.native_linear_address,
                   frame.linear_velocity_bits,
                   sizeof(frame.linear_velocity_bits))
#ifdef A9TAS_BARREL_CAPTURE_V1
        || backend.angular_source_base == 0 ||
        backend.angular_source_base >
            UINTPTR_MAX - kBarrelRbxOwnerOffset -
                              sizeof(frame.barrel_rbx) ||
        !ReadExact(mem, backend.native_angular_address,
                   frame.barrel_angular_velocity,
                   sizeof(frame.barrel_angular_velocity)) ||
        !ReadExact(mem,
                   backend.angular_source_base + kBarrelRbxOwnerOffset,
                   frame.barrel_rbx, sizeof(frame.barrel_rbx))
#endif
    )
        return false;
    float physics[19]{};
    std::memcpy(physics, frame.transform_bits, sizeof(frame.transform_bits));
    std::memcpy(physics + 16, frame.linear_velocity_bits,
                sizeof(frame.linear_velocity_bits));
    if (!FiniteBounded(physics, 19, 1000000.0f)) return false;
#ifdef A9TAS_BARREL_CAPTURE_V1
    if (!FiniteBounded(frame.barrel_angular_velocity, 3, 1000000.0f) ||
        !FiniteBounded(frame.barrel_rbx, 2, 1000000.0f))
        return false;
#endif
    *output = frame;
    return true;
}

bool WriteSynchronizedRecording(
    const char* path, std::uint32_t fixed_interval_us,
    const std::vector<RecordingFrameV1>& frames) {
    if (!path || frames.empty() ||
        frames.size() > a9tas::unified_tick_v1::kMaximumFrames)
        return false;
    RecordingHeaderV1 header{};
    std::memcpy(header.magic, a9tas::unified_tick_v1::kMagic,
                sizeof(header.magic));
    header.version = a9tas::unified_tick_v1::kVersion;
    header.header_size = sizeof(header);
    header.frame_size = sizeof(RecordingFrameV1);
    header.frame_count = static_cast<std::uint32_t>(frames.size());
    header.fixed_interval_us = fixed_interval_us;
    header.flags = a9tas::unified_tick_v1::kRequiredHeaderFlags;
    header.supported_skip_mask =
        a9tas::unified_tick_v1::kSupportedSkipMask;
    std::memcpy(header.build_id,
                a9tas::unified_tick_v1::kSupportedBuildId,
                sizeof(header.build_id));
    header.transform_offset = a9tas::unified_tick_v1::kTransformOffset;
    header.linear_velocity_offset =
        a9tas::unified_tick_v1::kLinearVelocityOffset;
    header.angular_velocity_offset =
        a9tas::unified_tick_v1::kAngularVelocityOffset;
    header.transform_size = a9tas::unified_tick_v1::kTransformSize;
    header.linear_velocity_size =
        a9tas::unified_tick_v1::kLinearVelocitySize;
    header.angular_velocity_size =
        a9tas::unified_tick_v1::kAngularVelocitySize;

    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        std::remove(path);
        return false;
    }
    const bool ok =
        std::fwrite(&header, sizeof(header), 1, file) == 1 &&
        std::fwrite(frames.data(), sizeof(frames.front()), frames.size(),
                    file) == frames.size() &&
        std::fflush(file) == 0 && std::ferror(file) == 0;
    const bool close_ok = std::fclose(file) == 0;
    if (!ok || !close_ok) {
        std::remove(path);
        return false;
    }
    return true;
}

bool WriteSyncReport(const char* path, const SyncReportHeaderV1& header,
                     const std::vector<SyncFrameAuditV1>& audits) {
    if (!path || audits.empty()) return false;
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
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

#ifdef A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1
bool CreateInputCycleSourceReady(const char* path, pid_t pid,
                                 std::size_t frozen_threads,
                                 std::uint32_t freeze_passes
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
                                 , std::uintptr_t lifecycle_state_address
#endif
                                 ) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    // The lifecycle READY proof carries the object-derived state address and
    // the complete frozen-thread witness.  Keep its larger buffer isolated so
    // the already-proven input-cycle build stays byte-for-byte unchanged.
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
    char payload[384]{};
#else
    char payload[256]{};
#endif
    const int length = std::snprintf(
        payload, sizeof(payload),
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
        "READY_ARMED_RACE_LIFECYCLE_SOURCE_V1 pid=%d controller_pid=%d "
        "state_address=0x%" PRIxPTR " state=2 "
        "target_threads_attached=%zu all_target_threads_frozen=1 "
        "thread_set_stable=1 freeze_passes=%u "
        "gameplay_state_writes=1 service_vptr_swaps=1 payload_staged=1 "
        "host_resume_gate=marker_removal\n",
#else
        "READY_ARMED_RACE_LIFECYCLE_SOURCE_V1 pid=%d controller_pid=%d "
        "state_address=0x%" PRIxPTR " state=2 "
        "target_threads_attached=%zu all_target_threads_frozen=1 "
        "thread_set_stable=1 freeze_passes=%u "
        "gameplay_state_writes=0 host_resume_gate=marker_removal\n",
#endif
        static_cast<int>(pid), static_cast<int>(getpid()),
        lifecycle_state_address, frozen_threads, freeze_passes);
#else
        "READY_ARMED_INPUT_CYCLE_SOURCE_V1 pid=%d controller_pid=%d "
        "target_threads_attached=%zu all_target_threads_frozen=1 "
        "thread_set_stable=1 freeze_passes=%u "
        "gameplay_state_writes=0 host_resume_gate=marker_removal\n",
        static_cast<int>(pid), static_cast<int>(getpid()), frozen_threads,
        freeze_passes);
#endif
    bool ok = length > 0 && static_cast<std::size_t>(length) < sizeof(payload);
    std::size_t written = 0;
    while (ok && written < static_cast<std::size_t>(length)) {
        const ssize_t count = write(
            fd, payload + written,
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

bool WaitForInputCycleSourceResume(const char* path,
                                   std::uint64_t timeout_ms) {
    const std::uint64_t now = MonotonicNs();
    if (now == 0 || timeout_ms > UINT64_MAX / 1000000ULL) return false;
    const std::uint64_t budget = timeout_ms * 1000000ULL;
    if (now > UINT64_MAX - budget) return false;
    const std::uint64_t deadline = now + budget;
    while (MonotonicNs() < deadline) {
        if (access(path, F_OK) != 0) return errno == ENOENT;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return false;
}

bool FreezeInputCycleSourceThreads(
    pid_t pid, std::uintptr_t accumulator_address,
    std::uintptr_t c98_address, std::uintptr_t c9c_address,
    std::uintptr_t state_watch_address, std::vector<TracedThread>* threads,
    std::uint64_t* failures, std::uint32_t* completed_passes) {
    if (!threads || !failures || !completed_passes) return false;
    *completed_passes = 0;
    for (std::uint32_t pass = 0; pass < 4; ++pass) {
        for (auto& thread : *threads) {
            if (!thread.live || thread.stopped) continue;
            if (!StopThread(thread.tid)) {
                ++*failures;
                return false;
            }
            thread.stopped = true;
        }
        const std::size_t added = AttachNewThreadsStopped(
            pid, accumulator_address, c98_address, c9c_address,
            state_watch_address, threads, failures, BoundaryDr7(true));
        *completed_passes = pass + 1;
        if (*failures != 0) return false;
        if (added == 0) return !threads->empty();
    }
    return false;
}

#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
unsigned long RaceLifecycleSourceDr7() {
    return 1UL | (1UL << 16) | (3UL << 18);
}

bool FreezeRaceLifecycleSourceThreads(
    pid_t pid, std::uintptr_t state_address,
    std::vector<TracedThread>* threads, std::uint64_t* failures,
    std::uint32_t* completed_passes) {
    if (!threads || !failures || !completed_passes || state_address == 0)
        return false;
    *completed_passes = 0;
    for (std::uint32_t pass = 0; pass < 4; ++pass) {
        for (auto& thread : *threads) {
            if (!thread.live || thread.stopped) continue;
            if (!StopThread(thread.tid)) {
                ++*failures;
                return false;
            }
            thread.stopped = true;
        }
        const std::size_t added = AttachNewThreadsStopped(
            pid, state_address, 0, 0, 0, threads, failures,
            RaceLifecycleSourceDr7());
        *completed_passes = pass + 1;
        if (*failures != 0) return false;
        if (added == 0) return !threads->empty();
    }
    return false;
}

bool WaitForRaceLifecycleSourceStart(
    pid_t pid, int mem, std::uint64_t timeout_ms,
    std::uintptr_t state_address, std::uintptr_t delta_address,
    std::uintptr_t c98_address, std::uintptr_t c9c_address,
    std::uintptr_t world_accumulator, std::vector<TracedThread>* threads,
    SyncReportHeaderV1* report, std::uint32_t* retired_threads,
    const char** failure_reason, std::uint64_t* observed_lifecycle_events
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
    , a9tas::natural_action_recording_host_v1::Runtime* action_runtime
#endif
    ) {
    if (failure_reason) *failure_reason = "invalid_arguments";
    if (observed_lifecycle_events) *observed_lifecycle_events = 0;
    if (!threads || !report || !retired_threads || !failure_reason ||
        !observed_lifecycle_events || state_address == 0
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
        || action_runtime == nullptr
#endif
        )
        return false;
    const std::uint64_t start_ns = MonotonicNs();
    if (start_ns == 0 || timeout_ms > (UINT64_MAX - start_ns) / 1000000ULL) {
        *failure_reason = "invalid_deadline";
        return false;
    }
    const std::uint64_t deadline_ns = start_ns + timeout_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 100000000ULL;
    std::uint64_t lifecycle_events = 0;
    const auto fail = [&](const char* reason) {
        *failure_reason = reason;
        *observed_lifecycle_events = lifecycle_events;
        return false;
    };
    while (MonotonicNs() < deadline_ns && report->read_errors == 0 &&
           report->ptrace_errors == 0 && report->semantic_errors == 0) {
        const std::uint64_t now_ns = MonotonicNs();
        if (now_ns >= next_rescan_ns) {
            std::uint64_t failures = 0;
            const std::size_t added = AttachNewThreads(
                pid, state_address, 0, 0, 0, threads, &failures,
                RaceLifecycleSourceDr7());
            report->thread_additions += added;
            report->ptrace_errors += failures;
            next_rescan_ns = now_ns + 100000000ULL;
        }
        int status = 0;
        const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        if (tid < 0) {
            if (errno == EINTR) continue;
            if (errno != ECHILD) ++report->ptrace_errors;
            continue;
        }
        TracedThread* tracked = FindThread(threads, tid);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (tracked && tracked->live) {
                tracked->live = false;
                tracked->stopped = false;
                ++*retired_threads;
            }
            if (WIFSIGNALED(status) || WEXITSTATUS(status) != 0) {
                ++report->semantic_errors;
                return fail("thread_abnormal_exit");
            }
            continue;
        }
        if (!WIFSTOPPED(status) || tracked == nullptr) {
            ++report->semantic_errors;
            return fail("untracked_or_nonstop_wait");
        }
        tracked->stopped = true;
        const int signal = WSTOPSIG(status);
        unsigned long dr6 = 0;
        if (signal == SIGTRAP && PeekDebug(tid, 6, &dr6) &&
            (dr6 & 1UL) != 0) {
            ++lifecycle_events;
            std::uint32_t state = 0;
            if (!ReadExact(mem, state_address, &state, sizeof(state))) {
                ++report->read_errors;
                return fail("state_read_failed");
            }
            if (lifecycle_events != 1) {
                ++report->semantic_errors;
                return fail("lifecycle_event_count");
            }
            if (state != a9tas::race_lifecycle_v1::kRacingState) {
                ++report->semantic_errors;
                return fail("state_not_racing");
            }
            std::uint32_t freeze_passes = 0;
            if (!FreezeRaceLifecycleSourceThreads(
                    pid, state_address, threads, &report->ptrace_errors,
                    &freeze_passes)) {
                ++report->semantic_errors;
                return fail("handoff_freeze_failed");
            }
            for (auto& thread : *threads) {
                if (!thread.live) continue;
                if (!thread.stopped ||
                    !ProgramStoppedThread(
                        thread.tid, delta_address, c98_address, c9c_address,
                        world_accumulator, BoundaryDr7(true))) {
                    ++report->ptrace_errors;
                    return fail("tick_watchpoint_program_failed");
                }
            }
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
            // Publish frame 1 only after the lifecycle 2->3 transition and
            // while the full target thread set is still frozen.
            if (!a9tas::natural_action_recording_host_v1::ArmFirstFrame(
                    mem, action_runtime)) {
                ++report->semantic_errors;
                return fail("natural_action_arm_failed");
            }
#endif
            for (auto& thread : *threads) {
                if (!thread.live || !thread.stopped) continue;
                if (!ContinueThread(thread.tid)) {
                    ++report->ptrace_errors;
                    return fail("tick_watchpoint_resume_failed");
                }
                thread.stopped = false;
            }
            std::printf(
                "AUTHORITATIVE_RACE_SOURCE_START_V1 state=3 events=%" PRIu64
                " event_tid=%d event_ns=%" PRIu64
                " tick_watchpoints_armed_before_continue=1\n",
                lifecycle_events, static_cast<int>(tid),
                MonotonicNs() - start_ns);
            std::fflush(stdout);
            *failure_reason = "none";
            *observed_lifecycle_events = lifecycle_events;
            return true;
        }
        ++report->unexpected_stops;
        ++report->semantic_errors;
        const int deliver = signal == SIGTRAP ? 0 : signal;
        if (!ContinueThread(tid, deliver)) ++report->ptrace_errors;
        return fail("unexpected_stop");
    }
    ++report->semantic_errors;
    if (report->read_errors != 0) return fail("read_error");
    if (report->ptrace_errors != 0) return fail("ptrace_error");
    return fail("lifecycle_timeout");
}
#endif

bool ResumeInputCycleSourceThreads(std::vector<TracedThread>* threads,
                                   std::uint32_t* retired_threads) {
    if (!threads || !retired_threads) return false;
    std::size_t resumed = 0;
    for (auto& thread : *threads) {
        if (!thread.live || !thread.stopped) continue;
        errno = 0;
        if (ContinueThread(thread.tid)) {
            thread.stopped = false;
            ++resumed;
            continue;
        }
        const int continue_errno = errno;
        int status = 0;
        pid_t waited = -1;
        do {
            waited = waitpid(thread.tid, &status, __WALL | WNOHANG);
        } while (waited == -1 && errno == EINTR);
        if (waited == thread.tid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0) {
            thread.live = false;
            thread.stopped = false;
            ++*retired_threads;
            continue;
        }
        std::fprintf(
            stderr,
            "input_cycle_source_resume_failed tid=%d errno=%d waited=%d"
            " status=0x%x resumed=%zu retired=%u\n",
            static_cast<int>(thread.tid), continue_errno,
            static_cast<int>(waited), status, resumed, *retired_threads);
        return false;
    }
    return true;
}
#endif

#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
bool WriteNaturalPrerollAnchor(
    const char* path, const a9tas::natural_preroll_v1::AnchorV1& anchor) {
    if (!path) return false;
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const ssize_t written = write(fd, &anchor, sizeof(anchor));
    const bool close_ok = close(fd) == 0;
    if (written != static_cast<ssize_t>(sizeof(anchor)) || !close_ok) {
        std::remove(path);
        return false;
    }
    return true;
}
#endif

}  // namespace

#ifndef A9TAS_SYNC_RECORDER_MAIN_NAME
#define A9TAS_SYNC_RECORDER_MAIN_NAME main
#endif

int A9TAS_SYNC_RECORDER_MAIN_NAME(int argc, char** argv) {
#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
    if (argc < 10 || argc > 13) {
        std::fprintf(
            stderr,
            "usage: %s PID LIB_BASE_HEX TIMEOUT_MS TARGET_FRAMES "
            "FIXED_INTERVAL_US A9UTK1_OUT "
#ifdef A9TAS_SYNC_ACTION_WINDOW_V1
            "A9USR3_OUT "
#elif defined(A9TAS_SYNC_BRAKE_CAPTURE_V1)
            "A9USR2_OUT "
#else
            "A9USR1_OUT "
#endif
            "A9NPA1_RAW_OUT ACK "
            "[PHYSICS_CONTEXT_HEX] [MAIN_OBJECT_HEX] [FINAL_OWNER_HEX]\n",
            argv[0]);
        return 2;
    }
    constexpr int kAckArgument = 9;
    constexpr int kContextArgument = 10;
#else
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
    if (argc < 11 || argc > 14) {
#else
    if (argc < 9 || argc > 12) {
#endif
        std::fprintf(
            stderr,
            "usage: %s PID LIB_BASE_HEX TIMEOUT_MS TARGET_FRAMES "
            "FIXED_INTERVAL_US A9UTK1_OUT "
#ifdef A9TAS_SYNC_ACTION_WINDOW_V1
            "A9USR3_OUT ACK "
#elif defined(A9TAS_SYNC_BRAKE_CAPTURE_V1)
            "A9USR2_OUT ACK "
#else
            "A9USR1_OUT ACK "
#endif
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
            "LIFECYCLE_OBJECT_HEX LIFECYCLE_STATE_HEX "
#endif
            "[PHYSICS_CONTEXT_HEX] [MAIN_OBJECT_HEX] [FINAL_OWNER_HEX]\n",
            argv[0]);
        return 2;
    }
    constexpr int kAckArgument = 8;
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
    constexpr int kLifecycleObjectArgument = 9;
    constexpr int kLifecycleStateArgument = 10;
    constexpr int kContextArgument = 11;
#else
    constexpr int kContextArgument = 9;
#endif
#endif
    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t timeout_value = 0;
    std::uint64_t target_frames_value = 0;
    std::uint64_t fixed_interval_value = 0;
    std::uint64_t context_value = 0;
    std::uint64_t main_object_value = 0;
    std::uint64_t final_owner_value = 0;
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
    std::uint64_t lifecycle_object_value = 0;
    std::uint64_t lifecycle_state_value = 0;
#endif
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &timeout_value) ||
        !ParseUnsigned(argv[4], 10, &target_frames_value) ||
        !ParseUnsigned(argv[5], 10, &fixed_interval_value) ||
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
        !ParseUnsigned(argv[kLifecycleObjectArgument], 16,
                       &lifecycle_object_value) ||
        !ParseUnsigned(argv[kLifecycleStateArgument], 16,
                       &lifecycle_state_value) ||
#endif
        (argc >= kContextArgument + 1 &&
         !ParseUnsigned(argv[kContextArgument], 16, &context_value)) ||
        (argc >= kContextArgument + 2 &&
         !ParseUnsigned(argv[kContextArgument + 1], 16,
                        &main_object_value)) ||
        (argc == kContextArgument + 3 &&
         !ParseUnsigned(argv[kContextArgument + 2], 16,
                        &final_owner_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || timeout_value < 1000 || timeout_value > 300000 ||
        target_frames_value == 0 ||
        target_frames_value > a9tas::unified_tick_v1::kMaximumFrames ||
        fixed_interval_value <
            a9tas::unified_tick_v1::kMinimumFixedIntervalUs ||
        fixed_interval_value >
            a9tas::unified_tick_v1::kMaximumFixedIntervalUs) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    if (std::strcmp(argv[kAckArgument], kSyncAcknowledgement) != 0) {
        std::fprintf(stderr,
                     "fixed-delta recording acknowledgement missing\n");
        return 2;
    }
    if (std::strcmp(argv[6], argv[7]) == 0) {
        std::fprintf(stderr, "recording and report paths must differ\n");
        return 2;
    }
#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
    if (std::strcmp(argv[6], argv[8]) == 0 ||
        std::strcmp(argv[7], argv[8]) == 0) {
        std::fprintf(stderr, "recording, report and anchor paths must differ\n");
        return 2;
    }
#endif

    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto timeout_ms = static_cast<std::uint64_t>(timeout_value);
    const auto target_frames = static_cast<std::size_t>(target_frames_value);
    const auto fixed_interval_us =
        static_cast<std::uint32_t>(fixed_interval_value);
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
    const auto lifecycle_object =
        static_cast<std::uintptr_t>(lifecycle_object_value);
    const auto lifecycle_state =
        static_cast<std::uintptr_t>(lifecycle_state_value);
    if (lifecycle_object == 0 || lifecycle_state == 0 ||
        lifecycle_object >
            UINTPTR_MAX - a9tas::race_lifecycle_v1::kPhaseStateOffset ||
        lifecycle_state !=
            lifecycle_object + a9tas::race_lifecycle_v1::kPhaseStateOffset ||
        (lifecycle_state & 3u) != 0) {
        std::fprintf(stderr, "lifecycle source object/state binding invalid\n");
        return 2;
    }
#endif
#ifdef A9TAS_SYNC_ACTION_WINDOW_V1
    char action_start_path[96]{};
    std::snprintf(action_start_path, sizeof(action_start_path),
                  "/data/local/tmp/a9tas_action_window_start_%d",
                  static_cast<int>(pid));
    if (access(action_start_path, F_OK) == 0) {
        std::fprintf(stderr,
                     "stale action-window trigger exists; no thread attached\n");
        return 3;
    }
#endif
    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported build; no thread was attached\n");
        return 3;
    }

    std::uintptr_t main_object = 0;
    std::uintptr_t final_owner = 0;
    if (!ResolveMainObject(pid, base,
                           static_cast<std::uintptr_t>(main_object_value),
                           &main_object) ||
        !ResolveFinalOwner(pid, base,
                           static_cast<std::uintptr_t>(final_owner_value),
                           &final_owner)) {
        std::fprintf(stderr, "scheduler object resolution failed\n");
        return 3;
    }

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem < 0) return 4;

#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
    std::uintptr_t lifecycle_vptr = 0;
    a9tas::race_lifecycle_v1::Candidate lifecycle_candidate{};
    if (!a9tas::race_lifecycle_v1::VerifyTargetBuild(mem, base) ||
        !a9tas::race_lifecycle_v1::ReadExact(
            mem, lifecycle_object, &lifecycle_vptr, sizeof(lifecycle_vptr)) ||
        !a9tas::race_lifecycle_v1::ValidateCandidate(
            mem, base, lifecycle_object, lifecycle_vptr,
            &lifecycle_candidate) ||
        lifecycle_candidate.state_address != lifecycle_state ||
        lifecycle_candidate.state !=
            a9tas::race_lifecycle_v1::kCountdownState) {
        std::fprintf(stderr,
                     "lifecycle source countdown candidate invalid; no attach\n");
        close(mem);
        return 3;
    }
#endif

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
        std::fprintf(stderr,
                     "synchronized recorder object resolution failed stage=%u\n",
                     backend.failure_stage);
        close(mem);
        return 3;
    }

    const std::uintptr_t delta_address = main_object + kAccumulatorOffset;
    const std::uintptr_t c98_address = final_owner + kC98Offset;
    const std::uintptr_t c9c_address = final_owner + kC9COffset;
    const std::uintptr_t completion =
        context + kContextCompletionTokenOffset;
    const std::uintptr_t callback_flags =
        context + kContextCallbackFlagsOffset;
    const std::uintptr_t f64 = vehicle.physics_base + kCarPhysicsF64Offset;
    const std::uintptr_t world_accumulator = world + kWorldAccumulatorOffset;

    std::vector<Mapping> maps;
    if ((delta_address & 7u) != 0 || (c98_address & 7u) != 0 ||
        c9c_address != c98_address + 4 || !ReadMaps(pid, &maps) ||
        !PipelineWritable(maps, delta_address, 8) ||
        !PipelineWritable(maps, c98_address, 8) ||
        !PipelineWritable(maps, completion, 8) ||
        !PipelineWritable(maps, callback_flags, 2) ||
        !PipelineWritable(maps, f64, 4) ||
        !PipelineWritable(maps, world_accumulator, 4) ||
        !PipelineWritable(maps, backend.native_pose_address,
                          a9tas::unified_tick_v1::kTransformSize) ||
        !PipelineWritable(maps, backend.native_linear_address,
                          a9tas::unified_tick_v1::kLinearVelocitySize)) {
        std::fprintf(stderr, "synchronized recording address validation failed\n");
        close(mem);
        return 3;
    }

    SyncReportHeaderV1 report{};
    std::memcpy(report.magic, kSyncReportMagic, sizeof(report.magic));
    report.version = kSyncReportVersion;
    report.header_size = sizeof(report);
    report.frame_audit_size = sizeof(SyncFrameAuditV1);
    report.flags = kSyncTargetVerified | kSyncIdentityVerified |
                   kSyncFixedDeltaOnly | kSyncSteeringCaptured |
                   kSyncPhysicsCaptured
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
                   | kSyncBrakeCaptured
#endif
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
                   | kSyncCompletionWriteCertificate
#endif
                   ;
    report.target_frames = static_cast<std::uint32_t>(target_frames);
    std::memcpy(report.build_id,
                a9tas::unified_tick_v1::kSupportedBuildId,
                sizeof(report.build_id));
    report.pid = static_cast<std::uint64_t>(pid);
    report.library_base = base;
    report.main_object = main_object;
    report.final_owner = final_owner;
    report.physics_context = context;
    report.completion_address = completion;
    report.callback_flags_address = callback_flags;
    report.f64_address = f64;
    report.inner_world = world;
    report.world_accumulator_address = world_accumulator;
    report.car_physics_state = vehicle.physics_base;
    report.wrapper = backend.base;
    report.native_body = backend.native_body;
    report.transform_address = backend.native_pose_address;
    report.linear_address = backend.native_linear_address;
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
    a9tas::natural_action_recording_host_v1::Runtime action_runtime{};
    const std::uint32_t action_session =
        (static_cast<std::uint32_t>(pid) ^
         static_cast<std::uint32_t>(target_frames) ^ 0xa9c10000u) | 1u;
    const char* action_resolve_failure = nullptr;
    if (!a9tas::natural_action_recording_host_v1::Resolve(
            mem, pid, base, final_owner,
            static_cast<std::uint32_t>(target_frames), action_session,
            &action_runtime, &action_resolve_failure)) {
        std::fprintf(stderr,
                     "natural-action recording payload/owner resolution failed "
                     "stage=%s\n",
                     action_resolve_failure ? action_resolve_failure : "unknown");
        close(mem);
        return 3;
    }
    report.action_payload_load_bias = action_runtime.payload.load_bias;
    report.action_wrapper = action_runtime.payload.wrapper;
    report.action_control = action_runtime.payload.control;
    report.action_evidence = action_runtime.payload.evidence;
    report.action_counts = action_runtime.payload.counts;
    report.action_shadow = action_runtime.payload.shadow;
    report.action_service = action_runtime.service;
    report.action_original_vptr = action_runtime.original_vptr;
    report.action_session_id = action_session;
#endif

#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
    a9tas::natural_preroll_v1::AnchorV1 natural_anchor{};
    std::memcpy(natural_anchor.magic, a9tas::natural_preroll_v1::kMagic,
                sizeof(natural_anchor.magic));
    natural_anchor.version = a9tas::natural_preroll_v1::kVersion;
    natural_anchor.size = sizeof(natural_anchor);
    natural_anchor.flags = a9tas::natural_preroll_v1::kUnboundFlags;
    natural_anchor.fixed_interval_us = fixed_interval_us;
    natural_anchor.frame_count = static_cast<std::uint32_t>(target_frames);
    std::memcpy(natural_anchor.build_id,
                a9tas::unified_tick_v1::kSupportedBuildId,
                sizeof(natural_anchor.build_id));
    natural_anchor.source_pid = static_cast<std::uint64_t>(pid);
    natural_anchor.source_library_base = base;
    natural_anchor.source_main_object = main_object;
    natural_anchor.source_final_owner = final_owner;
    natural_anchor.source_physics_context = context;
    natural_anchor.source_native_body = backend.native_body;
    bool natural_anchor_captured = false;
#endif

    std::vector<TracedThread> threads;
    std::uint64_t attach_failures = 0;
#ifdef A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1
    std::uint32_t freeze_passes = 0;
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
    const bool initial_freeze_stable = FreezeRaceLifecycleSourceThreads(
        pid, lifecycle_state, &threads, &attach_failures, &freeze_passes);
#else
    const bool initial_freeze_stable = FreezeInputCycleSourceThreads(
        pid, delta_address, c98_address, c9c_address, world_accumulator,
        &threads, &attach_failures, &freeze_passes);
#endif
    const std::size_t initial = threads.size();
#else
    const std::size_t initial = AttachNewThreads(
        pid, delta_address, c98_address, c9c_address, world_accumulator,
        &threads, &attach_failures, BoundaryDr7(true));
#endif
    report.initial_threads = static_cast<std::uint32_t>(initial);
    report.ptrace_errors += attach_failures;
    if (initial == 0 || attach_failures != 0
#ifdef A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1
        || !initial_freeze_stable
#endif
    ) {
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        close(mem);
        return 7;
    }
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
    if (!a9tas::natural_action_recording_host_v1::Stage(mem,
                                                        &action_runtime)) {
        std::fprintf(stderr,
                     "natural-action recording hook stage failed while frozen\n");
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        close(mem);
        return 7;
    }
#endif

#ifdef A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1
    char input_cycle_ready_path[112]{};
    std::snprintf(input_cycle_ready_path, sizeof(input_cycle_ready_path),
                  "/data/local/tmp/a9tas_input_cycle_source_ready_%d",
                  static_cast<int>(pid));
    const std::uint64_t ready_timeout_ms =
        std::min<std::uint64_t>(timeout_ms, 20000ULL);
    if (access(input_cycle_ready_path, F_OK) == 0 ||
        !CreateInputCycleSourceReady(input_cycle_ready_path, pid, initial,
                                     freeze_passes
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
                                     , lifecycle_state
#endif
                                     )) {
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
        a9tas::natural_action_recording_host_v1::Result action_cleanup{};
        (void)a9tas::natural_action_recording_host_v1::Finish(
            mem, &action_runtime, &action_cleanup);
#endif
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        close(mem);
        return 7;
    }
    std::printf(
        "INPUT_CYCLE_SOURCE_READY_ARMED pid=%d controller_pid=%d "
        "threads=%zu all_target_threads_frozen=1 thread_set_stable=1 "
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
        "freeze_passes=%u gameplay_state_writes=1 service_vptr_swaps=1 "
        "payload_staged=1\n",
#else
        "freeze_passes=%u gameplay_state_writes=0\n",
#endif
        static_cast<int>(pid), static_cast<int>(getpid()), initial,
        freeze_passes);
    std::fflush(stdout);
    std::uint32_t pre_loop_retired_threads = 0;
    const bool host_release_observed = WaitForInputCycleSourceResume(
        input_cycle_ready_path, ready_timeout_ms);
    if (!host_release_observed)
        std::fprintf(stderr,
                     "input_cycle_source_host_release_timeout timeout_ms=%" PRIu64
                     "\n",
                     ready_timeout_ms);
    const bool target_threads_resumed =
        host_release_observed &&
        ResumeInputCycleSourceThreads(&threads,
                                      &pre_loop_retired_threads);
    if (!target_threads_resumed) {
        unlink(input_cycle_ready_path);
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
        std::uint64_t action_freeze_failures = 0;
        std::uint32_t action_freeze_passes = 0;
        (void)FreezeRaceLifecycleSourceThreads(
            pid, lifecycle_state, &threads, &action_freeze_failures,
            &action_freeze_passes);
        a9tas::natural_action_recording_host_v1::Result action_cleanup{};
        (void)a9tas::natural_action_recording_host_v1::Finish(
            mem, &action_runtime, &action_cleanup);
#endif
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        close(mem);
        return 7;
    }
    std::printf(
        "INPUT_CYCLE_SOURCE_RESUME_RELEASED pid=%d target_threads=%zu"
        " clean_retired=%u\n",
        static_cast<int>(pid), initial, pre_loop_retired_threads);
    std::fflush(stdout);
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
    const char* lifecycle_failure_reason = "not_started";
    std::uint64_t lifecycle_events_observed = 0;
    if (!WaitForRaceLifecycleSourceStart(
            pid, mem, timeout_ms, lifecycle_state, delta_address,
            c98_address, c9c_address, world_accumulator, &threads, &report,
            &pre_loop_retired_threads, &lifecycle_failure_reason,
            &lifecycle_events_observed
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
            , &action_runtime
#endif
            )) {
        std::uint64_t freeze_failures = 0;
        std::uint32_t cleanup_passes = 0;
        FreezeRaceLifecycleSourceThreads(
            pid, lifecycle_state, &threads, &freeze_failures,
            &cleanup_passes);
        report.ptrace_errors += freeze_failures;
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
        a9tas::natural_action_recording_host_v1::Result action_cleanup{};
        if (!a9tas::natural_action_recording_host_v1::Finish(
                mem, &action_runtime, &action_cleanup))
            ++report.semantic_errors;
#endif
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        close(mem);
        std::fprintf(stderr,
                     "authoritative lifecycle source handoff failed closed;"
                     " reason=%s lifecycle_events=%" PRIu64
                     " retired=%u read_errors=%" PRIu64
                     " ptrace_errors=%" PRIu64
                     " semantic_errors=%" PRIu64
                     " unexpected_stops=%" PRIu64
                     " cleanup_failures=%" PRIu64
                     " cleanup_passes=%u captured_frames=0\n",
                     lifecycle_failure_reason, lifecycle_events_observed,
                     pre_loop_retired_threads, report.read_errors,
                     report.ptrace_errors, report.semantic_errors,
                     report.unexpected_stops, freeze_failures, cleanup_passes);
        return 7;
    }
    report.flags |= kSyncRaceLifecycleWitnessed;
#endif
#endif

    std::printf(
        "SYNCHRONIZED_TICK_RECORDER_V1 pid=%d target_frames=%zu fixed=%u "
        "main=0x%" PRIxPTR " owner=0x%" PRIxPTR
        " context=0x%" PRIxPTR " native=0x%" PRIxPTR
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
        " threads=%zu write_scope=fixed-delta-plus-passive-nitro-vptr\n",
#else
        " threads=%zu write_scope=fixed-delta-only\n",
#endif
        static_cast<int>(pid), target_frames, fixed_interval_us, main_object,
        final_owner, context, backend.native_body, threads.size());
    std::fflush(stdout);
#ifdef A9TAS_SYNC_ACTION_WINDOW_V1
    std::printf("SYNC_ACTION_WINDOW_ARMED trigger=%s wait_gameplay_writes=0\n",
                action_start_path);
    std::fflush(stdout);
#endif

#if defined(A9TAS_PAUSED_ANCHOR_WARMUP_V1) || \
    defined(A9TAS_NATURAL_PREROLL_ANCHOR_V1)
    // A paused outer loop can publish a nonzero candidate and cancel it with
    // a following zero delta. Observe one complete natural cycle without any
    // write before enabling fixed-delta recording. Cancelled paused candidates
    // return to waiting and remain in warmup. The action-window build remains
    // here until its host trigger exists on a neutral control cycle.
    bool warmup_pending = true;
    Machine machine{Stage::kWaiting, 0, 1};
#else
    Machine machine{Stage::kWaiting, 0, target_frames};
#endif
    pid_t cycle_tid = 0;
    pid_t post_phase_tid = 0;
    SyncFrameAuditV1 pending_audit{};
    RecordingFrameV1 pending_frame{};
    bool have_pending_audit = false;
    bool have_pending_frame = false;
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
    bool completion_write_observed = false;
#endif
    bool complete = false;
    std::vector<RecordingFrameV1> frames;
    std::vector<SyncFrameAuditV1> audits;
    frames.reserve(target_frames);
    audits.reserve(target_frames);
#ifdef A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1
    constexpr std::uint32_t kActionPostReleaseFrames = 30;
    bool action_observed = false;
    bool brake_release_observed = false;
    std::uint32_t post_release_frames = 0;
    std::uint64_t warmup_rejected_candidates = 0;
#endif
    std::uint32_t accounted_threads =
#ifdef A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1
        pre_loop_retired_threads;
#else
        0;
#endif
    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + timeout_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 250000000ULL;
#if defined(A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1) && \
    !defined(A9TAS_RACE_LIFECYCLE_SOURCE_V1)
    // Keep source frame zero identical to the live-passed final writer:
    // a complete same-thread C98 -> C9C -> delta-zero witness arms capture,
    // and the following positive delta opens source frame zero.
    enum class InputCycleSourceAnchorStage : std::uint8_t {
        kAwaitC98,
        kAwaitC9C,
        kAwaitDeltaZero,
        kReady,
    };
    InputCycleSourceAnchorStage input_cycle_anchor_stage =
        InputCycleSourceAnchorStage::kAwaitC98;
    pid_t input_cycle_anchor_tid = 0;
    std::uint64_t input_cycle_anchor_skipped_deltas = 0;
#endif

    const auto semantic_fault = [&](const char* reason, pid_t tid,
                                    unsigned long dr6) {
        std::fprintf(stderr,
                     "sync_recorder_fault reason=%s event=%" PRIu64
                     " frame=%zu stage=%u dr6=0x%lx tid=%d owner=%d\n",
                     reason, report.event_count, machine.frame_index,
                     static_cast<unsigned>(machine.stage), dr6,
                     static_cast<int>(tid), static_cast<int>(cycle_tid));
        ++report.semantic_errors;
    };

    const auto rearm_post_owner_for_input = [&](pid_t current_tid) {
        if (post_phase_tid <= 0) return false;
        if (post_phase_tid == current_tid)
            return ProgramStoppedThread(
                current_tid, delta_address, c98_address, c9c_address,
                world_accumulator, BoundaryDr7(true));
        TracedThread* owner = FindThread(&threads, post_phase_tid);
        if (!owner || !owner->live) return false;
        if (!owner->stopped && !StopThread(post_phase_tid)) return false;
        owner->stopped = true;
        if (!ProgramStoppedThread(
                post_phase_tid, delta_address, c98_address, c9c_address,
                world_accumulator, BoundaryDr7(true)))
            return false;
        if (!ContinueThread(post_phase_tid)) return false;
        owner->stopped = false;
        return true;
    };

    while (MonotonicNs() < deadline_ns && report.read_errors == 0 &&
           report.ptrace_errors == 0 && report.semantic_errors == 0 &&
           !complete) {
        const std::uint64_t now_ns = MonotonicNs();
        if (now_ns >= next_rescan_ns) {
            std::uint64_t failures = 0;
            const std::size_t added = AttachNewThreads(
                pid, delta_address, c98_address, c9c_address,
                world_accumulator, &threads, &failures,
                BoundaryDr7(true));
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
                if (WIFSIGNALED(status) || WEXITSTATUS(status) != 0) {
                    semantic_fault("thread_abnormal_exit", tid,
                                   static_cast<unsigned long>(status));
                } else if (tid == cycle_tid) {
                    semantic_fault("owner_exited", tid, 0);
                }
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
            const unsigned long hit = dr6 & 15UL;
            if ((hit & (hit - 1)) != 0) {
                semantic_fault("ambiguous_multi_hit", tid, dr6);
            } else if (hit == 8UL &&
                       machine.stage == Stage::kWaitWorldCommit) {
                std::uint32_t bits = 0;
                float value = 0.0f;
                if (!ReadExact(mem, world_accumulator, &bits, sizeof(bits))) {
                    ++report.read_errors;
                } else {
                    std::memcpy(&value, &bits, sizeof(value));
                    std::uint32_t actions = 0;
                    if (!std::isfinite(value) || std::fabs(value) > 60.0f ||
                        !have_pending_audit || !have_pending_frame ||
                        tid == cycle_tid ||
                        pending_audit.callback_deferred_clear_event == 0 ||
                        !Advance(&machine, Event::kWorldCommit, false, false,
                                 false, &actions)) {
                        semantic_fault("unexpected_world_commit", tid, dr6);
                    } else {
                        pending_audit.commit_tid =
                            static_cast<std::int32_t>(tid);
                        pending_audit.world_commit_event = report.event_count;
#if defined(A9TAS_PAUSED_ANCHOR_WARMUP_V1) || \
    defined(A9TAS_NATURAL_PREROLL_ANCHOR_V1)
                        if (warmup_pending) {
#ifdef A9TAS_SYNC_ACTION_WINDOW_V1
                            // The host trigger lives outside game state. Only
                            // accept it on a fully neutral certified control
                            // pair so the saved anchor precedes player input.
                            const bool accept_warmup =
                                access(action_start_path, F_OK) == 0 &&
                                pending_audit.c98_pair_after == 0 &&
                                pending_audit.c9c_pair_after == 0;
#elif defined(A9TAS_SYNC_BRAKE_CAPTURE_V1)
                            // Remain observational until the player naturally
                            // publishes a clear brake press. The accepted
                            // trigger cycle becomes the pre-roll anchor; frame
                            // zero starts on the following certified tick.
                            const bool accept_warmup =
                                pending_frame.brake <= -0.5f;
#endif
#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
                            if (accept_warmup) {
#endif
                            natural_anchor.cycle_tid =
                                pending_audit.cycle_tid;
                            natural_anchor.commit_tid =
                                pending_audit.commit_tid;
                            natural_anchor.events[0] =
                                pending_audit.delta_event;
                            natural_anchor.events[1] =
                                pending_audit.c98_event;
                            natural_anchor.events[2] =
                                pending_audit.c9c_prefix_event;
                            natural_anchor.events[3] =
                                pending_audit.f64_event;
                            natural_anchor.events[4] =
                                pending_audit.callback_close_event;
                            natural_anchor.events[5] =
                                pending_audit.callback_deferred_clear_event;
                            natural_anchor.events[6] =
                                pending_audit.world_commit_event;
                            natural_anchor.completion_before =
                                pending_audit.completion_before;
                            natural_anchor.completion_after =
                                pending_audit.completion_after;
                            natural_anchor.callback_flags =
                                pending_audit.callback_flags_at_c9c;
                            natural_anchor.c98_pair_after =
                                pending_audit.c98_pair_after;
                            natural_anchor.c9c_pair_after =
                                pending_audit.c9c_pair_after;
                            std::memcpy(
                                natural_anchor.anchor_transform,
                                pending_audit.transform,
                                sizeof(natural_anchor.anchor_transform));
                            std::memcpy(
                                natural_anchor.anchor_linear,
                                pending_audit.linear,
                                sizeof(natural_anchor.anchor_linear));
                            natural_anchor_captured = true;
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
                            }
#endif
#endif
#ifdef A9TAS_SYNC_ACTION_WINDOW_V1
                            if (accept_warmup) {
                                if (unlink(action_start_path) != 0) {
                                    semantic_fault(
                                        "action_window_trigger_unlink_failed",
                                        tid, dr6);
                                } else {
                                    report.flags |= kSyncHostTriggerObserved;
                                }
                            }
#endif
                            pending_frame = {};
                            pending_audit = {};
                            have_pending_frame = false;
                            have_pending_audit = false;
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
                            warmup_pending = !accept_warmup;
                            machine = Machine{
                                Stage::kWaiting, 0,
                                accept_warmup ? target_frames : 1,
                            };
#else
                            warmup_pending = false;
                            machine = Machine{
                                Stage::kWaiting, 0, target_frames,
                            };
#endif
                            if (!rearm_post_owner_for_input(tid)) {
                                ++report.ptrace_errors;
                            } else {
                                post_phase_tid = 0;
                                cycle_tid = 0;
                            }
                        } else {
#endif
                        frames.push_back(pending_frame);
                        audits.push_back(pending_audit);
#ifdef A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1
                        const bool simultaneous_action =
                            pending_frame.brake <= -0.5f &&
                            std::fabs(pending_frame.steering) >= 0.1f;
                        if (simultaneous_action) action_observed = true;
                        if (action_observed && pending_frame.brake == 0.0f)
                            brake_release_observed = true;
                        if (brake_release_observed) ++post_release_frames;
                        const bool action_window_done =
                            post_release_frames >= kActionPostReleaseFrames;
                        if (action_window_done)
                            report.flags |= kSyncActionReleaseCompleted;
#endif
                        pending_frame = {};
                        pending_audit = {};
                        have_pending_frame = false;
                        have_pending_audit = false;
                        report.captured_frames =
                            static_cast<std::uint32_t>(machine.frame_index);
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
                        if (!a9tas::natural_action_recording_host_v1::CommitFrame(
                                mem, &action_runtime,
                                report.captured_frames)) {
                            semantic_fault("natural_action_sequence_publish_failed",
                                           tid, dr6);
                        }
#endif
                        if (machine.stage == Stage::kComplete
#ifdef A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1
                            || action_window_done
#endif
                        ) {
                            complete = true;
                        } else if (!rearm_post_owner_for_input(tid)) {
                            ++report.ptrace_errors;
                        } else {
                            post_phase_tid = 0;
                            cycle_tid = 0;
                        }
#if defined(A9TAS_PAUSED_ANCHOR_WARMUP_V1) || \
    defined(A9TAS_NATURAL_PREROLL_ANCHOR_V1)
                        }
#endif
                    }
                }
            }
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
            else if (hit == 8UL && machine.stage == Stage::kInputOpen &&
                     tid == cycle_tid) {
                if (completion_write_observed ||
                    pending_audit.reserved[0] != 0) {
                    semantic_fault("duplicate_completion_write", tid, dr6);
                } else {
                    completion_write_observed = true;
                    pending_audit.reserved[0] = 1;
                    if (!ProgramStoppedThread(
                            tid, delta_address, c98_address, c9c_address,
                            world_accumulator, BoundaryDr7(true))) {
                        ++report.ptrace_errors;
                        semantic_fault("completion_watch_restore_failed", tid,
                                       dr6);
                    }
                }
            }
#endif
            else if (hit == 8UL && machine.stage == Stage::kWaiting) {
                // Ignore independent world commits until a frame is ready.
            } else if (tid == post_phase_tid) {
                if (tid != cycle_tid) {
                    semantic_fault("post_phase_wrong_owner", tid, dr6);
                } else if (hit == 1UL) {
                    semantic_fault("unexpected_completion", tid, dr6);
                } else if (hit == 2UL) {
                    std::uint16_t flags_value = 0;
                    if (!ReadExact(mem, callback_flags, &flags_value,
                                   sizeof(flags_value))) {
                        ++report.read_errors;
                    } else {
                        const std::uint8_t dispatching =
                            static_cast<std::uint8_t>(flags_value & 0xffu);
                        std::uint32_t actions = 0;
                        if (dispatching == 0 &&
                            machine.stage == Stage::kWaitCallbackClose) {
                            const std::uint32_t steering_bits =
                                static_cast<std::uint32_t>(
                                    pending_audit.c9c_pair_after >> 32);
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
                            const std::uint32_t brake_bits =
                                static_cast<std::uint32_t>(
                                    pending_audit.c9c_pair_after);
#endif
                            RecordingFrameV1 frame{};
                            if (!ReadSynchronizedFrame(
                                    mem, machine.frame_index, steering_bits,
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
                                    brake_bits,
#endif
                                    vehicle, backend, base, &frame)) {
                                ++report.read_errors;
                            } else if (!Advance(
                                           &machine, Event::kCallbackClose,
                                           true, true, false, &actions)) {
                                semantic_fault("callback_close_rejected", tid,
                                               dr6);
                            } else {
                                pending_audit.tick = frame.tick;
                                pending_audit.monotonic_ns = frame.monotonic_ns;
                                pending_audit.callback_close_event =
                                    report.event_count;
                                std::memcpy(pending_audit.transform,
                                            frame.transform_bits,
                                            sizeof(frame.transform_bits));
                                std::memcpy(pending_audit.linear,
                                            frame.linear_velocity_bits,
                                            sizeof(frame.linear_velocity_bits));
                                pending_frame = frame;
                                have_pending_frame = true;
                                have_pending_audit = true;
                            }
                        } else if (
                            dispatching == 0 &&
                            machine.stage == Stage::kWaitDeferredClear) {
                            if (!have_pending_audit ||
                                pending_audit.callback_deferred_clear_event !=
                                    0 ||
                                !Advance(
                                    &machine, Event::kDeferredCallbackClear,
                                    false, false, false, &actions)) {
                                semantic_fault(
                                    "deferred_callback_clear_rejected", tid,
                                    dr6);
                            } else {
                                pending_audit.callback_deferred_clear_event =
                                    report.event_count;
                            }
                        } else {
                            semantic_fault("unexpected_callback", tid, dr6);
                        }
                    }
                } else if (hit == 4UL) {
                    std::uint32_t bits = 0;
                    float value = 0.0f;
                    std::uint32_t actions = 0;
                    if (!ReadExact(mem, f64, &bits, sizeof(bits))) {
                        ++report.read_errors;
                    } else {
                        std::memcpy(&value, &bits, sizeof(value));
                        if (!std::isfinite(value) ||
                            std::fabs(value) > 1000000.0f ||
                            !Advance(&machine, Event::kF64, false, false,
                                     false, &actions)) {
                            semantic_fault("unexpected_f64", tid, dr6);
                        } else {
                            pending_audit.f64_event = report.event_count;
                        }
                    }
                } else if (hit == 8UL) {
                    semantic_fault("world_commit_before_ready", tid, dr6);
                }
            } else {
                if (hit == 1UL) {
                    std::int64_t delta_us = 0;
                    if (!ReadExact(mem, delta_address, &delta_us,
                                   sizeof(delta_us))) {
                        ++report.read_errors;
#if defined(A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1) && \
    !defined(A9TAS_RACE_LIFECYCLE_SOURCE_V1)
                    } else if (
                        machine.stage == Stage::kWaiting &&
                        input_cycle_anchor_stage !=
                            InputCycleSourceAnchorStage::kReady) {
                        if (delta_us == 0 &&
                            input_cycle_anchor_stage ==
                                InputCycleSourceAnchorStage::kAwaitDeltaZero &&
                            tid == input_cycle_anchor_tid) {
                            input_cycle_anchor_stage =
                                InputCycleSourceAnchorStage::kReady;
                            input_cycle_anchor_tid = 0;
                            report.flags |= kSyncInputCycleAnchorWitnessed;
                            std::fprintf(
                                stderr,
                                "input_cycle_source_anchor_complete event=%" PRIu64
                                " tid=%d skipped_deltas=%" PRIu64 "\n",
                                report.event_count, static_cast<int>(tid),
                                input_cycle_anchor_skipped_deltas);
                        } else if (
                            input_cycle_anchor_stage ==
                                InputCycleSourceAnchorStage::kAwaitC98 &&
                            delta_us >= 0) {
                            if (delta_us > 0)
                                ++input_cycle_anchor_skipped_deltas;
                        } else {
                            semantic_fault("input_cycle_source_anchor_delta_order",
                                           tid, dr6);
                        }
#endif
                    } else if (machine.stage == Stage::kWaiting &&
                               delta_us == 0) {
                        std::uint32_t actions = 0;
                        if (!Advance(&machine, Event::kDeltaZero, false, false,
                                     false, &actions))
                            semantic_fault("waiting_zero_rejected", tid, dr6);
                    } else if (machine.stage == Stage::kWaiting &&
                               delta_us > 0 && delta_us <= 1000000) {
                        std::uint32_t actions = 0;
                        if (!Advance(&machine, Event::kDeltaNonzero, false,
                                     false, false, &actions)) {
                            semantic_fault("delta_open_rejected", tid, dr6);
                        } else {
                            cycle_tid = tid;
                            pending_audit = {};
                            pending_frame = {};
                            have_pending_audit = false;
                            have_pending_frame = false;
                            pending_audit.cycle_tid =
                                static_cast<std::int32_t>(tid);
                            pending_audit.original_delta_us = delta_us;
                            pending_audit.applied_delta_us = fixed_interval_us;
                            pending_audit.delta_event = report.event_count;
                            const std::int64_t fixed_delta = fixed_interval_us;
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
                            completion_write_observed = false;
#endif
                            if (!ReadExact(
                                    mem, completion,
                                    &pending_audit.completion_before,
                                    sizeof(pending_audit.completion_before))) {
                                ++report.read_errors;
                            }
#if defined(A9TAS_PAUSED_ANCHOR_WARMUP_V1) || \
    defined(A9TAS_NATURAL_PREROLL_ANCHOR_V1)
                            else if (warmup_pending) {
                                // Deliberately leave the candidate delta
                                // untouched during the natural warmup cycle.
                            }
#endif
                            else if (!WriteExactVerified(
                                           mem, delta_address, &fixed_delta,
                                           sizeof(fixed_delta))) {
                                semantic_fault("fixed_delta_write_failed", tid,
                                               dr6);
                            } else {
                                ++report.delta_writes;
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
                                if (!ProgramStoppedThread(
                                        tid, delta_address, c98_address,
                                        c9c_address, completion,
                                        CompletionInputDr7())) {
                                    ++report.ptrace_errors;
                                    semantic_fault(
                                        "completion_watch_arm_failed", tid,
                                        dr6);
                                }
#endif
                            }
                        }
                    } else if (machine.stage == Stage::kInputOpen &&
                               tid == cycle_tid && delta_us == 0) {
#if defined(A9TAS_PAUSED_ANCHOR_WARMUP_V1) || \
    defined(A9TAS_NATURAL_PREROLL_ANCHOR_V1)
                        if (warmup_pending) {
                            std::uint32_t actions = 0;
                            if (!Advance(&machine, Event::kDeltaZero, false,
                                         false, false, &actions)) {
                                semantic_fault(
                                    "warmup_cancelled_cycle_rejected", tid,
                                    dr6);
                            } else {
                                pending_audit = {};
                                pending_frame = {};
                                have_pending_audit = false;
                                have_pending_frame = false;
                                cycle_tid = 0;
                            }
                        } else {
#endif
                        semantic_fault("paused_cycle_after_delta_write", tid,
                                       dr6);
#if defined(A9TAS_PAUSED_ANCHOR_WARMUP_V1) || \
    defined(A9TAS_NATURAL_PREROLL_ANCHOR_V1)
                        }
#endif
                    } else {
                        semantic_fault("unexpected_delta", tid, dr6);
                    }
                } else if (hit == 2UL || hit == 4UL) {
#if defined(A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1) && \
    !defined(A9TAS_RACE_LIFECYCLE_SOURCE_V1)
                    if (
                        machine.stage == Stage::kWaiting &&
                        input_cycle_anchor_stage !=
                            InputCycleSourceAnchorStage::kReady) {
                        if (hit == 2UL &&
                            input_cycle_anchor_stage ==
                                InputCycleSourceAnchorStage::kAwaitC98) {
                            input_cycle_anchor_stage =
                                InputCycleSourceAnchorStage::kAwaitC9C;
                            input_cycle_anchor_tid = tid;
                            std::fprintf(
                                stderr,
                                "input_cycle_source_anchor_c98 event=%" PRIu64
                                " tid=%d\n",
                                report.event_count, static_cast<int>(tid));
                        } else if (
                            hit == 4UL &&
                            input_cycle_anchor_stage ==
                                InputCycleSourceAnchorStage::kAwaitC9C &&
                            tid == input_cycle_anchor_tid) {
                            input_cycle_anchor_stage =
                                InputCycleSourceAnchorStage::kAwaitDeltaZero;
                            std::fprintf(
                                stderr,
                                "input_cycle_source_anchor_c9c event=%" PRIu64
                                " tid=%d\n",
                                report.event_count, static_cast<int>(tid));
                        } else if (
                            hit == 4UL &&
                            input_cycle_anchor_stage ==
                                InputCycleSourceAnchorStage::kAwaitC98) {
                            // Initial stopped enrollment can expose only the
                            // tail of a pre-freeze input cycle.
                        } else {
                            semantic_fault("input_cycle_source_anchor_input_order",
                                           tid, dr6);
                        }
                    } else
#endif
                    if (machine.stage == Stage::kWaiting) {
                        // Ignore an incomplete input prefix at initial attach.
                    } else if (tid != cycle_tid) {
                        semantic_fault("input_cross_thread", tid, dr6);
                    } else {
                        std::uint32_t actions = 0;
                        const Event event =
                            hit == 2UL ? Event::kC98 : Event::kC9C;
                        std::uint64_t pair_after = 0;
                        if (!Advance(&machine, event, false, false, false,
                                     &actions)) {
                            semantic_fault("input_order", tid, dr6);
                        } else if (!ReadExact(mem, c98_address, &pair_after,
                                              sizeof(pair_after))) {
                            ++report.read_errors;
                        } else if (event == Event::kC98) {
                            pending_audit.c98_event = report.event_count;
                            pending_audit.c98_pair_after = pair_after;
                        } else {
                            bool discarded_warmup_candidate = false;
                            const std::uint32_t steering_bits =
                                static_cast<std::uint32_t>(pair_after >> 32);
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
                            const std::uint32_t brake_bits =
                                static_cast<std::uint32_t>(pair_after);
                            const std::uint32_t c98_brake_bits =
                                static_cast<std::uint32_t>(
                                    pending_audit.c98_pair_after);
                            float brake = 0.0f;
                            std::memcpy(&brake, &brake_bits, sizeof(brake));
#endif
                            float steering = 0.0f;
                            std::memcpy(&steering, &steering_bits,
                                        sizeof(steering));
                            pending_audit.c9c_pair_after = pair_after;
                            std::uint16_t flags_value = 0;
                            if (!std::isfinite(steering) ||
                                std::fabs(steering) > 1.05f ||
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
                                !std::isfinite(brake) ||
                                std::fabs(brake) > 1.05f ||
#endif
                                !ReadExact(
                                    mem, completion,
                                    &pending_audit.completion_after,
                                    sizeof(pending_audit.completion_after)) ||
                                !ReadExact(mem, callback_flags, &flags_value,
                                           sizeof(flags_value))) {
                                ++report.read_errors;
                            }
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
                            else if (c98_brake_bits != brake_bits) {
                                semantic_fault("brake_pair_changed_within_tick",
                                               tid, dr6);
                            }
#endif
#ifdef A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1
                            else if (
                                warmup_pending &&
                                (pending_audit.completion_after ==
                                     pending_audit.completion_before ||
                                 (flags_value & 0xffu) != 1u)) {
                                // A long zero-write arm can observe a complete
                                // input-layout prefix that is not a physics
                                // dispatch. Discard it exactly like natural
                                // replay anchor search; no delta/control/final
                                // write has occurred while warmup_pending.
                                ++warmup_rejected_candidates;
                                pending_audit = {};
                                pending_frame = {};
                                have_pending_audit = false;
                                have_pending_frame = false;
                                machine = Machine{Stage::kWaiting, 0, 1};
                                cycle_tid = 0;
                                post_phase_tid = 0;
                                discarded_warmup_candidate = true;
                            }
#endif
                            else if (
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
                                !completion_write_observed ||
                                pending_audit.reserved[0] != 1
#else
                                pending_audit.completion_after ==
                                    pending_audit.completion_before
#endif
                            ) {
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
                                semantic_fault("completion_write_not_observed",
                                               tid, dr6);
#else
                                semantic_fault("completion_unchanged_at_c9c",
                                               tid, dr6);
#endif
                            } else if ((flags_value & 0xffu) != 1u) {
                                semantic_fault("callback_not_open_at_c9c", tid,
                                               dr6);
                            } else {
                                pending_audit.c9c_prefix_event =
                                    report.event_count;
                                pending_audit.callback_flags_at_c9c =
                                    flags_value;
                                std::uint32_t prefix_actions = 0;
                                if (!Advance(
                                        &machine, Event::kPrefixCertified,
                                        false, false, false,
                                        &prefix_actions)) {
                                    semantic_fault(
                                        "prefix_certificate_rejected", tid,
                                        dr6);
                                }
                            }
                            if (discarded_warmup_candidate) {
                                // The common trap tail resumes the input owner.
                            } else if (report.read_errors == 0 &&
                                report.semantic_errors == 0 &&
                                !ProgramStoppedThread(
                                    tid, completion, callback_flags, f64,
                                    world_accumulator, PipelineDr7())) {
                                ++report.ptrace_errors;
                            } else if (report.read_errors == 0 &&
                                       report.semantic_errors == 0) {
                                post_phase_tid = tid;
                            }
                        }
                    }
                } else {
                    semantic_fault("unexpected_input_dr", tid, dr6);
                }
            }

            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
                ++report.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        } else {
            ++report.unexpected_stops;
            const bool fatal_signal =
                signal == SIGSEGV || signal == SIGBUS ||
                signal == SIGILL || signal == SIGABRT ||
                signal == SIGFPE;
            if (fatal_signal) {
                siginfo_t info{};
                const bool have_siginfo =
                    ptrace(PTRACE_GETSIGINFO, tid, nullptr, &info) == 0;
                std::fprintf(
                    stderr,
                    "sync_recorder_target_fatal signal=%d tid=%d "
                    "fault_address=0x%" PRIxPTR " siginfo=%u\n",
                    signal, static_cast<int>(tid),
                    have_siginfo
                        ? reinterpret_cast<std::uintptr_t>(info.si_addr)
                        : static_cast<std::uintptr_t>(0),
                    have_siginfo ? 1u : 0u);
                // Keep the faulting thread stopped.  The common frozen cleanup
                // can now read the passive-action evidence and restore the
                // original service vptr before detaching; forwarding the fatal
                // signal here would destroy that diagnostic window.
                semantic_fault("fatal_target_signal", tid,
                               static_cast<unsigned long>(signal));
                continue;
            }
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver))
                ++report.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        }
    }

#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
    a9tas::natural_action_recording_host_v1::Result action_result{};
    std::uint64_t action_freeze_failures = 0;
    std::uint32_t action_freeze_passes = 0;
    const std::size_t action_threads_before_freeze = threads.size();
    const bool action_freeze_stable = FreezeRaceLifecycleSourceThreads(
        pid, lifecycle_state, &threads, &action_freeze_failures,
        &action_freeze_passes);
    if (threads.size() > action_threads_before_freeze)
        report.thread_additions +=
            threads.size() - action_threads_before_freeze;
    report.ptrace_errors += action_freeze_failures;
    const bool action_finish_ok = action_freeze_stable &&
        a9tas::natural_action_recording_host_v1::Finish(
            mem, &action_runtime, &action_result);
    report.action_wrapper_entries = action_result.evidence.wrapper_entries;
    report.action_original_calls = action_result.evidence.original_calls;
    report.action_clean_returns = action_result.evidence.clean_returns;
    report.action_counted_calls = action_result.evidence.counted_calls;
    report.action_out_of_window_calls =
        action_result.evidence.out_of_window_calls;
    report.action_overflow_calls = action_result.evidence.overflow_calls;
    report.action_failures = action_result.evidence.failures;
    report.action_count_sum = action_result.count_sum;
    report.action_cleanup_freeze_passes = action_freeze_passes;
    report.action_last_status = action_result.evidence.last_status;
    report.action_last_tid = action_result.evidence.last_tid;
    if (!action_finish_ok || action_result.counts.size() != frames.size()) {
        ++report.semantic_errors;
    } else {
        for (std::size_t index = 0; index < frames.size(); ++index) {
            frames[index].nitro_activation_count = action_result.counts[index];
            frames[index].skip_override_flags &=
                ~a9tas::unified_tick_v1::kSkipNitroActivation;
        }
        report.flags |= kSyncNitroCaptured;
    }
    std::printf(
        "NATURAL_ACTION_RECORDING_DONE finish=%u restored=%u exact=%u "
        "calls=%" PRIu64 " count_sum=%" PRIu64
        " freeze_passes=%u freeze_failures=%" PRIu64 "\n",
        action_finish_ok ? 1u : 0u,
        action_result.service_restored ? 1u : 0u,
        action_result.evidence_exact ? 1u : 0u,
        action_result.evidence.wrapper_entries, action_result.count_sum,
        action_freeze_passes, action_freeze_failures);
    std::fflush(stdout);
#endif

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
        report.flags |= kSyncCleanDetach;
#ifdef A9TAS_SYNC_ACTION_WINDOW_V1
    (void)unlink(action_start_path);
#endif
    close(mem);

    const std::uint32_t required_flags =
        kSyncTargetVerified | kSyncIdentityVerified | kSyncFixedDeltaOnly |
        kSyncSteeringCaptured | kSyncPhysicsCaptured | kSyncCleanDetach
#ifdef A9TAS_SYNC_BRAKE_CAPTURE_V1
        | kSyncBrakeCaptured
#endif
#ifdef A9TAS_SYNC_ACTION_WINDOW_V1
        | kSyncHostTriggerObserved
#endif
#ifdef A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1
        | kSyncActionReleaseCompleted
#endif
#ifdef A9TAS_RACE_LIFECYCLE_SOURCE_V1
        | kSyncRaceLifecycleWitnessed
#elif defined(A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1)
        | kSyncInputCycleAnchorWitnessed
#endif
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
        | kSyncNitroCaptured
#endif
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
        | kSyncCompletionWriteCertificate
#endif
        ;
#ifdef A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1
    bool success =
        complete && report.flags == required_flags &&
        report.captured_frames >= kActionPostReleaseFrames &&
        report.captured_frames <= report.target_frames &&
        frames.size() == report.captured_frames &&
        audits.size() == report.captured_frames &&
        report.delta_writes == report.captured_frames &&
        report.read_errors == 0 && report.ptrace_errors == 0 &&
        report.semantic_errors == 0 && report.unexpected_stops == 0;
#else
    bool success =
        complete && report.flags == required_flags &&
        report.captured_frames == report.target_frames &&
        frames.size() == target_frames && audits.size() == target_frames &&
        report.delta_writes == target_frames && report.read_errors == 0 &&
        report.ptrace_errors == 0 && report.semantic_errors == 0 &&
        report.unexpected_stops == 0;
#endif
#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
    success = success && natural_anchor_captured;
#endif
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
    // A natural-action source must contain at least one real activation; a
    // zero-action source remains valid for the older lifecycle recorder.
    success = success && action_result.count_sum > 0;
#endif
    bool recording_ok = false;
    bool report_ok = false;
#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
    bool anchor_ok = false;
#endif
    if (success) {
#ifdef A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1
        natural_anchor.frame_count = report.captured_frames;
#endif
        recording_ok = WriteSynchronizedRecording(
            argv[6], fixed_interval_us, frames);
        if (recording_ok) report_ok = WriteSyncReport(argv[7], report, audits);
#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
        if (recording_ok && report_ok) {
            std::memcpy(natural_anchor.frame0_transform,
                        frames.front().transform_bits,
                        sizeof(natural_anchor.frame0_transform));
            std::memcpy(natural_anchor.frame0_linear,
                        frames.front().linear_velocity_bits,
                        sizeof(natural_anchor.frame0_linear));
            anchor_ok = WriteNaturalPrerollAnchor(argv[8], natural_anchor);
        }
#endif
    }
    if (!recording_ok || !report_ok
#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
        || !anchor_ok
#endif
    ) {
        std::remove(argv[6]);
        std::remove(argv[7]);
#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
        std::remove(argv[8]);
#endif
    }
#ifdef A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1
    std::printf(
        "SYNC_ACTION_UNTIL_RELEASE_DIAGNOSTIC action=%u release=%u "
        "post_release_frames=%u warmup_rejected=%" PRIu64 " cap=%zu\n",
        action_observed ? 1u : 0u, brake_release_observed ? 1u : 0u,
        post_release_frames, warmup_rejected_candidates, target_frames);
#endif
    std::printf(
#ifdef A9TAS_NATURAL_ACTION_RECORDING_V1
        "SYNCHRONIZED_TICK_RECORDER_A9USR6_DONE complete=%u "
#elif defined(A9TAS_RACE_LIFECYCLE_SOURCE_V1)
        "SYNCHRONIZED_TICK_RECORDER_A9USR5_DONE complete=%u "
#elif defined(A9TAS_SYNC_ACTION_UNTIL_RELEASE_V1)
        "SYNCHRONIZED_TICK_RECORDER_A9USR4_DONE complete=%u "
#elif defined(A9TAS_SYNC_ACTION_WINDOW_V1)
        "SYNCHRONIZED_TICK_RECORDER_A9USR3_DONE complete=%u "
#elif defined(A9TAS_SYNC_BRAKE_CAPTURE_V1)
        "SYNCHRONIZED_TICK_RECORDER_A9USR2_DONE complete=%u "
#else
        "SYNCHRONIZED_TICK_RECORDER_A9USR1_DONE complete=%u "
#endif
        "captured=%u/%u delta_writes=%" PRIu64
        " read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
        " semantic_errors=%" PRIu64 " unexpected_stops=%" PRIu64
        " clean_detach=%u recording=%s report=%s\n",
        complete ? 1u : 0u, report.captured_frames, report.target_frames,
        report.delta_writes, report.read_errors, report.ptrace_errors,
        report.semantic_errors, report.unexpected_stops,
        (report.flags & kSyncCleanDetach) != 0, argv[6], argv[7]);
    return success && recording_ok && report_ok
#ifdef A9TAS_NATURAL_PREROLL_ANCHOR_V1
                   && anchor_ok
#endif
               ? 0
               : 8;
}
