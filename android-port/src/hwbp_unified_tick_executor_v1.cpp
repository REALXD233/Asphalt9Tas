// BUILD-ONLY unified A9UTK1 tick executor.
//
// This translation unit composes the live-proven scheduler HWBP layer, full
// four-address Gate 2, and 804-byte conditional-correction audit. No runner is
// provided. Runtime use requires a separate explicit authorization.
//
// V1 runtime capability is intentionally narrow: fixed delta, optional steer,
// and final transform+linear correction. Brake/accelerator, nitro, respawn,
// Barrel angular and Barrel RBX must all be skipped in every input frame.

#define A9TAS_SAME_BYTES_EXECUTOR_NO_MAIN
#include "hwbp_same_bytes_executor_v1.cpp"

#define A9TAS_UNIFIED_TICK_CORE_NO_MAIN
#include "unified_tick_executor_core_v1.cpp"

#if defined(A9TAS_UNIFIED_NITRO_OBSERVE_GATE_V1) && \
    !defined(A9TAS_UNIFIED_NITRO_RPC_V1)
#error "nitro observe gate requires A9TAS_UNIFIED_NITRO_RPC_V1"
#endif

#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
#include "unified_nitro_rpc_client_v1.h"
#endif

#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
#include "natural_preroll_anchor_v1.h"
#endif

#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
#include "final_writer_unified_integration_v1.h"
#endif

namespace {

#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
constexpr char kUnifiedReportMagic[8] = {
    'A', '9', 'U', 'E', 'R', '8', '\0', '\0',
};
constexpr std::uint32_t kUnifiedReportVersion = 8;
#elif defined(A9TAS_UNIFIED_NITRO_RPC_V1)
constexpr char kUnifiedReportMagic[8] = {
    'A', '9', 'U', 'E', 'R', '7', '\0', '\0',
};
constexpr std::uint32_t kUnifiedReportVersion = 7;
#elif defined(A9TAS_UNIFIED_BRAKE_V1)
constexpr char kUnifiedReportMagic[8] = {
    'A', '9', 'U', 'E', 'R', '6', '\0', '\0',
};
constexpr std::uint32_t kUnifiedReportVersion = 6;
#else
constexpr char kUnifiedReportMagic[8] = {
    'A', '9', 'U', 'E', 'R', '5', '\0', '\0',
};
constexpr std::uint32_t kUnifiedReportVersion = 5;
#endif
constexpr char kUnifiedAcknowledgement[] =
    "I_ACCEPT_UNIFIED_TICK_EXECUTOR_V1";

constexpr std::uint32_t kRequiredRuntimeSkipMask =
#ifndef A9TAS_UNIFIED_BRAKE_V1
    a9tas::unified_tick_v1::kSkipBrake |
#endif
#if !defined(A9TAS_UNIFIED_NITRO_RPC_V1) && \
    !defined(A9TAS_FINAL_WRITER_NATURAL_ACTION_V1)
    a9tas::unified_tick_v1::kSkipNitroActivation |
#endif
    a9tas::unified_tick_v1::kSkipAccelerator |
    a9tas::unified_tick_v1::kSkipBarrelAngular |
    a9tas::unified_tick_v1::kSkipBarrelRbx |
    a9tas::unified_tick_v1::kSkipRespawnButton;

enum UnifiedReportFlag : std::uint32_t {
    kUnifiedTargetVerified = 1u << 0,
    kUnifiedIdentityVerified = 1u << 1,
    kUnifiedRecordingValidated = 1u << 2,
    kUnifiedCapabilityValidated = 1u << 3,
    kUnifiedCleanDetach = 1u << 4,
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
    kUnifiedNitroRpcUsed = 1u << 5,
#endif
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
    kUnifiedCompletionWriteCertificate = 1u << 6,
#endif
};

enum UnifiedFrameAuditFlag : std::uint32_t {
    kUnifiedSteeringApplied = 1u << 0,
    kUnifiedCorrectionEqual = 1u << 1,
    kUnifiedCorrectionCorrected = 1u << 2,
    kUnifiedCorrectionSkipped = 1u << 3,
    kUnifiedAuditExact = 1u << 4,
    kUnifiedGate2Complete = 1u << 5,
    kUnifiedCommitted = 1u << 6,
    kUnifiedPrefixCertified = 1u << 7,
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
    kUnifiedNaturalAnchorMatched = 1u << 8,
#endif
#ifdef A9TAS_UNIFIED_BRAKE_V1
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
    kUnifiedBrakeApplied = 1u << 9,
#else
    kUnifiedBrakeApplied = 1u << 8,
#endif
#endif
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
    kUnifiedNitroRpcObserved = 1u << 10,
    kUnifiedNitroActivated = 1u << 11,
#endif
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
    kUnifiedCompletionWriteObserved = 1u << 12,
#endif
};

#pragma pack(push, 1)
struct UnifiedReportHeaderV5 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t frame_audit_size;
    std::uint32_t flags;
    std::uint32_t recording_frames;
    std::uint32_t processed_frames;
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
    std::uint64_t control_writes;
    std::uint64_t equal_frames;
    std::uint64_t corrected_frames;
    std::uint64_t skipped_frames;
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
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
    std::uint64_t nitro_rpc_requests;
    std::uint64_t nitro_activation_calls;
    std::uint64_t nitro_rpc_failures;
#endif
};

struct UnifiedFrameAuditV5 {
    std::uint64_t recording_tick;
    std::uint64_t recording_monotonic_ns;
    std::int32_t cycle_tid;
    std::uint32_t flags;
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
    std::int32_t commit_tid;
    std::uint8_t reserved[2];
    std::uint32_t steering_bits;
    std::uint8_t reserved2[4];
    std::uint64_t c98_pair_after;
    std::uint64_t c9c_pair_after;
    std::uint8_t recorded_transform[64];
    std::uint8_t recorded_linear[12];
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
    std::uint64_t nitro_phase_event;
    std::uint32_t nitro_requested_activations;
    std::uint32_t nitro_reserved;
    a9tas::nitro_rpc_v1::Response nitro_response;
#endif
    AuditSnapshotV1 before;
    AuditSnapshotV1 immediate;
};

#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
struct NaturalSearchReportHeaderV1 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t audit_size;
    std::uint32_t flags;
    std::uint32_t search_cycles;
    std::uint32_t matched_cycle_index;
    std::uint8_t build_id[20];
    std::uint32_t reserved0;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t main_object;
    std::uint64_t final_owner;
    std::uint64_t physics_context;
    std::uint64_t native_body;
    std::uint64_t event_count;
    std::uint64_t thread_additions;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
    std::uint32_t fixed_interval_us;
    std::uint32_t frame_count;
    std::uint8_t source_recording_sha256[32];
    std::uint8_t source_report_sha256[32];
};
#endif
#pragma pack(pop)

#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
static_assert(sizeof(UnifiedReportHeaderV5) == 320,
              "unified nitro report header ABI");
static_assert(sizeof(UnifiedFrameAuditV5) == 1940,
              "unified nitro frame audit ABI");
#else
static_assert(sizeof(UnifiedReportHeaderV5) == 296,
              "unified report header ABI");
static_assert(sizeof(UnifiedFrameAuditV5) == 1828,
              "unified frame audit ABI");
#endif
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
static_assert(sizeof(NaturalSearchReportHeaderV1) == 200,
              "A9NPR1 search report header ABI");
constexpr char kNaturalSearchReportMagic[8] = {
    'A', '9', 'N', 'P', 'R', '1', '\0', '\0',
};
enum NaturalSearchReportFlag : std::uint32_t {
    kNaturalSearchTargetVerified = 1u << 0,
    kNaturalSearchIdentityVerified = 1u << 1,
    kNaturalSearchAnchorValidated = 1u << 2,
    kNaturalSearchZeroGameplayWrites = 1u << 3,
    kNaturalSearchMatched = 1u << 4,
    kNaturalSearchCleanDetach = 1u << 5,
};
#endif

bool RuntimeCapabilitiesSupported(
    const std::vector<RecordingFrameV1>& frames, std::size_t* bad_index) {
    for (std::size_t index = 0; index < frames.size(); ++index) {
        if ((frames[index].skip_override_flags & kRequiredRuntimeSkipMask) !=
            kRequiredRuntimeSkipMask) {
            if (bad_index) *bad_index = index;
            return false;
        }
    }
    return true;
}

bool ProgramStoppedThread(pid_t tid, std::uintptr_t dr0,
                          std::uintptr_t dr1, std::uintptr_t dr2,
                          std::uintptr_t dr3, unsigned long dr7) {
    // Disable the old watch set before changing addresses. The thread is
    // already stopped at the event which owns this phase transition.
    return PokeDebug(tid, 7, 0) && PokeDebug(tid, 0, dr0) &&
           PokeDebug(tid, 1, dr1) && PokeDebug(tid, 2, dr2) &&
           PokeDebug(tid, 3, dr3) && PokeDebug(tid, 6, 0) &&
           PokeDebug(tid, 7, dr7);
}

#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
unsigned long CompletionInputDr7() {
    // Once a positive delta selects a real cycle, its world commit cannot
    // precede C9C. Reuse DR3 on the cycle owner for the 8-byte completion
    // store itself. This certifies legal same-value token reuse by observing
    // the write event instead of assuming that token values are unique.
    return (BoundaryDr7(true) & ~(3UL << 30)) | (2UL << 30);
}
#endif

#ifdef A9TAS_RACE_LIFECYCLE_START_V1
unsigned long RaceLifecycleDr7() {
    // Local DR0, write-only, four-byte length.
    return 1UL | (1UL << 16) | (3UL << 18);
}

bool ProgramAllStoppedThreads(
    std::vector<TracedThread>* threads, std::uintptr_t dr0,
    std::uintptr_t dr1, std::uintptr_t dr2, std::uintptr_t dr3,
    unsigned long dr7) {
    if (threads == nullptr || threads->empty()) return false;
    for (auto& thread : *threads) {
        if (!thread.live) continue;
        if (!thread.stopped ||
            !ProgramStoppedThread(thread.tid, dr0, dr1, dr2, dr3, dr7))
            return false;
    }
    return true;
}

