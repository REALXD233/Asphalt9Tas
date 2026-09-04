// BUILD-ONLY five-frame write-neutral binding observer.
//
// This candidate connects the source-faithful authoritative replay adapter to
// the live-proven HWBP phase boundaries without applying any replay value to
// the target. Target memory is opened O_RDONLY. The only target-side mutation
// is x86_64 debug-register programming required for hardware data breakpoints.
// Runtime deployment requires a separate, explicit user authorization.

#define A9TAS_PIPELINE_ORDER_NO_MAIN
#include "hwbp_pipeline_order_observer_v1.cpp"

#define A9TAS_AUTHORITATIVE_UNIFIED_ADAPTER_NO_MAIN
#include "authoritative_unified_adapter_v1.cpp"

#include <array>
#include <climits>
#include <cmath>

namespace {

using a9tas::authoritative_adapter_v1::AdapterEventV1;
using a9tas::authoritative_adapter_v1::AdapterStateV1;
using a9tas::authoritative_adapter_v1::AdvanceAdapter;
using a9tas::authoritative_adapter_v1::InitializeAdapter;
using a9tas::authoritative_tick_v1::TickInputStateV1;
using a9tas::unified_tick_v1::RecordingFrameV1;

constexpr char kNeutralMagic[8] = {'A', '9', 'A', 'N', 'O', '1', '\0', '\0'};
constexpr std::uint32_t kNeutralVersion = 1;
constexpr std::size_t kNeutralFrameCount = 5;
constexpr char kRequiredAcknowledgement[] =
    "I_ACCEPT_WRITE_NEUTRAL_FIVE_FRAME_OBSERVER_V1";

enum NeutralHeaderFlag : std::uint32_t {
    kNeutralTargetVerified = 1u << 0,
    kNeutralAddressesValidated = 1u << 1,
    kNeutralAdapterInitialized = 1u << 2,
    kNeutralFiveFramesBound = 1u << 3,
    kNeutralZeroGameplayWrites = 1u << 4,
    kNeutralCleanDetach = 1u << 5,
};

#pragma pack(push, 1)
struct NeutralFrameAuditV1 {
    std::uint32_t selected_tick;
    std::uint32_t published_tick;
    std::int32_t cycle_tid;
    std::int32_t commit_tid;
    std::int64_t observed_delta_us;
    std::uint64_t delta_event;
    std::uint64_t c98_event;
    std::uint64_t c9c_event;
    std::uint64_t prefix_event;
    std::uint64_t f64_event;
    std::uint64_t callback_close_event;
    std::uint64_t deferred_clear_event;
    std::uint64_t world_commit_event;
    std::uint64_t completion_before;
    std::uint64_t completion_after;
    std::uint16_t callback_flags_at_prefix;
    std::uint16_t reserved16;
    std::uint32_t phase_action_or;
};

struct NeutralReportHeaderV1 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t frame_audit_size;
    std::uint32_t flags;
    std::uint8_t build_id[20];
    std::uint32_t reserved0;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t main_object;
    std::uint64_t final_owner;
    std::uint64_t physics_context;
    std::uint64_t delta_address;
    std::uint64_t c98_address;
    std::uint64_t c9c_address;
    std::uint64_t completion_address;
    std::uint64_t callback_flags_address;
    std::uint64_t f64_address;
    std::uint64_t world_accumulator_address;
    std::uint64_t event_count;
    std::uint64_t read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t semantic_errors;
    std::uint64_t target_memory_write_attempts;
    std::uint64_t gameplay_action_calls;
    std::uint64_t thread_additions;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
    std::uint32_t fixed_interval_us;
    std::uint32_t bound_frames;
};
#pragma pack(pop)

static_assert(sizeof(NeutralFrameAuditV1) == 112,
              "neutral frame audit ABI");
static_assert(sizeof(NeutralReportHeaderV1) == 216,
              "neutral report header ABI");

bool ProgramStoppedThreadNeutral(pid_t tid, std::uintptr_t dr0,
                                 std::uintptr_t dr1, std::uintptr_t dr2,
                                 std::uintptr_t dr3, unsigned long dr7) {
    return PokeDebug(tid, 7, 0) && PokeDebug(tid, 0, dr0) &&
           PokeDebug(tid, 1, dr1) && PokeDebug(tid, 2, dr2) &&
           PokeDebug(tid, 3, dr3) && PokeDebug(tid, 6, 0) &&
           PokeDebug(tid, 7, dr7);
}