bool FreezeStableLifecycleThreadSet(
    pid_t pid, std::uintptr_t state_address,
    std::vector<TracedThread>* threads, std::uint64_t* additions,
    std::uint64_t* ptrace_errors) {
    if (threads == nullptr || additions == nullptr || ptrace_errors == nullptr)
        return false;
    for (int pass = 0; pass < 6; ++pass) {
        for (auto& thread : *threads) {
            if (!thread.live || thread.stopped) continue;
            if (!StopThread(thread.tid)) {
                ++*ptrace_errors;
                return false;
            }
            thread.stopped = true;
        }
        std::uint64_t failures = 0;
        const std::size_t added = AttachNewThreadsStopped(
            pid, state_address, 0, 0, 0, threads, &failures,
            RaceLifecycleDr7());
        *additions += added;
        *ptrace_errors += failures;
        if (failures != 0) return false;
        if (added == 0) return !threads->empty();
    }
    return false;
}

bool ContinueAllStoppedThreads(std::vector<TracedThread>* threads,
                               std::uint64_t* ptrace_errors) {
    if (threads == nullptr || ptrace_errors == nullptr) return false;
    bool ok = true;
    for (auto& thread : *threads) {
        if (!thread.live || !thread.stopped) continue;
        if (!ContinueThread(thread.tid)) {
            ++*ptrace_errors;
            ok = false;
        } else {
            thread.stopped = false;
        }
    }
    return ok;
}

bool WaitForAuthoritativeRaceStart(
    pid_t pid, int mem, std::uint64_t timeout_ms,
    std::uintptr_t state_address, std::uintptr_t delta_address,
    std::uintptr_t c98_address, std::uintptr_t c9c_address,
    std::uintptr_t world_accumulator, std::vector<TracedThread>* threads,
    UnifiedReportHeaderV5* report, std::uint32_t* retired_threads,
    const char** failure_reason, std::uint64_t* observed_lifecycle_events) {
    if (failure_reason != nullptr) *failure_reason = "invalid_arguments";
    if (observed_lifecycle_events != nullptr) *observed_lifecycle_events = 0;
    if (threads == nullptr || report == nullptr || retired_threads == nullptr ||
        failure_reason == nullptr || observed_lifecycle_events == nullptr ||
        state_address == 0)
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
                RaceLifecycleDr7());
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
            if (tracked != nullptr && tracked->live) {
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
        if (!WIFSTOPPED(status)) continue;
        if (tracked == nullptr) {
            ++report->semantic_errors;
            return fail("untracked_stop");
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
            if (!FreezeStableLifecycleThreadSet(
                    pid, state_address, threads, &report->thread_additions,
                    &report->ptrace_errors) ||
                !ProgramAllStoppedThreads(
                    threads, delta_address, c98_address, c9c_address,
                    world_accumulator, BoundaryDr7(true)) ||
                !ContinueAllStoppedThreads(threads, &report->ptrace_errors)) {
                ++report->semantic_errors;
                return fail("tick_watchpoint_handoff_failed");
            }
            std::printf(
                "AUTHORITATIVE_RACE_START_V1 state=3 events=%" PRIu64
                " event_tid=%d event_ns=%" PRIu64
                " tick_watchpoints_armed_before_continue=1\n",
                lifecycle_events, static_cast<int>(tid),
                MonotonicNs() - start_ns);
            std::fflush(stdout);
            *failure_reason = "none";
            *observed_lifecycle_events = lifecycle_events;
            return true;
        }
        ++report->semantic_errors;
        const int deliver = signal == SIGTRAP ? 0 : signal;
        if (!ContinueThread(tid, deliver))
            ++report->ptrace_errors;
        else
            tracked->stopped = false;
        return fail("unexpected_stop");
    }
    ++report->semantic_errors;
    if (report->read_errors != 0) return fail("read_error");
    if (report->ptrace_errors != 0) return fail("ptrace_error");
    return fail("lifecycle_timeout");
}
#endif

bool ComponentPayloadEqualUnified(const AuditSnapshotV1& current,
                                  const RecordingFrameV1& recorded) {
    float current_values[19]{};
    float recorded_values[19]{};
    std::memcpy(current_values,
                current.native_body +
                    a9tas::unified_tick_v1::kTransformOffset,
                a9tas::unified_tick_v1::kTransformSize);
    std::memcpy(current_values + 16,
                current.native_body +
                    a9tas::unified_tick_v1::kLinearVelocityOffset,
                a9tas::unified_tick_v1::kLinearVelocitySize);
    std::memcpy(recorded_values, recorded.transform_bits,
                sizeof(recorded.transform_bits));
    std::memcpy(recorded_values + 16, recorded.linear_velocity_bits,
                sizeof(recorded.linear_velocity_bits));
    for (std::size_t index = 0; index < 19; ++index)
        if (current_values[index] != recorded_values[index]) return false;
    return true;
}

#ifdef A9TAS_REPLAY_ALIGNMENT_GUARD_V1
bool FirstFrameAlignedUnified(
    const AuditSnapshotV1& current,
    const std::vector<RecordingFrameV1>& frames) {
    // This is a live-gate safety guard, not a replacement for the original
    // final correction. It rejects a replay whose first post-physics state is
    // implausibly far from the recorded first state before any 64+12 write.
    // Per-component limits are derived from the recording's first natural
    // transition, with small absolute floors for stationary components.
    if (frames.size() < 2) return false;
    float current_values[19]{};
    float first_values[19]{};
    float second_values[19]{};
    std::memcpy(current_values,
                current.native_body +
                    a9tas::unified_tick_v1::kTransformOffset,
                a9tas::unified_tick_v1::kTransformSize);
    std::memcpy(current_values + 16,
                current.native_body +
                    a9tas::unified_tick_v1::kLinearVelocityOffset,
                a9tas::unified_tick_v1::kLinearVelocitySize);
    std::memcpy(first_values, frames[0].transform_bits,
                sizeof(frames[0].transform_bits));
    std::memcpy(first_values + 16, frames[0].linear_velocity_bits,
                sizeof(frames[0].linear_velocity_bits));
    std::memcpy(second_values, frames[1].transform_bits,
                sizeof(frames[1].transform_bits));
    std::memcpy(second_values + 16, frames[1].linear_velocity_bits,
                sizeof(frames[1].linear_velocity_bits));
    for (std::size_t index = 0; index < 19; ++index) {
        const float step =
            std::fabs(second_values[index] - first_values[index]);
        const float floor = index < 16 ? 0.05f : 2.0f;
        const float tolerance = std::max(floor, step * 8.0f + 0.001f);
        if (!std::isfinite(current_values[index]) ||
            std::fabs(current_values[index] - first_values[index]) >
                tolerance)
            return false;
    }
    return true;
}
#endif

#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
bool LoadNaturalPrerollAnchor(
    const char* path, const RecordingHeaderV1& recording,
    const std::vector<RecordingFrameV1>& frames,
    a9tas::natural_preroll_v1::AnchorV1* output) {
    if (!path || !output || frames.empty()) return false;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    a9tas::natural_preroll_v1::AnchorV1 anchor{};
    const ssize_t count = read(fd, &anchor, sizeof(anchor));
    std::uint8_t extra = 0;
    const ssize_t tail = read(fd, &extra, 1);
    const bool close_ok = close(fd) == 0;
    const std::uint8_t zero4[4]{};
    const std::uint8_t zero6[6]{};
    if (count != static_cast<ssize_t>(sizeof(anchor)) || tail != 0 ||
        !close_ok ||
        std::memcmp(anchor.magic, a9tas::natural_preroll_v1::kMagic,
                    sizeof(anchor.magic)) != 0 ||
        anchor.version != a9tas::natural_preroll_v1::kVersion ||
        anchor.size != sizeof(anchor) ||
        anchor.flags != a9tas::natural_preroll_v1::kBoundFlags ||
        anchor.fixed_interval_us != recording.fixed_interval_us ||
        anchor.frame_count != frames.size() || anchor.reserved0 != 0 ||
        std::memcmp(anchor.build_id,
                    a9tas::unified_tick_v1::kSupportedBuildId,
                    sizeof(anchor.build_id)) != 0 ||
        std::memcmp(anchor.reserved1, zero4, sizeof(zero4)) != 0 ||
        std::memcmp(anchor.reserved2, zero6, sizeof(zero6)) != 0 ||
        anchor.source_pid == 0 || anchor.source_library_base == 0 ||
        anchor.source_main_object == 0 || anchor.source_final_owner == 0 ||
        anchor.source_physics_context == 0 || anchor.source_native_body == 0 ||
        anchor.cycle_tid <= 0 || anchor.commit_tid <= 0 ||
        anchor.cycle_tid == anchor.commit_tid ||
        anchor.completion_before == anchor.completion_after ||
        (anchor.callback_flags & 0xffu) != 1u ||
        std::memcmp(anchor.frame0_transform, frames.front().transform_bits,
                    sizeof(anchor.frame0_transform)) != 0 ||
        std::memcmp(anchor.frame0_linear,
                    frames.front().linear_velocity_bits,
                    sizeof(anchor.frame0_linear)) != 0)
        return false;
    bool recording_hash_nonzero = false;
    bool report_hash_nonzero = false;
    for (std::size_t index = 0; index < 32; ++index) {
        recording_hash_nonzero |= anchor.recording_sha256[index] != 0;
        report_hash_nonzero |= anchor.report_sha256[index] != 0;
    }
    for (std::size_t index = 1; index < 7; ++index)
        if (anchor.events[index - 1] >= anchor.events[index]) return false;
    float values[38]{};
    std::memcpy(values, anchor.anchor_transform,
                sizeof(anchor.anchor_transform));
    std::memcpy(values + 16, anchor.anchor_linear,
                sizeof(anchor.anchor_linear));
    std::memcpy(values + 19, anchor.frame0_transform,
                sizeof(anchor.frame0_transform));
    std::memcpy(values + 35, anchor.frame0_linear,
                sizeof(anchor.frame0_linear));
    if (!recording_hash_nonzero || !report_hash_nonzero ||
        !FiniteBounded(values, 38, 1000000.0f))
        return false;
    *output = anchor;
    return true;
}

bool NaturalPrerollAnchorMatches(
    const AuditSnapshotV1& current,
    const a9tas::natural_preroll_v1::AnchorV1& anchor) {
    float current_values[19]{};
    float anchor_values[19]{};
    float frame0_values[19]{};
    std::memcpy(current_values,
                current.native_body +
                    a9tas::unified_tick_v1::kTransformOffset,
                a9tas::unified_tick_v1::kTransformSize);
    std::memcpy(current_values + 16,
                current.native_body +
                    a9tas::unified_tick_v1::kLinearVelocityOffset,
                a9tas::unified_tick_v1::kLinearVelocitySize);
    std::memcpy(anchor_values, anchor.anchor_transform,
                sizeof(anchor.anchor_transform));
    std::memcpy(anchor_values + 16, anchor.anchor_linear,
                sizeof(anchor.anchor_linear));
    std::memcpy(frame0_values, anchor.frame0_transform,
                sizeof(anchor.frame0_transform));
    std::memcpy(frame0_values + 16, anchor.frame0_linear,
                sizeof(anchor.frame0_linear));
    for (std::size_t index = 0; index < 19; ++index) {
        const float natural_step =
            std::fabs(frame0_values[index] - anchor_values[index]);
        const float floor = index < 16 ? 0.01f : 0.5f;
        const float tolerance =
            std::max(floor, natural_step * 2.0f + 0.001f);
        if (!std::isfinite(current_values[index]) ||
            std::fabs(current_values[index] - anchor_values[index]) >
                tolerance)
            return false;
    }
    return true;
}
#endif

AuditSnapshotV1 ExpectedImmediateUnified(
    const AuditSnapshotV1& before, const RecordingFrameV1& recorded,
    bool corrected) {
    AuditSnapshotV1 expected = before;
    if (corrected) {
        std::memcpy(expected.native_body +
                        a9tas::unified_tick_v1::kTransformOffset,
                    recorded.transform_bits,
                    sizeof(recorded.transform_bits));
        std::memcpy(expected.native_body +
                        a9tas::unified_tick_v1::kLinearVelocityOffset,
                    recorded.linear_velocity_bits,
                    sizeof(recorded.linear_velocity_bits));
    }
    return expected;
}

bool RollbackUnified(int mem,
                     const a9tas::vehicle_state_v1::Layout& vehicle,
                     const a9tas::vehicle_state_v1::BackendLayout& backend,
                     std::uintptr_t base, const AuditSnapshotV1& before) {
    const bool pose_ok = WriteExactVerified(
        mem, backend.native_pose_address,
        before.native_body + a9tas::unified_tick_v1::kTransformOffset,
        a9tas::unified_tick_v1::kTransformSize);
    const bool linear_ok = WriteExactVerified(
        mem, backend.native_linear_address,
        before.native_body + a9tas::unified_tick_v1::kLinearVelocityOffset,
        a9tas::unified_tick_v1::kLinearVelocitySize);
    AuditSnapshotV1 restored{};
    return pose_ok && linear_ok &&
           ReadAuditSnapshot(mem, vehicle, backend, base, &restored) &&
           std::memcmp(&restored, &before, sizeof(before)) == 0;
}

bool ApplySteering(int mem, std::uintptr_t c98_address,
                   const RecordingFrameV1& frame, std::uint64_t* writes,
                   std::uint64_t* pair_after) {
    if (!writes || !pair_after) return false;
    *pair_after = 0;
#ifndef A9TAS_UNIFIED_BRAKE_V1
    if (frame.skip_override_flags & a9tas::unified_tick_v1::kSkipSteer)
        return true;
    std::uint64_t pair = 0;
    if (!ReadExact(mem, c98_address, &pair, sizeof(pair))) return false;
    std::uint32_t steering_bits = 0;
    std::memcpy(&steering_bits, &frame.steering, sizeof(steering_bits));
    pair = (pair & 0xffffffffULL) |
           (static_cast<std::uint64_t>(steering_bits) << 32);
#else
    if ((frame.skip_override_flags & a9tas::unified_tick_v1::kSkipSteer)
        && (frame.skip_override_flags & a9tas::unified_tick_v1::kSkipBrake)
    )
        return true;
    std::uint64_t pair = 0;
    if (!ReadExact(mem, c98_address, &pair, sizeof(pair))) return false;
    std::uint32_t steering_bits = 0;
    std::memcpy(&steering_bits, &frame.steering, sizeof(steering_bits));
    if (!(frame.skip_override_flags & a9tas::unified_tick_v1::kSkipSteer))
        pair = (pair & 0xffffffffULL) |
               (static_cast<std::uint64_t>(steering_bits) << 32);
    std::uint32_t brake_bits = 0;
    std::memcpy(&brake_bits, &frame.brake, sizeof(brake_bits));
    if (!(frame.skip_override_flags & a9tas::unified_tick_v1::kSkipBrake))
        pair = (pair & 0xffffffff00000000ULL) | brake_bits;
#endif
    if (!WriteExactVerified(mem, c98_address, &pair, sizeof(pair)))
        return false;
    ++*writes;
    *pair_after = pair;
    return true;
}