bool RearmPostOwnerForInput(pid_t current_tid, pid_t post_phase_tid,
                            std::vector<TracedThread>* threads,
                            std::uintptr_t delta_address,
                            std::uintptr_t c98_address,
                            std::uintptr_t c9c_address,
                            std::uintptr_t world_accumulator) {
    if (post_phase_tid <= 0) return false;
    if (post_phase_tid == current_tid) {
        return ProgramStoppedThreadNeutral(
            current_tid, delta_address, c98_address, c9c_address,
            world_accumulator, BoundaryDr7(true));
    }
    TracedThread* owner = FindThread(threads, post_phase_tid);
    if (!owner || !owner->live) return false;
    if (!owner->stopped && !StopThread(post_phase_tid)) return false;
    owner->stopped = true;
    if (!ProgramStoppedThreadNeutral(
            post_phase_tid, delta_address, c98_address, c9c_address,
            world_accumulator, BoundaryDr7(true))) {
        return false;
    }
    if (!ContinueThread(post_phase_tid)) return false;
    owner->stopped = false;
    return true;
}

bool WriteNeutralReport(const char* path, const NeutralReportHeaderV1& header,
                        const std::array<NeutralFrameAuditV1,
                                         kNeutralFrameCount>& audits) {
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
        std::fwrite(audits.data(), sizeof(audits.front()), audits.size(), file) ==
            audits.size() &&
        std::fflush(file) == 0 && std::ferror(file) == 0;
    const bool close_ok = std::fclose(file) == 0;
    if (!ok || !close_ok) {
        std::remove(path);
        return false;
    }
    return true;
}