bool WriteUnifiedReport(const char* path,
                        const UnifiedReportHeaderV5& header,
                        const std::vector<UnifiedFrameAuditV5>& audits) {
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

#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
bool WriteNaturalSearchReport(
    const char* path, const NaturalSearchReportHeaderV1& header,
    const std::vector<UnifiedFrameAuditV5>& audits) {
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
#endif

}  // namespace

#ifndef A9TAS_UNIFIED_TICK_EXECUTOR_NO_MAIN
#ifndef A9TAS_UNIFIED_TICK_EXECUTOR_MAIN_NAME
#define A9TAS_UNIFIED_TICK_EXECUTOR_MAIN_NAME main
#endif
int A9TAS_UNIFIED_TICK_EXECUTOR_MAIN_NAME(int argc, char** argv) {
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
    if (argc < 9 || argc > 12) {
        std::fprintf(
            stderr,
            "usage: %s PID LIB_BASE_HEX TIMEOUT_MS A9UTK1_PATH "
            "A9NPA1_PATH REPORT_PATH SEARCH_REPORT_PATH ACKNOWLEDGEMENT "
            "[PHYSICS_CONTEXT_HEX] [MAIN_OBJECT_HEX] [FINAL_OWNER_HEX]\n",
            argv[0]);
        return 2;
    }
#else
    if (argc < 7 || argc > 10) {
        std::fprintf(
            stderr,
            "usage: %s PID LIB_BASE_HEX TIMEOUT_MS A9UTK1_PATH REPORT_PATH "
            "ACKNOWLEDGEMENT [PHYSICS_CONTEXT_HEX] [MAIN_OBJECT_HEX] "
            "[FINAL_OWNER_HEX]\n",
            argv[0]);
        return 2;
    }
#endif
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
    if (std::strcmp(argv[8], kUnifiedAcknowledgement) != 0) {
#else
    if (std::strcmp(argv[6], kUnifiedAcknowledgement) != 0) {
#endif
        std::fprintf(stderr,
                     "unified acknowledgement missing; no process opened\n");
        return 2;
    }
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
    if (access(argv[6], F_OK) == 0 || access(argv[7], F_OK) == 0 ||
        std::strcmp(argv[6], argv[7]) == 0) {
#else
    if (access(argv[5], F_OK) == 0) {
#endif
        std::fprintf(stderr,
                     "report path already exists; refusing to overwrite it\n");
        return 2;
    }
    if (!RunStateMachineSelfTest()) {
        std::fprintf(stderr,
                     "unified state-machine self-test failed; no process opened\n");
        return 3;
    }

    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t timeout_value = 0;
    std::uint64_t context_value = 0;
    std::uint64_t main_object_value = 0;
    std::uint64_t final_owner_value = 0;
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &timeout_value) ||
        (argc >= 10 && !ParseUnsigned(argv[9], 16, &context_value)) ||
        (argc >= 11 && !ParseUnsigned(argv[10], 16, &main_object_value)) ||
        (argc == 12 && !ParseUnsigned(argv[11], 16, &final_owner_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || timeout_value < 1000 || timeout_value > 300000) {
#else
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &timeout_value) ||
        (argc >= 8 && !ParseUnsigned(argv[7], 16, &context_value)) ||
        (argc >= 9 && !ParseUnsigned(argv[8], 16, &main_object_value)) ||
        (argc == 10 && !ParseUnsigned(argv[9], 16, &final_owner_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || timeout_value < 1000 || timeout_value > 300000) {
#endif
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }

    RecordingHeaderV1 recording_header{};
    std::vector<RecordingFrameV1> frames;
    if (!LoadRecording(argv[4], &recording_header, &frames)) {
        std::fprintf(stderr, "invalid A9UTK1 recording; no process opened\n");
        return 3;
    }
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
    a9tas::natural_preroll_v1::AnchorV1 natural_anchor{};
    if (!LoadNaturalPrerollAnchor(argv[5], recording_header, frames,
                                  &natural_anchor)) {
        std::fprintf(stderr,
                     "invalid or unbound A9NPA1 anchor; no process opened\n");
        return 3;
    }
#endif
#ifdef A9TAS_REPLAY_ALIGNMENT_GUARD_V1
    if (frames.size() < 2) {
        std::fprintf(stderr,
                     "alignment-guard replay requires at least two frames; "
                     "no process opened\n");
        return 3;
    }
#endif
    std::size_t unsupported_frame = 0;
    if (!RuntimeCapabilitiesSupported(frames, &unsupported_frame)) {
        std::fprintf(
            stderr,
            "unsupported active input/action at frame=%zu; no process opened\n",
            unsupported_frame);
        return 3;
    }
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    for (std::size_t index = 0; index < frames.size(); ++index) {
        if ((frames[index].skip_override_flags &
             a9tas::unified_tick_v1::kSkipTransformForced) != 0) {
            std::fprintf(stderr,
                         "final-writer replay forbids transform skip at "
                         "frame=%zu; no process opened\n",
                         index);
            return 3;
        }
    }
#endif
#ifdef A9TAS_UNIFIED_NITRO_OBSERVE_GATE_V1
    if (frames.size() != 1 ||
        !(frames[0].skip_override_flags &
          a9tas::unified_tick_v1::kSkipNitroActivation) ||
        frames[0].nitro_activation_count != 0) {
        std::fprintf(stderr,
                     "nitro observe gate requires one frame with nitro "
                     "skipped and count zero; no process opened\n");
        return 3;
    }
#endif
    std::uint64_t expected_control_writes = 0;
    for (const auto& frame : frames)
#ifndef A9TAS_UNIFIED_BRAKE_V1
        if (!(frame.skip_override_flags &
              a9tas::unified_tick_v1::kSkipSteer)
        )
#else
        if (!(frame.skip_override_flags &
              a9tas::unified_tick_v1::kSkipSteer)
            || !(frame.skip_override_flags &
                 a9tas::unified_tick_v1::kSkipBrake)
        )
#endif
            expected_control_writes += 2;
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
    std::uint64_t expected_nitro_rpc_requests = 0;
    std::uint64_t expected_nitro_activation_calls = 0;
    for (const auto& frame : frames) {
        if (!(frame.skip_override_flags &
              a9tas::unified_tick_v1::kSkipNitroActivation) &&
            frame.nitro_activation_count != 0) {
            ++expected_nitro_rpc_requests;
            expected_nitro_activation_calls +=
                frame.nitro_activation_count;
        }
    }
#ifdef A9TAS_UNIFIED_NITRO_OBSERVE_GATE_V1
    if (expected_nitro_rpc_requests == 0) expected_nitro_rpc_requests = 1;
#endif
#endif
#ifdef A9TAS_NATURAL_PREROLL_SEARCH_ONLY_V1
    (void)expected_control_writes;
#endif

    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto timeout_ms = static_cast<std::uint64_t>(timeout_value);
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
        std::fprintf(stderr, "unified object resolution failed stage=%u\n",
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
        c9c_address != c98_address + 4 ||
        !ReadMaps(pid, &maps) || !PipelineWritable(maps, delta_address, 8) ||
        !PipelineWritable(maps, c98_address, 8) ||
        !PipelineWritable(maps, completion, 8) ||
        !PipelineWritable(maps, callback_flags, 2) ||
        !PipelineWritable(maps, f64, 4) ||
        !PipelineWritable(maps, world_accumulator, 4) ||
        !PipelineWritable(maps, backend.native_body,
                          kNativeBodyAuditSize) ||
        !PipelineWritable(maps, backend.base + kWrapperAuditOffset,
                          kWrapperAuditSize)) {
        std::fprintf(stderr, "unified address validation failed\n");
        close(mem);
        return 3;
    }

#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
    const int nitro_rpc = a9tas::unified_nitro_rpc_v1::Connect();
    if (nitro_rpc < 0) {
        std::fprintf(stderr,
                     "nitro RPC socket unavailable; no thread was attached\n");
        close(mem);
        return 6;
    }
    std::uint64_t nitro_rpc_sequence = MonotonicNs();
    if (nitro_rpc_sequence == 0) nitro_rpc_sequence = 1;
#endif

    UnifiedReportHeaderV5 report{};
    std::memcpy(report.magic, kUnifiedReportMagic, sizeof(report.magic));
    report.version = kUnifiedReportVersion;
    report.header_size = sizeof(report);
    report.frame_audit_size = sizeof(UnifiedFrameAuditV5);
    report.flags = kUnifiedTargetVerified | kUnifiedIdentityVerified |
                   kUnifiedRecordingValidated |
                   kUnifiedCapabilityValidated
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
                   | kUnifiedCompletionWriteCertificate
#endif
                   ;
    report.recording_frames = static_cast<std::uint32_t>(frames.size());
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

#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    a9tas::final_writer_unified_v1::Runtime final_writer_runtime{};
    if (!a9tas::final_writer_unified_v1::Setup(
            pid, mem, base, vehicle.physics_base,
            backend.native_pose_address, backend.native_linear_address,
            argv[4], static_cast<std::uint32_t>(frames.size()),
            &final_writer_runtime)) {
        std::fprintf(stderr,
                     "final-writer hash/source/storage setup failed before "
                     "attach\n");
        close(mem);
        return 3;
    }
#endif

    std::vector<TracedThread> threads;
    std::uint64_t attach_failures = 0;
    std::size_t initial = 0;
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    if (g_a9tas_final_writer_arm_before_resume_v1) {
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
        initial = AttachNewThreadsStopped(
            pid, g_a9tas_race_lifecycle_state_address_v1, 0, 0, 0,
            &threads, &attach_failures, RaceLifecycleDr7());
#else
        initial = AttachNewThreadsStopped(
            pid, delta_address, c98_address, c9c_address,
            world_accumulator, &threads, &attach_failures,
            BoundaryDr7(true));
#endif
    } else
#endif
    {
        initial = AttachNewThreads(
            pid, delta_address, c98_address, c9c_address,
            world_accumulator, &threads, &attach_failures,
            BoundaryDr7(true));
    }
    report.initial_threads = static_cast<std::uint32_t>(initial);
    report.ptrace_errors += attach_failures;
    if (initial == 0 || attach_failures != 0) {
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
        if (g_a9tas_final_writer_arm_before_resume_v1)
            std::fprintf(stderr,
                         "final-writer stopped initial attach failed"
                         " initial=%zu failures=%" PRIu64 "\n",
                         initial, attach_failures);
#endif
        for (auto& thread : threads) {
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        }
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
        if (!a9tas::final_writer_unified_v1::ResetUninstalled(
                mem, &final_writer_runtime))
            std::fprintf(stderr,
                         "final-writer unattached storage reset failed\n");
#endif
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
        close(nitro_rpc);
#endif
        close(mem);
        return 7;
    }

#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    if (g_a9tas_final_writer_arm_before_resume_v1) {
        char ready_path[112]{};
        std::snprintf(ready_path, sizeof(ready_path),
                      "/data/local/tmp/a9tas_final_writer_ready_%d",
                      static_cast<int>(pid));
        const auto cleanup_prearm_failure = [&]() {
            unlink(ready_path);
            if (final_writer_runtime.installed)
                a9tas::final_writer_unified_v1::ConditionalRollback(
                    mem, &final_writer_runtime);
            for (auto& thread : threads)
                if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        };
        std::int64_t paused_delta = -1;
        std::uint64_t observed_start_ticks = 0;
        std::uint32_t prearm_stage = 0;
        auto install_result =
            a9tas::final_writer_unified_v1::PrearmInstallResult::kOk;
        if (access(ready_path, F_OK) == 0) {
            prearm_stage = 1;
        } else {
            install_result =
                a9tas::final_writer_unified_v1::InstallBeforeResume(
                    pid, mem, delta_address, c98_address, c9c_address,
                    world_accumulator, &threads, &report.thread_additions,
                    &final_writer_runtime);
            if (install_result !=
                a9tas::final_writer_unified_v1::PrearmInstallResult::kOk)
                prearm_stage = 100 + static_cast<std::uint32_t>(install_result);
        }
        if (prearm_stage == 0 &&
            !ReadExact(mem, delta_address, &paused_delta,
                       sizeof(paused_delta)))
            prearm_stage = 2;
        if (prearm_stage == 0 && paused_delta != 0) prearm_stage = 3;
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
        std::uint32_t lifecycle_state = 0;
        if (prearm_stage == 0 &&
            (!ReadExact(mem, g_a9tas_race_lifecycle_state_address_v1,
                        &lifecycle_state, sizeof(lifecycle_state)) ||
             lifecycle_state !=
                 a9tas::race_lifecycle_v1::kCountdownState))
            prearm_stage = 6;
        if (prearm_stage == 0 &&
            !ProgramAllStoppedThreads(
                &threads, g_a9tas_race_lifecycle_state_address_v1,
                0, 0, 0, RaceLifecycleDr7()))
            prearm_stage = 7;
#endif
        if (prearm_stage == 0 &&
            (!ReadProcessStartTicks(pid, &observed_start_ticks) ||
             observed_start_ticks !=
                 g_a9tas_final_writer_expected_start_ticks_v1))
            prearm_stage = 4;
        if (prearm_stage == 0 &&
            !CreateFinalWriterArmedMarker(
                ready_path, pid,
                g_a9tas_final_writer_expected_start_ticks_v1,
                delta_address, threads.size()))
            prearm_stage = 5;
        if (prearm_stage != 0) {
            cleanup_prearm_failure();
            std::fprintf(stderr,
                         "final-writer pre-resume install failed closed"
                         " stage=%u install_result=%u delta=%" PRId64
                         " threads=%zu additions=%" PRIu64 "\n",
                         prearm_stage,
                         static_cast<std::uint32_t>(install_result),
                         paused_delta, threads.size(),
                         report.thread_additions);
            close(mem);
            return 7;
        }
        std::printf(
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
            "RACE_LIFECYCLE_FINAL_WRITER_READY_ARMED pid=%d start_ticks=%" PRIu64
            " ready=%s frames=%zu state_address=0x%" PRIxPTR " state=2"
            " target_threads_attached=%zu vptr_writes=1"
            " gameplay_state_writes=0 all_target_threads_frozen=1\n",
#else
            "FINAL_WRITER_READY_ARMED pid=%d start_ticks=%" PRIu64
            " ready=%s frames=%zu target_threads_attached=%zu"
            " vptr_writes=1 gameplay_state_writes=0"
            " all_target_threads_frozen=1\n",
#endif
            static_cast<int>(pid),
            g_a9tas_final_writer_expected_start_ticks_v1, ready_path,
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
            frames.size(), g_a9tas_race_lifecycle_state_address_v1,
            threads.size());
#else
            frames.size(), threads.size());
#endif
        std::fflush(stdout);
        if (!WaitForFinalWriterMarkerRemoval(ready_path, timeout_ms) ||
            !ReadProcessStartTicks(pid, &observed_start_ticks) ||
            observed_start_ticks !=
                g_a9tas_final_writer_expected_start_ticks_v1 ||
            !a9tas::final_writer_unified_v1::ResumeAfterPrearm(
                &threads, &final_writer_runtime)) {
            cleanup_prearm_failure();
            std::fprintf(stderr,
                         "final-writer armed resume failed closed\n");
            close(mem);
            return 7;
        }
        std::printf(
            "FINAL_WRITER_RESUME_RELEASED pid=%d start_ticks=%" PRIu64
            " vptr_installed=1 target_threads_resumed=%zu\n",
            static_cast<int>(pid),
            g_a9tas_final_writer_expected_start_ticks_v1, threads.size());
        std::fflush(stdout);
    }
#endif

#ifdef A9TAS_RACE_LIFECYCLE_START_V1
    std::uint32_t lifecycle_pre_loop_retired_threads = 0;
    const char* lifecycle_failure_reason = "not_started";
    std::uint64_t lifecycle_events_observed = 0;
    if (!WaitForAuthoritativeRaceStart(
            pid, mem, timeout_ms,
            g_a9tas_race_lifecycle_state_address_v1, delta_address,
            c98_address, c9c_address, world_accumulator, &threads, &report,
            &lifecycle_pre_loop_retired_threads, &lifecycle_failure_reason,
            &lifecycle_events_observed)) {
        const bool frozen = a9tas::final_writer_unified_v1::FreezeOthers(
            pid, 0, delta_address, c98_address, c9c_address,
            world_accumulator, &threads, &report.thread_additions);
        if (!frozen ||
            !a9tas::final_writer_unified_v1::ConditionalRollback(
                mem, &final_writer_runtime))
            ++report.rollback_failures;
        for (auto& thread : threads)
            if (thread.live &&
                !ClearAndDetach(thread.tid, thread.stopped))
                ++report.ptrace_errors;
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
        close(nitro_rpc);
#endif
        close(mem);
        std::fprintf(stderr,
                     "authoritative race-start handoff failed closed;"
                     " reason=%s lifecycle_events=%" PRIu64
                     " retired=%u read_errors=%" PRIu64
                     " ptrace_errors=%" PRIu64
                     " semantic_errors=%" PRIu64
                     " no replay frame consumed\n",
                     lifecycle_failure_reason, lifecycle_events_observed,
                     lifecycle_pre_loop_retired_threads, report.read_errors,
                     report.ptrace_errors, report.semantic_errors);
        return 7;
    }
#endif

    std::printf(
        "UNIFIED_TICK_EXECUTOR_V1_BUILD_ONLY pid=%d frames=%zu "
        "main=0x%" PRIxPTR " owner=0x%" PRIxPTR
        " context=0x%" PRIxPTR " native=0x%" PRIxPTR
        " threads=%zu capability=fixed-delta+optional-steer+final-64+12\n",
        static_cast<int>(pid), frames.size(), main_object, final_owner,
        context, backend.native_body, threads.size());
    std::fflush(stdout);

#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
    bool searching_anchor = true;
    bool pending_anchor_match = false;
    std::uint64_t anchor_search_cycles = 0;
    std::uint64_t anchor_rejected_candidates = 0;
    std::uint32_t matched_anchor_cycle = UINT32_MAX;
    std::vector<UnifiedFrameAuditV5> anchor_search_audits;
    Machine machine{Stage::kWaiting, 0, 1};
#else
    Machine machine{Stage::kWaiting, 0, frames.size()};
#endif
    pid_t cycle_tid = 0;
    pid_t post_phase_tid = 0;
    UnifiedFrameAuditV5 pending{};
    bool pending_audit = false;
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
    auto run_prephysics_nitro_rpc = [&](const RecordingFrameV1& frame) {
        const bool skipped =
            (frame.skip_override_flags &
             a9tas::unified_tick_v1::kSkipNitroActivation) != 0;
        const std::uint32_t activations =
            skipped ? 0u : frame.nitro_activation_count;
        bool should_request = activations != 0;
#ifdef A9TAS_UNIFIED_NITRO_OBSERVE_GATE_V1
        should_request = should_request || machine.frame_index == 0;
#endif
        if (!should_request) return true;
        // The RPC is valid only at the stopped fixed-delta event.  C98/C9C
        // must not have occurred yet, so the real game physics has not been
        // released for this frame.
        const auto delta_event =
            pending.*(&UnifiedFrameAuditV5::delta_event);
        if (delta_event == 0 || pending.c98_event != 0 ||
            pending.c9c_prefix_event != 0 ||
            pending.nitro_phase_event != 0 || nitro_rpc_sequence == 0)
            return false;
        const std::uint64_t sequence = nitro_rpc_sequence++;
        ++report.nitro_rpc_requests;
        pending.nitro_phase_event = report.event_count;
        pending.nitro_requested_activations = activations;
        a9tas::nitro_rpc_v1::Response response{};
        const bool ok = a9tas::unified_nitro_rpc_v1::Request(
            nitro_rpc, sequence, final_owner, activations, &response);
        pending.nitro_response = response;
        if (!ok) {
            ++report.nitro_rpc_failures;
            return false;
        }
        report.nitro_activation_calls += activations;
        report.flags |= kUnifiedNitroRpcUsed;
        pending.flags |= kUnifiedNitroRpcObserved;
        if (activations != 0) pending.flags |= kUnifiedNitroActivated;
        return true;
    };
#endif
    bool complete = false;
    std::vector<UnifiedFrameAuditV5> audits;
    audits.reserve(frames.size());
    std::uint32_t accounted_threads =
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
        lifecycle_pre_loop_retired_threads;
#else
        0;
#endif
    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + timeout_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 250000000ULL;
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    enum class FinalWriterStartAnchorStage : std::uint8_t {
        kAwaitC98,
        kAwaitC9C,
        kAwaitDeltaZero,
        kReady,
    };
    FinalWriterStartAnchorStage final_writer_start_anchor_stage =
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
        // The game-owned 2 -> 3 edge already establishes IN_RACE.  Matching
        // upstream, the next positive fixed-delta callback owns replay tick 0.
        FinalWriterStartAnchorStage::kReady;
#else
        FinalWriterStartAnchorStage::kAwaitC98;
#endif
    pid_t final_writer_start_anchor_tid = 0;
    std::uint64_t final_writer_start_anchor_skipped_deltas = 0;
#endif

    const auto semantic_fault = [&](const char* reason, pid_t tid,
                                    unsigned long dr6) {
        std::fprintf(stderr,
                     "unified_fault reason=%s event=%" PRIu64
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
                if (!ReadExact(mem, world_accumulator, &bits,
                               sizeof(bits))) {
                    ++report.read_errors;
                } else {
                    std::memcpy(&value, &bits, sizeof(value));
                    std::uint32_t actions = 0;
                    if (!std::isfinite(value) || std::fabs(value) > 60.0f ||
                        !pending_audit || tid == cycle_tid ||
                        pending.callback_deferred_clear_event == 0 ||
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
                        !a9tas::final_writer_unified_v1::
                            CommitAtWorldBoundary(
                                mem,
                                static_cast<std::uint32_t>(
                                    machine.frame_index),
                                &final_writer_runtime) ||
#endif
                        !Advance(&machine, Event::kWorldCommit, false, false,
                                 false, &actions)) {
                        semantic_fault("unexpected_world_commit", tid, dr6);
                    } else {
                        pending.commit_tid = static_cast<std::int32_t>(tid);
                        pending.world_commit_event = report.event_count;
                        pending.flags |= kUnifiedCommitted;
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                        if (searching_anchor) {
                            if (pending_anchor_match) {
                                pending.flags |=
                                    kUnifiedNaturalAnchorMatched;
                                matched_anchor_cycle =
                                    static_cast<std::uint32_t>(
                                        anchor_search_audits.size());
                            }
                            anchor_search_audits.push_back(pending);
                            ++anchor_search_cycles;
                            pending = {};
                            pending_audit = false;
                            if (pending_anchor_match) {
                                searching_anchor = false;
#ifdef A9TAS_NATURAL_PREROLL_SEARCH_ONLY_V1
                                complete = true;
#else
                                machine = Machine{
                                    Stage::kWaiting, 0, frames.size(),
                                };
#endif
                            } else {
                                machine = Machine{Stage::kWaiting, 0, 1};
                            }
                            pending_anchor_match = false;
#ifdef A9TAS_NATURAL_PREROLL_SEARCH_ONLY_V1
                            if (complete) {
                                post_phase_tid = 0;
                                cycle_tid = 0;
                            } else
#endif
                            if (!rearm_post_owner_for_input(tid)) {
                                ++report.ptrace_errors;
                            } else {
                                post_phase_tid = 0;
                                cycle_tid = 0;
                            }
                        } else {
#endif
                        audits.push_back(pending);
                        pending = {};
                        pending_audit = false;
                        report.processed_frames = static_cast<std::uint32_t>(
                            machine.frame_index);
                        if (machine.stage == Stage::kComplete) {
                            complete = true;
                        } else if (!rearm_post_owner_for_input(tid)) {
                            ++report.ptrace_errors;
                        } else {
                            post_phase_tid = 0;
                            cycle_tid = 0;
                        }
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                        }
#endif
                    }
                }
            } else if (hit == 8UL && machine.stage == Stage::kWaiting) {
                // The world worker is independent of the physics/input owner.
                // Ignore commits until a certified frame is waiting for one.
            } else if (tid == post_phase_tid) {
                if (tid != cycle_tid) {
                    semantic_fault("post_phase_wrong_owner", tid, dr6);
                } else if (hit == 1UL) {
                    // The completion transition must already have happened
                    // between DT and C9C and is certified from two stopped
                    // snapshots. A later transition belongs to another cycle.
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
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                            if (searching_anchor) {
                                if (!ReadAuditSnapshot(
                                        mem, vehicle, backend, base,
                                        &pending.before)) {
                                    ++report.read_errors;
                                } else {
                                    pending_anchor_match =
                                        NaturalPrerollAnchorMatches(
                                            pending.before, natural_anchor);
                                    pending.immediate = pending.before;
                                    pending.flags |= kUnifiedAuditExact |
                                                     kUnifiedGate2Complete;
                                    pending.callback_close_event =
                                        report.event_count;
                                    pending_audit = true;
                                    if (!Advance(
                                            &machine,
                                            Event::kCallbackClose, true,
                                            true, false, &actions))
                                        semantic_fault(
                                            "anchor_callback_close_rejected",
                                            tid, dr6);
                                }
                            } else
#endif
                            {
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
                            if (machine.frame_index >= frames.size()) {
                                semantic_fault("frame_index_overflow", tid,
                                               dr6);
                            } else {
                                const auto& frame = frames[machine.frame_index];
                                pending.recording_tick = frame.tick;
                                pending.recording_monotonic_ns =
                                    frame.monotonic_ns;
                                std::memcpy(pending.recorded_transform,
                                            frame.transform_bits,
                                            sizeof(frame.transform_bits));
                                std::memcpy(pending.recorded_linear,
                                            frame.linear_velocity_bits,
                                            sizeof(frame.linear_velocity_bits));
                                a9tas::final_writer_replay_v1::FrameAudit
                                    payload_audit{};
                                a9tas::final_writer_replay_v1::Evidence
                                    payload_evidence{};
                                const std::uint32_t external_index =
                                    static_cast<std::uint32_t>(
                                        machine.frame_index);
                                if (!a9tas::final_writer_unified_v1::
                                        AcknowledgeAtCallbackClose(
                                            mem, external_index, frame,
                                            &payload_audit,
                                            &payload_evidence,
                                            &final_writer_runtime) ||
                                    !ReadAuditSnapshot(
                                        mem, vehicle, backend, base,
                                        &pending.immediate)) {
                                    ++report.read_errors;
                                    semantic_fault(
                                        "final_writer_callback_audit_failed",
                                        tid, dr6);
                                } else {
                                    pending.before = pending.immediate;
                                    std::memcpy(
                                        pending.before.native_body +
                                            a9tas::unified_tick_v1::
                                                kTransformOffset,
                                        payload_audit.before_transform,
                                        sizeof(payload_audit.before_transform));
                                    std::memcpy(
                                        pending.before.native_body +
                                            a9tas::unified_tick_v1::
                                                kLinearVelocityOffset,
                                        payload_audit.before_linear,
                                        sizeof(payload_audit.before_linear));
                                    const bool equal =
                                        (payload_audit.flags &
                                         a9tas::final_writer_replay_v1::
                                             kAuditEqual) != 0;
                                    if (equal) {
                                        pending.flags |=
                                            kUnifiedCorrectionEqual;
                                        ++report.equal_frames;
                                    } else {
                                        pending.flags |=
                                            kUnifiedCorrectionCorrected;
                                        ++report.corrected_frames;
                                        ++report.write_attempts;
                                    }
                                    pending.flags |=
                                        kUnifiedAuditExact |
                                        kUnifiedGate2Complete;
                                    pending.callback_close_event =
                                        report.event_count;
                                    pending_audit = true;
                                    if (external_index == 0 &&
                                        final_writer_runtime.
                                            first_frame_others_frozen &&
                                        !a9tas::final_writer_unified_v1::
                                            ResumeFrozenOthers(
                                                tid, &threads,
                                                &final_writer_runtime)) {
                                        semantic_fault(
                                            "final_writer_resume_frozen_failed",
                                            tid, dr6);
                                    } else if (!Advance(
                                                   &machine,
                                                   Event::kCallbackClose,
                                                   true, equal, false,
                                                   &actions)) {
                                        semantic_fault(
                                            "final_writer_callback_close_rejected",
                                            tid, dr6);
                                    }
                                }
                            }
#else
                            if (machine.frame_index >= frames.size()) {
                                semantic_fault("frame_index_overflow", tid,
                                               dr6);
                            } else {
                                const auto& frame = frames[machine.frame_index];
                                pending.recording_tick = frame.tick;
                                pending.recording_monotonic_ns =
                                    frame.monotonic_ns;
                                std::memcpy(pending.recorded_transform,
                                            frame.transform_bits,
                                            sizeof(frame.transform_bits));
                                std::memcpy(pending.recorded_linear,
                                            frame.linear_velocity_bits,
                                            sizeof(frame.linear_velocity_bits));
                                if (!ReadAuditSnapshot(mem, vehicle, backend,
                                                       base,
                                                       &pending.before)) {
                                    ++report.read_errors;
                                } else {
                                    const bool equal =
                                        ComponentPayloadEqualUnified(
                                            pending.before, frame);
                                    const bool skip =
                                        (frame.skip_override_flags &
                                         a9tas::unified_tick_v1::
                                             kSkipTransformForced) != 0;
                                    bool transport_ok = true;
                                    bool corrected = false;
                                    if (skip) {
                                        pending.flags |=
                                            kUnifiedCorrectionSkipped;
                                        ++report.skipped_frames;
                                    } else if (equal) {
                                        pending.flags |=
                                            kUnifiedCorrectionEqual;
                                        ++report.equal_frames;
#ifdef A9TAS_REPLAY_ALIGNMENT_GUARD_V1
                                    } else if (
                                        machine.frame_index == 0 &&
                                        !FirstFrameAlignedUnified(
                                            pending.before, frames)) {
                                        transport_ok = false;
                                        semantic_fault(
                                            "first_frame_alignment_guard",
                                            tid, dr6);
#endif
                                    } else {
                                        corrected = true;
                                        pending.flags |=
                                            kUnifiedCorrectionCorrected;
                                        ++report.corrected_frames;
                                        ++report.write_attempts;
                                        const bool pose_ok =
                                            WriteExactVerified(
                                                mem,
                                                backend.native_pose_address,
                                                frame.transform_bits,
                                                sizeof(frame.transform_bits));
                                        const bool linear_ok =
                                            WriteExactVerified(
                                                mem,
                                                backend.native_linear_address,
                                                frame.linear_velocity_bits,
                                                sizeof(frame.linear_velocity_bits));
                                        transport_ok = pose_ok && linear_ok;
                                        if (!transport_ok) {
                                            ++report.write_failures;
                                            ++report.rollback_attempts;
                                            if (!RollbackUnified(
                                                    mem, vehicle, backend,
                                                    base, pending.before))
                                                ++report.rollback_failures;
                                            semantic_fault(
                                                "correction_transport_failed",
                                                tid, dr6);
                                        }
                                    }
                                    if (transport_ok &&
                                        !ReadAuditSnapshot(
                                            mem, vehicle, backend, base,
                                            &pending.immediate)) {
                                        ++report.read_errors;
                                        if (corrected) {
                                            ++report.rollback_attempts;
                                            if (!RollbackUnified(
                                                    mem, vehicle, backend,
                                                    base, pending.before))
                                                ++report.rollback_failures;
                                        }
                                    } else if (transport_ok) {
                                        const AuditSnapshotV1 expected =
                                            ExpectedImmediateUnified(
                                                pending.before, frame,
                                                corrected);
                                        if (std::memcmp(
                                                &expected,
                                                &pending.immediate,
                                                sizeof(expected)) != 0) {
                                            if (corrected) {
                                                ++report.rollback_attempts;
                                                if (!RollbackUnified(
                                                        mem, vehicle, backend,
                                                        base,
                                                        pending.before))
                                                    ++report.rollback_failures;
                                            }
                                            semantic_fault(
                                                "immediate_audit_mismatch", tid,
                                                dr6);
                                        } else {
                                            pending.flags |=
                                                kUnifiedAuditExact |
                                                kUnifiedGate2Complete;
                                            pending.callback_close_event =
                                                report.event_count;
                                            pending_audit = true;
                                            if (!Advance(
                                                    &machine,
                                                    Event::kCallbackClose,
                                                    true, equal, skip,
                                                    &actions))
                                                semantic_fault(
                                                    "callback_close_rejected",
                                                    tid, dr6);
                                        }
                                    }
                                }
                            }
#endif
                            }
                        } else if (
                            dispatching == 0 &&
                            machine.stage == Stage::kWaitDeferredClear) {
                            if (!pending_audit ||
                                pending.callback_deferred_clear_event != 0 ||
                                !Advance(
                                    &machine,
                                    Event::kDeferredCallbackClear, false,
                                    false, false, &actions)) {
                                semantic_fault(
                                    "deferred_callback_clear_rejected", tid,
                                    dr6);
                            } else {
                                pending.callback_deferred_clear_event =
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
                                     false, &actions))
                            semantic_fault("unexpected_f64", tid, dr6);
                        else
                            pending.f64_event = report.event_count;
                    }
                } else if (hit == 8UL) {
                    semantic_fault("world_commit_before_ready", tid, dr6);
                }
            } else {
                // Threads which are not the current post-phase owner always
                // carry the input-phase layout.
                if (hit == 1UL) {
                    std::int64_t delta_us = 0;
                    if (!ReadExact(mem, delta_address, &delta_us,
                                   sizeof(delta_us))) {
                        ++report.read_errors;
                    }
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
                    else if (
                        machine.stage == Stage::kWaiting &&
                        final_writer_start_anchor_stage !=
                            FinalWriterStartAnchorStage::kReady) {
                        // MainTimeSource+0x150 is written on every scaled-time
                        // callback, including menu/countdown cycles which
                        // never enter gameplay input.  The upstream hook is
                        // one layer later: it runs only on a real physics
                        // frame.  Keep the writer disarmed until one complete
                        // same-thread C98 -> C9C -> zero input cycle proves
                        // that the race input phase is live.  Frame zero is
                        // the next positive delta after that closed witness.
                        if (delta_us == 0 &&
                            final_writer_start_anchor_stage ==
                                FinalWriterStartAnchorStage::kAwaitDeltaZero &&
                            tid == final_writer_start_anchor_tid) {
                            final_writer_start_anchor_stage =
                                FinalWriterStartAnchorStage::kReady;
                            final_writer_start_anchor_tid = 0;
                            std::fprintf(
                                stderr,
                                "final_writer_start_anchor_complete event=%" PRIu64
                                " tid=%d skipped_deltas=%" PRIu64 "\n",
                                report.event_count, static_cast<int>(tid),
                                final_writer_start_anchor_skipped_deltas);
                        } else if (
                            final_writer_start_anchor_stage ==
                            FinalWriterStartAnchorStage::kAwaitC98 &&
                            delta_us >= 0) {
                            if (delta_us > 0)
                                ++final_writer_start_anchor_skipped_deltas;
                        } else {
                            semantic_fault(
                                "final_writer_start_anchor_delta_order", tid,
                                dr6);
                        }
                    }
#endif
                    else if (machine.stage == Stage::kWaiting &&
                             delta_us == 0) {
                        std::uint32_t actions = 0;
                        if (!Advance(&machine, Event::kDeltaZero, false,
                                     false, false, &actions))
                            semantic_fault("waiting_zero_rejected", tid, dr6);
                    }
                    else if (machine.stage == Stage::kWaiting &&
                               delta_us > 0
#ifndef A9TAS_FINAL_WRITER_REPLAY_V1
                               && delta_us <= 1000000
#endif
                    ) {
                        std::uint32_t actions = 0;
                        if (!Advance(&machine, Event::kDeltaNonzero, false,
                                     false, false, &actions)) {
                            semantic_fault("delta_open_rejected", tid, dr6);
                        } else {
                            cycle_tid = tid;
                            pending = {};
                            pending.cycle_tid = static_cast<std::int32_t>(tid);
                            pending.original_delta_us = delta_us;
                            pending.applied_delta_us =
                                recording_header.fixed_interval_us;
                            pending.delta_event = report.event_count;
                            bool delta_ready_for_actions = false;
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
#ifdef A9TAS_FINAL_WRITER_NATURAL_ACTION_V1
                            if (!a9tas::final_writer_unified_v1::BeginTick(
                                    mem,
                                    static_cast<std::uint32_t>(
                                        machine.frame_index),
                                    frames[machine.frame_index],
                                    &final_writer_runtime)) {
#else
                            if (!a9tas::final_writer_unified_v1::BeginTick(
                                    mem,
                                    static_cast<std::uint32_t>(
                                        machine.frame_index),
                                    &final_writer_runtime)) {
#endif
                                semantic_fault(
                                    "final_writer_begin_cursor_mismatch", tid,
                                    dr6);
                            }
#endif
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                            if (!ReadExact(mem, completion,
                                           &pending.completion_before,
                                           sizeof(pending.completion_before))) {
                                ++report.read_errors;
                            } else {
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                                if (searching_anchor) {
                                    // The complete anchor-search cycle is
                                    // observational: no delta or control write.
                                    pending.applied_delta_us = 0;
                                } else
#endif
                                {
                                    std::memcpy(
                                        &pending.steering_bits,
                                        &frames[machine.frame_index].steering,
                                        sizeof(pending.steering_bits));
                                    const std::int64_t fixed_delta =
                                        recording_header.fixed_interval_us;
                                    if (!WriteExactVerified(
                                            mem, delta_address, &fixed_delta,
                                            sizeof(fixed_delta))) {
                                        ++report.write_failures;
                                        semantic_fault(
                                            "fixed_delta_write_failed", tid,
                                            dr6);
                                    } else {
                                        ++report.delta_writes;
                                        delta_ready_for_actions = true;
                                    }
                                }
                            }
#else
                            std::memcpy(
                                &pending.steering_bits,
                                &frames[machine.frame_index].steering,
                                sizeof(pending.steering_bits));
                            const std::int64_t fixed_delta =
                                recording_header.fixed_interval_us;
                            if (!ReadExact(mem, completion,
                                           &pending.completion_before,
                                           sizeof(pending.completion_before))) {
                                ++report.read_errors;
                            } else if (!WriteExactVerified(
                                     mem, delta_address, &fixed_delta,
                                     sizeof(fixed_delta))) {
                                ++report.write_failures;
                                semantic_fault("fixed_delta_write_failed", tid,
                                               dr6);
                             } else {
                                 ++report.delta_writes;
                                 delta_ready_for_actions = true;
                             }
#endif
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
                            if (delta_ready_for_actions &&
                                !a9tas::final_writer_unified_v1::
                                    ArmWriterForTick(
                                        mem,
                                        static_cast<std::uint32_t>(
                                            machine.frame_index),
                                        &final_writer_runtime)) {
                                semantic_fault(
                                    "final_writer_frame_permit_failed", tid,
                                    dr6);
                            }
#endif
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
                            if (delta_ready_for_actions &&
                                report.semantic_errors == 0 &&
                                !ProgramStoppedThread(
                                    tid, delta_address, c98_address,
                                    c9c_address, completion,
                                    CompletionInputDr7())) {
                                ++report.ptrace_errors;
                                semantic_fault(
                                    "completion_watch_arm_failed", tid, dr6);
                            }
#endif
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
                            if (delta_ready_for_actions &&
                                !run_prephysics_nitro_rpc(
                                    frames[machine.frame_index])) {
                                semantic_fault("prephysics_nitro_rpc_failed",
                                               tid, dr6);
                            }
#else
                            (void)delta_ready_for_actions;
#endif
                        }
                    } else if (machine.stage == Stage::kInputOpen &&
                               tid == cycle_tid && delta_us == 0) {
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                        if (searching_anchor) {
                            std::uint32_t actions = 0;
                            if (!Advance(&machine, Event::kDeltaZero, false,
                                         false, false, &actions)) {
                                semantic_fault(
                                    "anchor_cancelled_cycle_rejected", tid,
                                    dr6);
                            } else {
                                pending = {};
                                pending_audit = false;
                                pending_anchor_match = false;
                                cycle_tid = 0;
                            }
                        } else {
#endif
                        // V1 cannot distinguish a paused candidate before it
                        // writes fixed delta. Continuing here would rewrite the
                        // same field every paused outer loop. Fail after the
                        // first cancelled candidate; a future version needs a
                        // separately proven pause-state gate.
                        semantic_fault("paused_cycle_after_delta_write", tid,
                                       dr6);
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                        }
#endif
                    } else {
                        std::fprintf(
                            stderr,
                            "unexpected_delta_value value=%" PRId64
                            " address=0x%" PRIxPTR " event=%" PRIu64
                            " frame=%zu stage=%u tid=%d owner=%d\n",
                            delta_us, delta_address, report.event_count,
                            machine.frame_index,
                            static_cast<unsigned>(machine.stage),
                            static_cast<int>(tid), static_cast<int>(cycle_tid));
                        semantic_fault("unexpected_delta", tid, dr6);
                    }
                } else if (hit == 2UL || hit == 4UL) {
                    if (
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
                        machine.stage == Stage::kWaiting &&
                        final_writer_start_anchor_stage !=
                            FinalWriterStartAnchorStage::kReady
#else
                        false
#endif
                    ) {
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
                        if (hit == 2UL &&
                            final_writer_start_anchor_stage ==
                                FinalWriterStartAnchorStage::kAwaitC98) {
                            final_writer_start_anchor_stage =
                                FinalWriterStartAnchorStage::kAwaitC9C;
                            final_writer_start_anchor_tid = tid;
                            std::fprintf(
                                stderr,
                                "final_writer_start_anchor_c98 event=%" PRIu64
                                " tid=%d\n",
                                report.event_count, static_cast<int>(tid));
                        } else if (
                            hit == 4UL &&
                            final_writer_start_anchor_stage ==
                                FinalWriterStartAnchorStage::kAwaitC9C &&
                            tid == final_writer_start_anchor_tid) {
                            final_writer_start_anchor_stage =
                                FinalWriterStartAnchorStage::kAwaitDeltaZero;
                            std::fprintf(
                                stderr,
                                "final_writer_start_anchor_c9c event=%" PRIu64
                                " tid=%d\n",
                                report.event_count, static_cast<int>(tid));
                        } else if (
                            hit == 4UL &&
                            final_writer_start_anchor_stage ==
                                FinalWriterStartAnchorStage::kAwaitC98) {
                            // A stopped attach may expose the tail of a cycle
                            // whose C98 happened before all threads froze.
                            // It is not a complete witness and consumes no
                            // replay state.
                        } else {
                            semantic_fault(
                                "final_writer_start_anchor_input_order", tid,
                                dr6);
                        }
#endif
                    } else if (machine.stage == Stage::kWaiting) {
                        // Initial attach or a newly-created thread may land in
                        // an incomplete prefix. Do not select/consume a frame.
                    } else if (tid != cycle_tid) {
                        semantic_fault("input_cross_thread", tid, dr6);
                    } else if (
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                               !searching_anchor &&
#endif
                               machine.frame_index >= frames.size()) {
                        semantic_fault("input_frame_index_overflow", tid,
                                       dr6);
                    } else {
                        const auto& frame = frames[machine.frame_index];
                        std::uint32_t actions = 0;
                        const Event event =
                            hit == 2UL ? Event::kC98 : Event::kC9C;
                        std::uint64_t pair_after = 0;
                        if (!Advance(&machine, event, false, false, false,
                                     &actions)) {
                            semantic_fault("input_order", tid, dr6);
                        }
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                        else {
                            bool steering_ok = false;
                            if (searching_anchor) {
                                steering_ok = ReadExact(
                                    mem, c98_address, &pair_after,
                                    sizeof(pair_after));
                            } else
                            {
                                steering_ok = ApplySteering(
                                    mem, c98_address, frame,
                                    &report.control_writes, &pair_after);
                            }
                            if (!steering_ok) {
                                ++report.write_failures;
                                semantic_fault("steering_write_failed", tid,
                                               dr6);
                            } else {
#else
                        else if (!ApplySteering(
                                       mem, c98_address, frame,
                                       &report.control_writes,
                                       &pair_after)) {
                            ++report.write_failures;
                            semantic_fault("steering_write_failed", tid, dr6);
                        } else {
#endif
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                            if (!searching_anchor &&
                                !(frame.skip_override_flags &
                                  a9tas::unified_tick_v1::kSkipSteer))
#else
                            if (!(frame.skip_override_flags &
                                  a9tas::unified_tick_v1::kSkipSteer))
#endif
                                pending.flags |= kUnifiedSteeringApplied;
#ifdef A9TAS_UNIFIED_BRAKE_V1
                            if (
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                                !searching_anchor &&
#endif
                                !(frame.skip_override_flags &
                                  a9tas::unified_tick_v1::kSkipBrake))
                                pending.flags |= kUnifiedBrakeApplied;
#endif
                            if (event == Event::kC98) {
                                pending.c98_event = report.event_count;
                                pending.c98_pair_after = pair_after;
                            } else {
                                pending.c9c_pair_after = pair_after;
                                std::uint16_t flags_value = 0;
                                if (!ReadExact(
                                        mem, completion,
                                        &pending.completion_after,
                                        sizeof(pending.completion_after)) ||
                                    !ReadExact(mem, callback_flags,
                                               &flags_value,
                                               sizeof(flags_value))) {
                                    ++report.read_errors;
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                                } else if (
                                    searching_anchor &&
                                    (pending.completion_after ==
                                         pending.completion_before ||
                                     (flags_value & 0xffu) != 1u)) {
                                    // A long running search can observe a
                                    // non-physics scheduler candidate. Since
                                    // search has performed no gameplay write,
                                    // discard the incomplete prefix and keep
                                    // the strict replay machine untouched.
                                    ++anchor_rejected_candidates;
                                    pending = {};
                                    pending_audit = false;
                                    pending_anchor_match = false;
                                    machine = Machine{Stage::kWaiting, 0, 1};
                                    cycle_tid = 0;
#endif
                                }
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
                                else if (!(pending.flags &
                                           kUnifiedCompletionWriteObserved)) {
                                    semantic_fault(
                                        "completion_write_not_observed", tid,
                                        dr6);
                                }
#else
                                else if (pending.completion_after ==
                                         pending.completion_before) {
                                    semantic_fault(
                                        "completion_unchanged_at_c9c", tid,
                                        dr6);
                                }
#endif
                                else if ((flags_value & 0xffu) != 1u) {
                                    semantic_fault(
                                        "callback_not_open_at_c9c", tid,
                                        dr6);
                                } else {
                                    pending.c9c_prefix_event =
                                        report.event_count;
                                    pending.callback_flags_at_c9c =
                                        flags_value;
                                    std::uint32_t prefix_actions = 0;
                                    if (!Advance(
                                            &machine,
                                            Event::kPrefixCertified, false,
                                            false, false, &prefix_actions)) {
                                        semantic_fault(
                                            "prefix_certificate_rejected",
                                            tid, dr6);
                                    } else {
                                        pending.flags |=
                                            kUnifiedPrefixCertified;
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
                                        if (!final_writer_runtime.installed &&
                                            !a9tas::final_writer_unified_v1::
                                                InstallAtCertifiedPrefix(
                                                    pid, mem, tid,
                                                    delta_address,
                                                    c98_address, c9c_address,
                                                    world_accumulator,
                                                    static_cast<std::uint32_t>(
                                                        machine.frame_index),
                                                    &threads,
                                                    &report.thread_additions,
                                                    &final_writer_runtime)) {
                                            semantic_fault(
                                                "final_writer_install_failed",
                                                tid, dr6);
                                        }
                                        // FreezeOthers may attach a newly
                                        // created thread and reallocate the
                                        // vector.  Never retain the old
                                        // TracedThread pointer afterward.
                                        tracked = FindThread(&threads, tid);
                                        if (tracked == nullptr)
                                            semantic_fault(
                                                "final_writer_owner_lost",
                                                tid, dr6);
#endif
                                    }
                                }
                                if (
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                                    cycle_tid != 0 &&
#endif
                                    report.read_errors == 0 &&
                                    report.semantic_errors == 0 &&
                                    !ProgramStoppedThread(
                                         tid, completion, callback_flags, f64,
                                         world_accumulator, PipelineDr7())) {
                                    ++report.ptrace_errors;
                                } else if (
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                                           cycle_tid != 0 &&
#endif
                                           report.read_errors == 0 &&
                                           report.semantic_errors == 0) {
                                    post_phase_tid = tid;
                                }
                            }
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
                            }
#endif
                        }
                    }
                }
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
                else if (hit == 8UL &&
                         machine.stage == Stage::kInputOpen &&
                         tid == cycle_tid) {
                    // The hardware trap proves that the game published a
                    // completion token even when the stored value is reused.
                    pending.flags |= kUnifiedCompletionWriteObserved;
                }
#endif
                else {
                    semantic_fault("unexpected_input_dr", tid, dr6);
                }
            }

            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
                ++report.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        } else {
            semantic_fault("unexpected_stop", tid, dr6);
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver))
                ++report.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        }
    }

#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    bool final_writer_final_ok = false;
    bool final_writer_payload_report_ok = false;
    const bool final_writer_cleanup_frozen =
        a9tas::final_writer_unified_v1::FreezeOthers(
            pid, 0, delta_address, c98_address, c9c_address,
            world_accumulator, &threads, &report.thread_additions);
    if (!final_writer_cleanup_frozen) ++report.ptrace_errors;
    if (final_writer_cleanup_frozen) {
        final_writer_final_ok =
            complete && a9tas::final_writer_unified_v1::ValidateFinal(
                            mem, &final_writer_runtime);
    }
    if (final_writer_final_ok) {
        final_writer_payload_report_ok =
            a9tas::final_writer_unified_v1::WritePayloadReport(
                pid, base, mem, &final_writer_runtime);
        if (!final_writer_payload_report_ok) ++report.semantic_errors;
    } else {
        // A failed thread-set freeze must never strand the installed shadow
        // vptr. ConditionalRollback verifies the complete hash-pinned control
        // identity and performs only the exact shadow -> original transition.
        ++report.rollback_attempts;
        if (!a9tas::final_writer_unified_v1::ConditionalRollback(
                mem, &final_writer_runtime))
            ++report.rollback_failures;
        ++report.semantic_errors;
    }
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
        report.flags |= kUnifiedCleanDetach;
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
    close(nitro_rpc);