bool FeedNeutral(AdapterStateV1* adapter, AdapterEventV1 event,
                 NeutralFrameAuditV1* audit, bool comparator_available = false,
                 const TickInputStateV1* consumed = nullptr,
                 bool* committed = nullptr, bool* complete = nullptr) {
    const auto result = AdvanceAdapter(adapter, event, comparator_available,
                                       false, consumed);
    if (!result.accepted) return false;
    if (audit) audit->phase_action_or |= result.phase_actions;
    if (committed) *committed = result.committed;
    if (complete) *complete = result.complete;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 7 || argc > 10) {
        std::fprintf(
            stderr,
            "usage: %s PID LIB_BASE_HEX TIMEOUT_MS REPORT_PATH "
            "ACKNOWLEDGEMENT [PHYSICS_CONTEXT_HEX] [MAIN_OBJECT_HEX] "
            "[FINAL_OWNER_HEX]\n",
            argv[0]);
        return 2;
    }
    if (std::strcmp(argv[5], kRequiredAcknowledgement) != 0) {
        std::fprintf(stderr,
                     "neutral-observer acknowledgement missing; no process "
                     "opened\n");
        return 2;
    }
    if (access(argv[4], F_OK) == 0) {
        std::fprintf(stderr,
                     "report path already exists; refusing to overwrite it\n");
        return 2;
    }

    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t timeout_value = 0;
    std::uint64_t context_value = 0;
    std::uint64_t main_object_value = 0;
    std::uint64_t final_owner_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &timeout_value) ||
        (argc >= 8 && !ParseUnsigned(argv[7], 16, &context_value)) ||
        (argc >= 9 && !ParseUnsigned(argv[8], 16, &main_object_value)) ||
        (argc == 10 && !ParseUnsigned(argv[9], 16, &final_owner_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || timeout_value < 1000 || timeout_value > 300000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }

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
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 4;

    std::uintptr_t context = 0;
    std::uintptr_t adapter_object = 0;
    std::uintptr_t world = 0;
    a9tas::vehicle_state_v1::Layout vehicle{};
    if (!ResolvePhysicsContext(pid, mem, base,
                               static_cast<std::uintptr_t>(context_value),
                               &context, &adapter_object, &world) ||
        !a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle)) {
        std::fprintf(stderr, "neutral object resolution failed\n");
        close(mem);
        return 3;
    }

    const std::uintptr_t delta_address = main_object + kAccumulatorOffset;
    const std::uintptr_t c98_address = final_owner + kC98Offset;
    const std::uintptr_t c9c_address = final_owner + kC9COffset;
    const std::uintptr_t completion =
        context + kContextCompletionTokenOffset;
    const std::uintptr_t callback_flags = context + kContextCallbackFlagsOffset;
    const std::uintptr_t f64 = vehicle.physics_base + kCarPhysicsF64Offset;
    const std::uintptr_t world_accumulator = world + kWorldAccumulatorOffset;

    std::vector<Mapping> maps;
    float fixed_interval_seconds = 0.0f;
    if ((delta_address & 7u) != 0 || (c98_address & 7u) != 0 ||
        c9c_address != c98_address + 4 || !ReadMaps(pid, &maps) ||
        !PipelineWritable(maps, delta_address, 8) ||
        !PipelineWritable(maps, c98_address, 8) ||
        !PipelineWritable(maps, completion, 8) ||
        !PipelineWritable(maps, callback_flags, 2) ||
        !PipelineWritable(maps, f64, 4) ||
        !PipelineWritable(maps, world_accumulator, 4) ||
        !ReadExact(mem, context + kContextFixedIntervalOffset,
                   &fixed_interval_seconds, sizeof(fixed_interval_seconds)) ||
        !std::isfinite(fixed_interval_seconds) ||
        fixed_interval_seconds <= 0.0f || fixed_interval_seconds > 1.0f) {
        std::fprintf(stderr, "neutral address validation failed\n");
        close(mem);
        return 3;
    }
    const auto fixed_interval_us = static_cast<std::uint32_t>(
        std::llround(static_cast<double>(fixed_interval_seconds) * 1000000.0));
    if (fixed_interval_us < a9tas::unified_tick_v1::kMinimumFixedIntervalUs ||
        fixed_interval_us > a9tas::unified_tick_v1::kMaximumFixedIntervalUs) {
        std::fprintf(stderr, "fixed interval outside recording ABI\n");
        close(mem);
        return 3;
    }

    std::array<RecordingFrameV1, kNeutralFrameCount> frames{};
    for (std::size_t index = 0; index < frames.size(); ++index) {
        frames[index].tick = index;
        frames[index].monotonic_ns =
            static_cast<std::uint64_t>(index) * fixed_interval_us * 1000ULL;
        frames[index].skip_override_flags =
            a9tas::unified_tick_v1::kSupportedSkipMask;
    }
    AdapterStateV1 authoritative{};
    if (!InitializeAdapter(&authoritative, frames.data(), frames.size(),
                           fixed_interval_us, frames.size() - 1)) {
        std::fprintf(stderr, "authoritative adapter initialization failed\n");
        close(mem);
        return 3;
    }

    NeutralReportHeaderV1 report{};
    std::memcpy(report.magic, kNeutralMagic, sizeof(report.magic));
    report.version = kNeutralVersion;
    report.header_size = sizeof(report);
    report.frame_audit_size = sizeof(NeutralFrameAuditV1);
    report.flags = kNeutralTargetVerified | kNeutralAddressesValidated |
                   kNeutralAdapterInitialized | kNeutralZeroGameplayWrites;
    std::memcpy(report.build_id, a9tas::unified_tick_v1::kSupportedBuildId,
                sizeof(report.build_id));
    report.pid = static_cast<std::uint64_t>(pid);
    report.library_base = base;
    report.main_object = main_object;
    report.final_owner = final_owner;
    report.physics_context = context;
    report.delta_address = delta_address;
    report.c98_address = c98_address;
    report.c9c_address = c9c_address;
    report.completion_address = completion;
    report.callback_flags_address = callback_flags;
    report.f64_address = f64;
    report.world_accumulator_address = world_accumulator;
    report.fixed_interval_us = fixed_interval_us;

    std::vector<TracedThread> threads;
    std::uint64_t attach_failures = 0;
    const std::size_t initial = AttachNewThreads(
        pid, delta_address, c98_address, c9c_address, world_accumulator,
        &threads, &attach_failures, BoundaryDr7(true));
    report.initial_threads = static_cast<std::uint32_t>(initial);
    report.ptrace_errors += attach_failures;
    if (initial == 0 || attach_failures != 0) {
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        close(mem);
        return 7;
    }

    std::printf(
        "AUTHORITATIVE_NEUTRAL_OBSERVER_V1_BUILD_ONLY pid=%d frames=%zu "
        "fixed_us=%u target_memory=read_only gameplay_writes=0\n",
        static_cast<int>(pid), frames.size(), fixed_interval_us);
    std::fflush(stdout);

    std::array<NeutralFrameAuditV1, kNeutralFrameCount> audits{};
    NeutralFrameAuditV1 pending{};
    pid_t cycle_tid = 0;
    pid_t post_phase_tid = 0;
    std::uint32_t accounted_threads = 0;
    bool complete = false;
    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + timeout_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 250000000ULL;

    const auto semantic_fault = [&](const char* reason, pid_t tid,
                                    unsigned long dr6) {
        std::fprintf(stderr,
                     "neutral_fault reason=%s event=%" PRIu64
                     " frame=%zu stage=%u dr6=0x%lx tid=%d owner=%d\n",
                     reason, report.event_count, authoritative.phase.frame_index,
                     static_cast<unsigned>(authoritative.phase.stage), dr6,
                     static_cast<int>(tid), static_cast<int>(cycle_tid));
        ++report.semantic_errors;
    };

    while (MonotonicNs() < deadline_ns && report.read_errors == 0 &&
           report.ptrace_errors == 0 && report.semantic_errors == 0 &&
           !complete) {
        const std::uint64_t now_ns = MonotonicNs();
        if (now_ns >= next_rescan_ns) {
            std::uint64_t failures = 0;
            const std::size_t added = AttachNewThreads(
                pid, delta_address, c98_address, c9c_address,
                world_accumulator, &threads, &failures, BoundaryDr7(true));
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
                if (tid == cycle_tid) semantic_fault("owner_exited", tid, 0);
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
                       authoritative.phase.stage == Stage::kWaitWorldCommit) {
                std::uint32_t accumulator_bits = 0;
                float accumulator_value = 0.0f;
                if (!ReadExact(mem, world_accumulator, &accumulator_bits,
                               sizeof(accumulator_bits))) {
                    ++report.read_errors;
                } else {
                    std::memcpy(&accumulator_value, &accumulator_bits,
                                sizeof(accumulator_value));
                    TickInputStateV1 consumed{};
                    consumed.race_tick = pending.selected_tick;
                    bool committed = false;
                    bool adapter_complete = false;
                    if (!std::isfinite(accumulator_value) ||
                        std::fabs(accumulator_value) > 60.0f ||
                        tid == cycle_tid || pending.deferred_clear_event == 0 ||
                        !FeedNeutral(&authoritative,
                                     AdapterEventV1::kWorldCommit, &pending,
                                     false, &consumed, &committed,
                                     &adapter_complete) ||
                        !committed) {
                        semantic_fault("unexpected_world_commit", tid, dr6);
                    } else {
                        pending.commit_tid = static_cast<std::int32_t>(tid);
                        pending.world_commit_event = report.event_count;
                        pending.published_tick =
                            authoritative.next_input.race_tick - 1;
                        const std::size_t audit_index =
                            authoritative.committed_frames - 1;
                        if (audit_index >= audits.size()) {
                            semantic_fault("audit_index_overflow", tid, dr6);
                        } else {
                            audits[audit_index] = pending;
                            report.bound_frames =
                                authoritative.committed_frames;
                            if (adapter_complete) {
                                complete = true;
                            } else if (!RearmPostOwnerForInput(
                                           tid, post_phase_tid, &threads,
                                           delta_address, c98_address,
                                           c9c_address, world_accumulator)) {
                                ++report.ptrace_errors;
                            } else {
                                pending = {};
                                post_phase_tid = 0;
                                cycle_tid = 0;
                            }
                        }
                    }
                }
            } else if (hit == 8UL &&
                       authoritative.phase.stage == Stage::kWaiting) {
                // Independent world commits are irrelevant without an open
                // authoritative frame.
            } else if (tid == post_phase_tid) {
                if (tid != cycle_tid) {
                    semantic_fault("post_phase_wrong_owner", tid, dr6);
                } else if (hit == 2UL) {
                    std::uint16_t flags_value = 0;
                    if (!ReadExact(mem, callback_flags, &flags_value,
                                   sizeof(flags_value))) {
                        ++report.read_errors;
                    } else {
                        const std::uint8_t dispatching =
                            static_cast<std::uint8_t>(flags_value & 0xffu);
                        if (dispatching == 0 &&
                            authoritative.phase.stage ==
                                Stage::kWaitCallbackClose) {
                            if (!FeedNeutral(
                                    &authoritative,
                                    AdapterEventV1::kCallbackClose, &pending,
                                    true)) {
                                semantic_fault("callback_close_rejected", tid,
                                               dr6);
                            } else {
                                pending.callback_close_event =
                                    report.event_count;
                            }
                        } else if (
                            dispatching == 0 &&
                            authoritative.phase.stage ==
                                Stage::kWaitDeferredClear) {
                            if (!FeedNeutral(
                                    &authoritative,
                                    AdapterEventV1::kDeferredCallbackClear,
                                    &pending)) {
                                semantic_fault("deferred_clear_rejected", tid,
                                               dr6);
                            } else {
                                pending.deferred_clear_event =
                                    report.event_count;
                            }
                        } else {
                            semantic_fault("unexpected_callback", tid, dr6);
                        }
                    }
                } else if (hit == 4UL) {
                    std::uint32_t f64_bits = 0;
                    float f64_value = 0.0f;
                    if (!ReadExact(mem, f64, &f64_bits, sizeof(f64_bits))) {
                        ++report.read_errors;
                    } else {
                        std::memcpy(&f64_value, &f64_bits, sizeof(f64_value));
                        if (!std::isfinite(f64_value) ||
                            std::fabs(f64_value) > 1000000.0f ||
                            !FeedNeutral(&authoritative,
                                         AdapterEventV1::kF64, &pending)) {
                            semantic_fault("unexpected_f64", tid, dr6);
                        } else {
                            pending.f64_event = report.event_count;
                        }
                    }
                } else {
                    semantic_fault("unexpected_post_phase_dr", tid, dr6);
                }
            } else if (hit == 1UL) {
                std::int64_t delta_us = 0;
                if (!ReadExact(mem, delta_address, &delta_us,
                               sizeof(delta_us))) {
                    ++report.read_errors;
                } else if (authoritative.phase.stage == Stage::kWaiting &&
                           delta_us == 0) {
                    if (!FeedNeutral(&authoritative,
                                     AdapterEventV1::kDeltaZero, nullptr)) {
                        semantic_fault("waiting_zero_rejected", tid, dr6);
                    }
                } else if (authoritative.phase.stage == Stage::kWaiting &&
                           delta_us > 0 && delta_us <= 1000000) {
                    const auto result = AdvanceAdapter(
                        &authoritative, AdapterEventV1::kDeltaNonzero, false,
                        false, nullptr);
                    if (!result.accepted || !result.packet_selected ||
                        result.selected_packet.tick !=
                            authoritative.phase.frame_index) {
                        semantic_fault("delta_open_rejected", tid, dr6);
                    } else {
                        cycle_tid = tid;
                        pending = {};
                        pending.selected_tick = static_cast<std::uint32_t>(
                            result.selected_packet.tick);
                        pending.cycle_tid = static_cast<std::int32_t>(tid);
                        pending.observed_delta_us = delta_us;
                        pending.delta_event = report.event_count;
                        pending.phase_action_or = result.phase_actions;
                        if (!ReadExact(mem, completion,
                                       &pending.completion_before,
                                       sizeof(pending.completion_before))) {
                            ++report.read_errors;
                        }
                    }
                } else if (authoritative.phase.stage == Stage::kInputOpen &&
                           tid == cycle_tid && delta_us == 0) {
                    const auto result = AdvanceAdapter(
                        &authoritative, AdapterEventV1::kDeltaZero, false,
                        false, nullptr);
                    if (!result.accepted || !result.selection_rolled_back) {
                        semantic_fault("pause_rollback_rejected", tid, dr6);
                    } else {
                        pending = {};
                        cycle_tid = 0;
                    }
                } else {
                    semantic_fault("unexpected_delta", tid, dr6);
                }
            } else if (hit == 2UL || hit == 4UL) {
                if (authoritative.phase.stage == Stage::kWaiting) {
                    // Ignore an incomplete prefix at initial attach or on a
                    // newly-created thread.
                } else if (tid != cycle_tid) {
                    semantic_fault("input_cross_thread", tid, dr6);
                } else {
                    const AdapterEventV1 event =
                        hit == 2UL ? AdapterEventV1::kC98
                                   : AdapterEventV1::kC9C;
                    if (!FeedNeutral(&authoritative, event, &pending)) {
                        semantic_fault("input_order", tid, dr6);
                    } else if (hit == 2UL) {
                        pending.c98_event = report.event_count;
                    } else {
                        pending.c9c_event = report.event_count;
                        std::uint16_t flags_value = 0;
                        if (!ReadExact(mem, completion,
                                       &pending.completion_after,
                                       sizeof(pending.completion_after)) ||
                            !ReadExact(mem, callback_flags, &flags_value,
                                       sizeof(flags_value))) {
                            ++report.read_errors;
                        } else if (pending.completion_after ==
                                   pending.completion_before) {
                            semantic_fault("completion_unchanged_at_c9c", tid,
                                           dr6);
                        } else if ((flags_value & 0xffu) != 1u) {
                            semantic_fault("callback_not_open_at_c9c", tid,
                                           dr6);
                        } else if (!FeedNeutral(
                                       &authoritative,
                                       AdapterEventV1::kPrefixCertified,
                                       &pending)) {
                            semantic_fault("prefix_rejected", tid, dr6);
                        } else {
                            pending.prefix_event = report.event_count;
                            pending.callback_flags_at_prefix = flags_value;
                            if (!ProgramStoppedThreadNeutral(
                                    tid, completion, callback_flags, f64,
                                    world_accumulator, PipelineDr7())) {
                                ++report.ptrace_errors;
                            } else {
                                post_phase_tid = tid;
                            }
                        }
                    }
                }
            } else {
                semantic_fault("unexpected_input_dr", tid, dr6);
            }

            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid)) {
                ++report.ptrace_errors;
            } else if (tracked) {
                tracked->stopped = false;
            }
        } else {
            semantic_fault("unexpected_stop", tid, dr6);
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver)) {
                ++report.ptrace_errors;
            } else if (tracked) {
                tracked->stopped = false;
            }
        }
    }

    for (auto& thread : threads) {
        if (!thread.live) continue;
        if (!ClearAndDetach(thread.tid, thread.stopped)) {
            ++report.ptrace_errors;
        } else {
            ++accounted_threads;
        }
        thread.live = false;
        thread.stopped = false;
    }
    report.final_threads = accounted_threads;
    if (report.final_threads ==
        report.initial_threads + report.thread_additions) {
        report.flags |= kNeutralCleanDetach;
    }
    close(mem);

    const std::uint32_t required_flags =
        kNeutralTargetVerified | kNeutralAddressesValidated |
        kNeutralAdapterInitialized | kNeutralFiveFramesBound |
        kNeutralZeroGameplayWrites | kNeutralCleanDetach;
    if (complete && report.bound_frames == kNeutralFrameCount)
        report.flags |= kNeutralFiveFramesBound;
    const bool success =
        complete && report.flags == required_flags &&
        report.bound_frames == kNeutralFrameCount &&
        authoritative.committed_frames == kNeutralFrameCount &&
        authoritative.phase.stage == Stage::kComplete &&
        report.read_errors == 0 && report.ptrace_errors == 0 &&
        report.semantic_errors == 0 &&
        report.target_memory_write_attempts == 0 &&
        report.gameplay_action_calls == 0;
    const bool report_ok = success && WriteNeutralReport(argv[4], report, audits);
    std::printf(
        "AUTHORITATIVE_NEUTRAL_OBSERVER_V1_DONE complete=%u bound=%u/%zu "
        "target_memory_write_attempts=%" PRIu64
        " gameplay_action_calls=%" PRIu64
        " read_errors=%" PRIu64 " ptrace_errors=%" PRIu64
        " semantic_errors=%" PRIu64 " clean_detach=%u report=%s\n",
        complete ? 1u : 0u, report.bound_frames, kNeutralFrameCount,
        report.target_memory_write_attempts, report.gameplay_action_calls,
        report.read_errors, report.ptrace_errors, report.semantic_errors,
        (report.flags & kNeutralCleanDetach) != 0, argv[4]);
    return success && report_ok ? 0 : 8;
}