#endif
    close(mem);

    const std::uint32_t required_flags =
        kUnifiedTargetVerified | kUnifiedIdentityVerified |
        kUnifiedRecordingValidated | kUnifiedCapabilityValidated |
        kUnifiedCleanDetach
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
        | kUnifiedNitroRpcUsed
#endif
#ifdef A9TAS_COMPLETION_WRITE_CERTIFICATE_V1
        | kUnifiedCompletionWriteCertificate
#endif
        ;
#if defined(A9TAS_NATURAL_PREROLL_REPLAY_V1) || \
    defined(A9TAS_UNIFIED_NITRO_RPC_V1) || \
    defined(A9TAS_FINAL_WRITER_REPLAY_V1)
    bool success =
#else
    const bool success =
#endif
#ifdef A9TAS_NATURAL_PREROLL_SEARCH_ONLY_V1
        complete && report.flags == required_flags &&
        report.processed_frames == 0 && audits.empty() &&
        report.delta_writes == 0 && report.control_writes == 0 &&
        report.equal_frames == 0 && report.corrected_frames == 0 &&
        report.skipped_frames == 0 && report.read_errors == 0 &&
        report.ptrace_errors == 0 && report.semantic_errors == 0 &&
        report.write_attempts == 0 && report.write_failures == 0 &&
        report.rollback_attempts == 0 && report.rollback_failures == 0;
#else
        complete && report.flags == required_flags &&
        report.processed_frames == report.recording_frames &&
        audits.size() == frames.size() &&
        report.equal_frames + report.corrected_frames +
                report.skipped_frames ==
            report.recording_frames &&
        report.delta_writes == report.recording_frames &&
        report.control_writes == expected_control_writes &&
        report.read_errors == 0 && report.ptrace_errors == 0 &&
        report.semantic_errors == 0 && report.write_failures == 0 &&
        report.write_attempts == report.corrected_frames &&
        report.rollback_attempts == 0 && report.rollback_failures == 0;
#endif
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    success = success && final_writer_final_ok &&
              final_writer_payload_report_ok;
#endif
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
    success = success &&
              report.nitro_rpc_requests == expected_nitro_rpc_requests &&
              report.nitro_activation_calls ==
                  expected_nitro_activation_calls &&
              report.nitro_rpc_failures == 0;
#endif
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
    success = success && !searching_anchor && anchor_search_cycles > 0 &&
              anchor_search_cycles == anchor_search_audits.size() &&
              matched_anchor_cycle < anchor_search_audits.size();
#endif
    bool report_ok = false;
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
#ifdef A9TAS_NATURAL_PREROLL_SEARCH_ONLY_V1
    if (success) report_ok = true;
#else
    if (success) report_ok = WriteUnifiedReport(argv[6], report, audits);
#endif
    bool search_report_ok = false;
    if (success && report_ok) {
        NaturalSearchReportHeaderV1 search_report{};
        std::memcpy(search_report.magic, kNaturalSearchReportMagic,
                    sizeof(search_report.magic));
        search_report.version = 1;
        search_report.header_size = sizeof(search_report);
        search_report.audit_size = sizeof(UnifiedFrameAuditV5);
        search_report.flags =
            kNaturalSearchTargetVerified |
            kNaturalSearchIdentityVerified |
            kNaturalSearchAnchorValidated |
            kNaturalSearchZeroGameplayWrites |
            kNaturalSearchMatched | kNaturalSearchCleanDetach;
        search_report.search_cycles =
            static_cast<std::uint32_t>(anchor_search_audits.size());
        search_report.matched_cycle_index = matched_anchor_cycle;
        std::memcpy(search_report.build_id,
                    a9tas::unified_tick_v1::kSupportedBuildId,
                    sizeof(search_report.build_id));
        search_report.pid = static_cast<std::uint64_t>(pid);
        search_report.library_base = base;
        search_report.main_object = main_object;
        search_report.final_owner = final_owner;
        search_report.physics_context = context;
        search_report.native_body = backend.native_body;
        search_report.event_count = report.event_count;
        search_report.thread_additions = report.thread_additions;
        search_report.initial_threads = report.initial_threads;
        search_report.final_threads = report.final_threads;
        search_report.fixed_interval_us =
            recording_header.fixed_interval_us;
        search_report.frame_count =
            static_cast<std::uint32_t>(frames.size());
        std::memcpy(search_report.source_recording_sha256,
                    natural_anchor.recording_sha256,
                    sizeof(search_report.source_recording_sha256));
        std::memcpy(search_report.source_report_sha256,
                    natural_anchor.report_sha256,
                    sizeof(search_report.source_report_sha256));
        search_report_ok = WriteNaturalSearchReport(
            argv[7], search_report, anchor_search_audits);
    }
    if (!report_ok || !search_report_ok) {
        std::remove(argv[6]);
        std::remove(argv[7]);
    }
#else
    if (success) report_ok = WriteUnifiedReport(argv[5], report, audits);
#endif
#ifdef A9TAS_FINAL_WRITER_REPLAY_V1
    if (!success || !report_ok)
        std::remove(g_a9tas_final_writer_payload_report_path_v1);
#endif
    std::printf(
        "UNIFIED_TICK_EXECUTOR_A9UER5_DONE complete=%u processed=%u/%u "
        "delta_writes=%" PRIu64 " control_writes=%" PRIu64
        " equal=%" PRIu64 " corrected=%" PRIu64 " skipped=%" PRIu64
        " read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
        " semantic_errors=%" PRIu64 " clean_detach=%u report=%s\n",
        complete ? 1u : 0u, report.processed_frames,
        report.recording_frames, report.delta_writes,
        report.control_writes, report.equal_frames, report.corrected_frames,
        report.skipped_frames, report.read_errors, report.ptrace_errors,
        report.semantic_errors,
        (report.flags & kUnifiedCleanDetach) != 0,
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
        argv[6]);
#else
        argv[5]);
#endif
#ifdef A9TAS_UNIFIED_NITRO_RPC_V1
    std::printf(
        "UNIFIED_NITRO_RPC_V1 requests=%" PRIu64
        " activation_calls=%" PRIu64 " failures=%" PRIu64
        " phase=after_fixed_delta_before_c98\n",
        report.nitro_rpc_requests, report.nitro_activation_calls,
        report.nitro_rpc_failures);
#endif
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
    std::printf(
        "NATURAL_PREROLL_REPLAY_V1 search_cycles=%" PRIu64
        " rejected_candidates=%" PRIu64
        " anchor_matched=%u search_gameplay_writes=0 search_report=%s\n",
        anchor_search_cycles, anchor_rejected_candidates,
        searching_anchor ? 0u : 1u, argv[7]);
#endif
#ifdef A9TAS_NATURAL_PREROLL_REPLAY_V1
    return success && report_ok && search_report_ok ? 0 : 8;
#else
    return success && report_ok ? 0 : 8;
#endif
}
#endif
