// PARTIAL MAINLINE / DIAGNOSTIC STATE TRACE — NOT FINAL PHYSICS CORRECTION.
//
// The input scheduler remains useful, but A9PST1 position/quaternion/velocity
// output is only a diagnostic mirror trace.  It must not be used as the source
// for AluTasV2's exact final correction, which records native_body+0x10 (64
// bytes) and native_body+0x150 (12 bytes) as raw float bits.  Its historical
// F64 completion gate is likewise superseded by the still-open four-address
// pipeline Gate 2.  See src/native_physics_recording_v1.h and
// evidence/EXPERIMENT_QUARANTINE_AND_MAINLINE_20260817.md.
//
// Houdini-safe replay controller anchored to the proven physics-time boundary.
//
// DR0 observes MainLoop+0x150. A non-zero write is PRE_PHYSICS and the zero
// reset is POST_PHYSICS. DR1/DR2 observe the native C98/C9C setters inside the
// same synchronous interval. When state tracing is requested, DR3 observes the
// final angular-velocity component (F64), the proven point at which pose and all
// six velocity components have been published. This is an observation-only
// completion gate; AluTasV2's final correction payload does not include angular
// velocity. A replay frame is selected at
// PRE_PHYSICS and committed only after C98 -> C9C -> F64 -> POST_PHYSICS.
// Paused outer-frame cycles contain no control/state sequence and never advance.
//
// No guest instruction is patched and translated code is never single-stepped.

#define main A9TasSchedulerObserverMain_NotUsed
#include "hwbp_scheduler_observer_v1.cpp"
#undef main

#include "vehicle_state_resolver_v1.h"

namespace {

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

struct StateTraceHeaderV1 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t frame_size;
    std::uint32_t flags;
    std::uint8_t build_id[20];
    std::uint32_t reserved0;
    std::uint64_t pid;
    std::uint64_t library_base;
    std::uint64_t main_object;
    std::uint64_t final_owner;
    std::uint64_t physics_base;
    std::uint64_t position_address;
    std::uint64_t rotation_address;
    std::uint64_t linear_address;
    std::uint64_t angular_address;
    std::uint64_t start_ns;
    std::uint64_t target_frames;
    std::uint64_t state_frames;
    std::uint64_t state_read_errors;
    std::uint64_t state_write_errors;
    std::int64_t fixed_interval_us;
    std::uint8_t reserved[32];
};

struct StateTraceFrameV1 {
    std::uint64_t tick;
    std::uint64_t monotonic_ns;
    std::int64_t original_interval_us;
    std::int64_t applied_interval_us;
    float position[3];
    float rotation[4];
    float linear[3];
    float angular[3];
    float c98;
    float c9c;
    std::uint32_t flags;
    std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(ReplayHeaderV1) == 64, "replay header ABI");
static_assert(sizeof(ReplayFrameV1) == 16, "replay frame ABI");
static_assert(sizeof(StateTraceFrameV1) == 100, "state frame ABI");

bool LoadReplay(const char* path, std::vector<ReplayFrameV1>* frames) {
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
    const bool body_ok =
        std::fread(frames->data(), sizeof(ReplayFrameV1), frames->size(),
                   file) == frames->size() &&
        std::fgetc(file) == EOF;
    std::fclose(file);
    if (!body_ok) {
        std::fprintf(stderr, "invalid A9SPR1 length\n");
        return false;
    }
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
    std::uint32_t c98 = 0;
    std::uint32_t c9c = 0;
    std::memcpy(&c98, &frame.longitudinal, sizeof(c98));
    std::memcpy(&c9c, &frame.steering, sizeof(c9c));
    return static_cast<std::uint64_t>(c98) |
           (static_cast<std::uint64_t>(c9c) << 32);
}

bool WriteExactVerified(int mem, std::uintptr_t address, const void* input,
                        std::size_t size) {
    if (pwrite(mem, input, size, static_cast<off_t>(address)) !=
        static_cast<ssize_t>(size))
        return false;
    std::uint64_t verify = 0;
    if (size > sizeof(verify) ||
        !ReadExact(mem, address, &verify, size))
        return false;
    return std::memcmp(&verify, input, size) == 0;
}

void CleanupReplayThreads(std::vector<TracedThread>* threads,
                          std::uint64_t* errors) {
    for (auto& thread : *threads) {
        if (!thread.live) continue;
        if (!ClearAndDetach(thread.tid, thread.stopped)) ++*errors;
        thread.live = false;
        thread.stopped = false;
    }
}

bool FinalOwnerIdentityAlive(int mem, std::uintptr_t base,
                             std::uintptr_t owner) {
    const std::uintptr_t inner_vtable = base + kInnerVtableRva;
    std::int64_t adjustment = 0;
    if (!ReadExact(mem, inner_vtable - kInnerAdjustSlotDelta, &adjustment,
                   sizeof(adjustment)))
        return false;
    const auto signed_inner = static_cast<std::intptr_t>(owner) - adjustment;
    if (signed_inner <= 0) return false;
    std::uintptr_t actual_vtable = 0;
    return ReadExact(mem, static_cast<std::uintptr_t>(signed_inner),
                     &actual_vtable, sizeof(actual_vtable)) &&
           actual_vtable == inner_vtable;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || argc > 10) {
        std::fprintf(
            stderr,
            "usage: %s PID LIB_BASE_HEX REPLAY_PATH TIMEOUT_MS "
            "[MAIN_OBJECT_HEX] [FINAL_OWNER_HEX] [FIXED_INTERVAL_US] "
            "[STATE_OUT_PATH] [WAIT_OWNER_CHANGE_0_OR_1]\n",
            argv[0]);
        return 2;
    }

    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t timeout_value = 0;
    std::uint64_t object_value = 0;
    std::uint64_t owner_value = 0;
    std::uint64_t fixed_interval_value = 0;
    std::uint64_t wait_owner_change_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[4], 10, &timeout_value) ||
        (argc >= 6 && !ParseUnsigned(argv[5], 16, &object_value)) ||
        (argc >= 7 && !ParseUnsigned(argv[6], 16, &owner_value)) ||
        (argc >= 8 &&
         !ParseUnsigned(argv[7], 10, &fixed_interval_value)) ||
        (argc == 10 &&
         !ParseUnsigned(argv[9], 10, &wait_owner_change_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || timeout_value < 1000 || timeout_value > 600000 ||
        (fixed_interval_value != 0 &&
         (fixed_interval_value < 1000 || fixed_interval_value > 100000)) ||
        wait_owner_change_value > 1) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }

    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto explicit_object = static_cast<std::uintptr_t>(object_value);
    const auto explicit_owner = static_cast<std::uintptr_t>(owner_value);
    const auto timeout_ms = static_cast<std::uint64_t>(timeout_value);
    const auto fixed_interval_us =
        static_cast<std::int64_t>(fixed_interval_value);
    const char* state_output_path = argc == 9 ? argv[8] : nullptr;
    if (argc == 10) state_output_path = argv[8];
    const bool wait_owner_change = wait_owner_change_value == 1;

    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported build; no thread was attached\n");
        return 3;
    }
    std::vector<ReplayFrameV1> frames;
    if (!LoadReplay(argv[3], &frames)) return 3;

    std::uintptr_t main_object = 0;
    if (!ResolveMainObject(pid, base, explicit_object, &main_object)) return 3;
    std::uintptr_t final_owner = 0;
    if (!ResolveFinalOwner(pid, base, explicit_owner, &final_owner)) return 3;
    const std::uintptr_t accumulator_address = main_object + kAccumulatorOffset;
    std::uintptr_t c98_address = final_owner + kC98Offset;
    std::uintptr_t c9c_address = final_owner + kC9COffset;
    if ((accumulator_address & 7u) != 0 || (c98_address & 7u) != 0 ||
        c9c_address != c98_address + 4) {
        std::fprintf(stderr, "unaligned boundary/control address\n");
        return 3;
    }

    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem < 0) {
        std::perror("open mem for scheduler replay");
        return 4;
    }

    FILE* state_out = nullptr;
    StateTraceHeaderV1 state_header{};
    a9tas::vehicle_state_v1::Layout state_layout{};
    const bool state_complete_gate = state_output_path != nullptr;
    std::uintptr_t state_watch_address = 0;
    if (state_output_path) {
        std::uint64_t scanned_bytes = 0;
        if (!a9tas::vehicle_state_v1::Resolve(
                pid, mem, base, &state_layout, &scanned_bytes)) {
            std::fprintf(stderr, "vehicle state resolver failed closed\n");
            close(mem);
            return 4;
        }
        if (state_layout.angular_address > UINTPTR_MAX - 2 * sizeof(float)) {
            std::fprintf(stderr, "state-complete watch address overflow\n");
            close(mem);
            return 4;
        }
        state_watch_address =
            state_layout.angular_address + 2 * sizeof(float);
        if ((state_watch_address & (sizeof(float) - 1)) != 0) {
            std::fprintf(stderr, "unaligned state-complete watch address\n");
            close(mem);
            return 4;
        }
        state_out = std::fopen(state_output_path, "wb");
        if (!state_out) {
            std::perror("open state trace");
            close(mem);
            return 4;
        }
        std::memcpy(state_header.magic, "A9PST1\0\0", 8);
        state_header.version = 1;
        state_header.header_size = sizeof(state_header);
        state_header.frame_size = sizeof(StateTraceFrameV1);
        if (wait_owner_change) state_header.flags |= 2u;
        state_header.flags |= 4u;
        std::memcpy(state_header.build_id, kReplayBuildId,
                    sizeof(kReplayBuildId));
        state_header.pid = static_cast<std::uint64_t>(pid);
        state_header.library_base = base;
        state_header.main_object = main_object;
        state_header.final_owner = final_owner;
        state_header.physics_base = state_layout.physics_base;
        state_header.position_address = state_layout.position_address;
        state_header.rotation_address = state_layout.rotation_address;
        state_header.linear_address = state_layout.linear_address;
        state_header.angular_address = state_layout.angular_address;
        state_header.target_frames = frames.size();
        state_header.fixed_interval_us = fixed_interval_us;
        if (std::fwrite(&state_header, sizeof(state_header), 1, state_out) !=
            1) {
            std::fclose(state_out);
            close(mem);
            return 4;
        }
        std::printf(
            "HWBP_SCHED_REPLAY_V1_STATE_RESOLVED scanned=%" PRIu64
            " physics=0x%" PRIxPTR " position=0x%" PRIxPTR
            " rotation=0x%" PRIxPTR " linear=0x%" PRIxPTR
            " angular=0x%" PRIxPTR " state_complete=0x%" PRIxPTR "\n",
            scanned_bytes, state_layout.physics_base,
            state_layout.position_address, state_layout.rotation_address,
            state_layout.linear_address, state_layout.angular_address,
            state_watch_address);
    }

    std::vector<TracedThread> threads;
    std::uint64_t attach_errors = 0;
    const unsigned long initial_dr7 =
        wait_owner_change ? AccumulatorOnlyDr7()
                          : BoundaryDr7(state_complete_gate);
    const std::size_t initial = AttachNewThreads(
        pid, accumulator_address, c98_address, c9c_address,
        state_watch_address, &threads, &attach_errors, initial_dr7);
    if (initial == 0 || attach_errors != 0) {
        std::fprintf(stderr,
                     "initial attach failed threads=%zu errors=%" PRIu64 "\n",
                     initial, attach_errors);
        CleanupReplayThreads(&threads, &attach_errors);
        if (state_out) std::fclose(state_out);
        close(mem);
        return 5;
    }

    std::printf(
        "HWBP_SCHED_REPLAY_V1_ARMED pid=%d object=0x%" PRIxPTR
        " owner=0x%" PRIxPTR " frames=%zu threads=%zu fixed_us=%" PRId64
        " wait_owner_change=%u state_complete_gate=%u "
        "write_scope=C98_C9C%s single_step=none\n",
        static_cast<int>(pid), main_object, final_owner, frames.size(),
        threads.size(), fixed_interval_us, wait_owner_change ? 1u : 0u,
        state_complete_gate ? 1u : 0u,
        fixed_interval_us ? "+ACCUMULATOR" : "");
    std::fflush(stdout);

    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + timeout_ms * 1000000ULL;
    const std::uint64_t first_now_ns = MonotonicNs();
    if (start_ns == 0 || first_now_ns == 0 || deadline_ns <= start_ns) {
        std::fprintf(stderr,
                     "invalid monotonic clock start=%" PRIu64
                     " now=%" PRIu64 " deadline=%" PRIu64 "\n",
                     start_ns, first_now_ns, deadline_ns);
        std::uint64_t cleanup_errors = 0;
        CleanupReplayThreads(&threads, &cleanup_errors);
        if (state_out) std::fclose(state_out);
        close(mem);
        return 5;
    }
    std::printf("HWBP_SCHED_REPLAY_V1_CLOCK start=%" PRIu64
                " now=%" PRIu64 " deadline=%" PRIu64 " timeout_ms=%" PRIu64
                "\n",
                start_ns, first_now_ns, deadline_ns, timeout_ms);
    std::fflush(stdout);
    std::uint64_t next_rescan_ns = start_ns + 250000000ULL;
    std::size_t tick = 0;
    std::uint64_t events = 0;
    std::uint64_t outer_cycles = 0;
    std::uint64_t paused_cycles = 0;
    std::uint64_t write_errors = 0;
    std::uint64_t ptrace_errors = 0;
    std::uint64_t unexpected_stops = 0;
    std::uint64_t sequence_errors = 0;
    std::uint64_t state_frames = 0;
    std::uint64_t state_complete_hits = 0;
    std::uint64_t owner_resolve_attempts = 0;
    std::uint64_t owner_gate_ignored_control_events = 0;
    std::uint64_t next_owner_resolve_ns = 0;
    std::int64_t cycle_original_interval_us = 0;
    std::int64_t cycle_applied_interval_us = 0;
    bool in_cycle = false;
    bool waiting_for_owner_change = wait_owner_change;
    const std::uintptr_t initial_final_owner = final_owner;
    bool saw_initial_owner_dead = false;
    bool owner_address_reused = false;
    pid_t owner_control_tid = 0;
    bool saw_c98 = false;
    bool saw_c9c = false;
    bool saw_state_complete = false;
    pid_t cycle_tid = 0;
    bool failed = false;

    while (tick < frames.size() && MonotonicNs() < deadline_ns) {
        const std::uint64_t now_ns = MonotonicNs();
        if (now_ns >= next_rescan_ns) {
            std::uint64_t failures = 0;
            const unsigned long rescan_dr7 =
                waiting_for_owner_change ? AccumulatorOnlyDr7()
                                         : BoundaryDr7(state_complete_gate);
            AttachNewThreads(pid, accumulator_address, c98_address,
                             c9c_address, state_watch_address, &threads,
                             &failures, rescan_dr7);
            attach_errors += failures;
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
            ++ptrace_errors;
            failed = true;
            break;
        }
        TracedThread* tracked = FindThread(&threads, tid);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (tracked) {
                tracked->live = false;
                tracked->stopped = false;
            }
            if (in_cycle && tid == cycle_tid) {
                ++sequence_errors;
                failed = true;
                break;
            }
            continue;
        }
        if (!WIFSTOPPED(status)) continue;
        if (tracked) tracked->stopped = true;
        const int signal = WSTOPSIG(status);
        unsigned long dr6 = 0;
        const bool have_dr6 = PeekDebug(tid, 6, &dr6);
        const unsigned long hit_mask = state_complete_gate ? 15UL : 7UL;
        unsigned long hit = dr6 & hit_mask;
        if (signal == SIGTRAP && have_dr6 && hit != 0) {
            ++events;
            std::int64_t accumulator = 0;
            if (!ReadExact(mem, accumulator_address, &accumulator,
                           sizeof(accumulator))) {
                ++write_errors;
                failed = true;
            } else if (!waiting_for_owner_change && hit != 1UL &&
                       hit != 2UL && hit != 4UL && hit != 8UL) {
                ++sequence_errors;
                failed = true;
            }

            if (!failed && waiting_for_owner_change) {
                bool owner_changed = false;
                const std::uint64_t owner_check_ns = MonotonicNs();
                if ((hit & 6UL) != 0) ++owner_gate_ignored_control_events;
                if ((hit & 1UL) != 0 && accumulator != 0 &&
                    !saw_initial_owner_dead &&
                    !FinalOwnerIdentityAlive(mem, base,
                                             initial_final_owner))
                    saw_initial_owner_dead = true;
                if ((hit & 1UL) != 0 && accumulator != 0 &&
                    saw_initial_owner_dead &&
                    owner_check_ns >= next_owner_resolve_ns) {
                    ++owner_resolve_attempts;
                    std::uintptr_t new_owner = 0;
                    if (ResolveFinalOwner(pid, base, 0, &new_owner)) {
                        a9tas::vehicle_state_v1::Layout new_layout{};
                        std::uint64_t scanned_bytes = 0;
                        bool layout_ok = !state_out;
                        if (state_out) {
                            // The proven physics interface has remained stable
                            // across Retry transitions. Rebuild and validate it
                            // in place first, avoiding a second multi-gigabyte
                            // scan while the authoritative physics thread is
                            // stopped. Fall back to a fail-closed global resolve
                            // only if the old interface identity was replaced.
                            layout_ok =
                                a9tas::vehicle_state_v1::BuildLayout(
                                    mem, base, state_layout.interface,
                                    state_layout.interface_vtable,
                                    &new_layout) ||
                                a9tas::vehicle_state_v1::Resolve(
                                    pid, mem, base, &new_layout,
                                    &scanned_bytes);
                        }
                        const std::uintptr_t new_c98 =
                            new_owner + kC98Offset;
                        const std::uintptr_t new_c9c =
                            new_owner + kC9COffset;
                        bool new_watch_ok = true;
                        std::uintptr_t new_state_watch = 0;
                        if (state_complete_gate) {
                            new_watch_ok =
                                new_layout.angular_address <=
                                UINTPTR_MAX - 2 * sizeof(float);
                            if (new_watch_ok) {
                                new_state_watch =
                                    new_layout.angular_address +
                                    2 * sizeof(float);
                                new_watch_ok =
                                    (new_state_watch &
                                     (sizeof(float) - 1)) == 0;
                            }
                        }
                        if (!layout_ok || !new_watch_ok ||
                            !PokeDebug(tid, 1, new_c98) ||
                            !PokeDebug(tid, 2, new_c9c) ||
                            !PokeDebug(tid, 3, new_state_watch) ||
                            !PokeDebug(
                                tid, 7,
                                BoundaryDr7(state_complete_gate))) {
                            ++ptrace_errors;
                            failed = true;
                        } else {
                            final_owner = new_owner;
                            owner_address_reused =
                                new_owner == initial_final_owner;
                            c98_address = new_c98;
                            c9c_address = new_c9c;
                            state_watch_address = new_state_watch;
                            if (state_out) {
                                state_layout = new_layout;
                                state_header.final_owner = final_owner;
                                state_header.physics_base =
                                    state_layout.physics_base;
                                state_header.position_address =
                                    state_layout.position_address;
                                state_header.rotation_address =
                                    state_layout.rotation_address;
                                state_header.linear_address =
                                    state_layout.linear_address;
                                state_header.angular_address =
                                    state_layout.angular_address;
                            }
                            waiting_for_owner_change = false;
                            owner_control_tid = tid;
                            // This stop is the new lifecycle's PRE_PHYSICS.
                            // Any DR1/DR2/DR3 bits would refer to disabled old
                            // targets and must not contaminate candidate tick 0.
                            hit = 1UL;
                            owner_changed = true;
                            std::printf(
                                "HWBP_SCHED_REPLAY_V1_OWNER_CHANGED old=0x%"
                                PRIxPTR " new=0x%" PRIxPTR
                                " address_reused=%u state_scanned=%" PRIu64
                                " tid=%d\n",
                                initial_final_owner, final_owner,
                                owner_address_reused ? 1u : 0u, scanned_bytes,
                                static_cast<int>(tid));
                            std::fflush(stdout);
                        }
                    }
                    if (!owner_changed) {
                        // Retry can destroy the old owner before the replacement
                        // exists. Bound global scans to one attempt per second,
                        // leave gameplay data untouched, and do not consume
                        // replay tick zero while waiting.
                        next_owner_resolve_ns = MonotonicNs() + 1000000000ULL;
                    }
                }
                if (!failed && !owner_changed) {
                    if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid)) {
                        ++ptrace_errors;
                        failed = true;
                    } else if (tracked) {
                        tracked->stopped = false;
                    }
                    if (failed) break;
                    continue;
                }
            }

            if (!failed && hit == 1UL && accumulator != 0) {
                if (wait_owner_change && !waiting_for_owner_change &&
                    tid != owner_control_tid) {
                    // A newly-created or replacement authoritative thread was
                    // initially attached with DR0 only. Enable the new owner's
                    // controls while stopped at PRE_PHYSICS, before either
                    // setter can execute.
                    if (!PokeDebug(tid, 1, c98_address) ||
                        !PokeDebug(tid, 2, c9c_address) ||
                        !PokeDebug(tid, 3, state_watch_address) ||
                        !PokeDebug(
                            tid, 7,
                            BoundaryDr7(state_complete_gate))) {
                        ++ptrace_errors;
                        failed = true;
                    } else {
                        owner_control_tid = tid;
                    }
                }
                if (!failed && in_cycle) {
                    ++sequence_errors;
                    failed = true;
                } else if (!failed) {
                    in_cycle = true;
                    saw_c98 = false;
                    saw_c9c = false;
                    saw_state_complete = false;
                    cycle_tid = tid;
                    cycle_original_interval_us = accumulator;
                    cycle_applied_interval_us =
                        fixed_interval_us != 0 ? fixed_interval_us
                                               : accumulator;
                    ++outer_cycles;
                    if (fixed_interval_us != 0 &&
                        !WriteExactVerified(mem, accumulator_address,
                                            &fixed_interval_us,
                                            sizeof(fixed_interval_us))) {
                        ++write_errors;
                        failed = true;
                    }
                }
            } else if (!failed && hit == 2UL) {
                if (!in_cycle || tid != cycle_tid || saw_c98 || saw_c9c ||
                    saw_state_complete) {
                    ++sequence_errors;
                    failed = true;
                } else {
                    const std::uint64_t pair = FramePair(frames[tick]);
                    if (!WriteExactVerified(mem, c98_address, &pair,
                                            sizeof(pair))) {
                        ++write_errors;
                        failed = true;
                    } else {
                        saw_c98 = true;
                    }
                }
            } else if (!failed && hit == 4UL) {
                if (!in_cycle || tid != cycle_tid || !saw_c98 || saw_c9c ||
                    saw_state_complete) {
                    ++sequence_errors;
                    failed = true;
                } else {
                    const std::uint64_t pair = FramePair(frames[tick]);
                    if (!WriteExactVerified(mem, c98_address, &pair,
                                            sizeof(pair))) {
                        ++write_errors;
                        failed = true;
                    } else {
                        saw_c9c = true;
                    }
                }
            } else if (!failed && hit == 8UL) {
                float angular_z = 0.0f;
                if (!state_complete_gate || !in_cycle || tid != cycle_tid ||
                    !saw_c98 || !saw_c9c || saw_state_complete) {
                    ++sequence_errors;
                    failed = true;
                } else if (!ReadExact(mem, state_watch_address, &angular_z,
                                      sizeof(angular_z)) ||
                           !std::isfinite(angular_z)) {
                    ++state_header.state_read_errors;
                    failed = true;
                } else {
                    saw_state_complete = true;
                    ++state_complete_hits;
                }
            } else if (!failed && hit == 1UL && accumulator == 0) {
                if (!in_cycle || tid != cycle_tid || saw_c98 != saw_c9c ||
                    (saw_state_complete && !(saw_c98 && saw_c9c))) {
                    ++sequence_errors;
                    failed = true;
                } else if (saw_c98 && saw_c9c) {
                    if (state_complete_gate && !saw_state_complete) {
                        ++sequence_errors;
                        failed = true;
                    } else if (state_out) {
                        a9tas::vehicle_state_v1::Snapshot snapshot{};
                        std::uint64_t controls = 0;
                        StateTraceFrameV1 state_frame{};
                        const bool state_ok =
                            a9tas::vehicle_state_v1::ReadSnapshot(
                                mem, state_layout, &snapshot) &&
                            ReadExact(mem, c98_address, &controls,
                                      sizeof(controls));
                        if (!state_ok) {
                            ++state_header.state_read_errors;
                            failed = true;
                        } else {
                            state_frame.tick = tick;
                            state_frame.monotonic_ns = MonotonicNs();
                            state_frame.original_interval_us =
                                cycle_original_interval_us;
                            state_frame.applied_interval_us =
                                cycle_applied_interval_us;
                            std::memcpy(state_frame.position,
                                        snapshot.position,
                                        sizeof(state_frame.position));
                            std::memcpy(state_frame.rotation,
                                        snapshot.rotation,
                                        sizeof(state_frame.rotation));
                            std::memcpy(state_frame.linear, snapshot.linear,
                                        sizeof(state_frame.linear));
                            std::memcpy(state_frame.angular, snapshot.angular,
                                        sizeof(state_frame.angular));
                            const std::uint32_t c98_bits =
                                static_cast<std::uint32_t>(controls);
                            const std::uint32_t c9c_bits =
                                static_cast<std::uint32_t>(controls >> 32);
                            std::memcpy(&state_frame.c98, &c98_bits,
                                        sizeof(c98_bits));
                            std::memcpy(&state_frame.c9c, &c9c_bits,
                                        sizeof(c9c_bits));
                            state_frame.flags = 1u;
                            if (std::fwrite(&state_frame,
                                            sizeof(state_frame), 1,
                                            state_out) != 1) {
                                ++state_header.state_write_errors;
                                failed = true;
                            } else {
                                ++state_frames;
                            }
                        }
                    }
                    if (!failed) ++tick;
                    if (tick % 120 == 0 || tick == frames.size()) {
                        std::printf(
                            "HWBP_SCHED_REPLAY_V1_PROGRESS ticks=%zu/%zu "
                            "outer=%" PRIu64 " paused=%" PRIu64 "\n",
                            tick, frames.size(), outer_cycles, paused_cycles);
                        std::fflush(stdout);
                    }
                } else {
                    ++paused_cycles;
                }
                in_cycle = false;
                saw_c98 = false;
                saw_c9c = false;
                saw_state_complete = false;
                cycle_tid = 0;
            }

            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid)) {
                ++ptrace_errors;
                failed = true;
            } else if (tracked) {
                tracked->stopped = false;
            }
            if (failed) break;
        } else {
            ++unexpected_stops;
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver)) {
                ++ptrace_errors;
                failed = true;
            } else if (tracked) {
                tracked->stopped = false;
            }
            if (failed) break;
        }
    }

    const std::uint64_t loop_end_ns = MonotonicNs();
    CleanupReplayThreads(&threads, &ptrace_errors);
    bool complete = tick == frames.size() && !in_cycle && !failed &&
                    !waiting_for_owner_change &&
                    write_errors == 0 && ptrace_errors == 0 &&
                    unexpected_stops == 0 && sequence_errors == 0 &&
                    attach_errors == 0 &&
                    (!state_complete_gate ||
                     state_complete_hits == frames.size());
    if (state_out) {
        state_header.start_ns = start_ns;
        state_header.state_frames = state_frames;
        if (complete && state_frames == frames.size() &&
            state_header.state_read_errors == 0 &&
            state_header.state_write_errors == 0)
            state_header.flags |= 1u;
        if (std::fseek(state_out, 0, SEEK_SET) != 0 ||
            std::fwrite(&state_header, sizeof(state_header), 1, state_out) !=
                1 ||
            std::fflush(state_out) != 0 || std::fclose(state_out) != 0) {
            complete = false;
        }
    }
    close(mem);
    std::printf(
        "HWBP_SCHED_REPLAY_V1_DONE complete=%u ticks=%zu/%zu events=%" PRIu64
        " outer=%" PRIu64 " paused=%" PRIu64 " write_errors=%" PRIu64
        " ptrace_errors=%" PRIu64 " sequence_errors=%" PRIu64
        " unexpected_stops=%" PRIu64 " attach_errors=%" PRIu64 "\n",
        complete ? 1u : 0u, tick, frames.size(), events, outer_cycles,
        paused_cycles, write_errors, ptrace_errors, sequence_errors,
        unexpected_stops, attach_errors);
    if (wait_owner_change) {
        std::printf(
            "HWBP_SCHED_REPLAY_V1_OWNER_GATE attempts=%" PRIu64
            " transitioned=%u saw_old_dead=%u address_reused=%u "
            "ignored_old_control_events=%" PRIu64 "\n",
            owner_resolve_attempts, waiting_for_owner_change ? 0u : 1u,
            saw_initial_owner_dead ? 1u : 0u,
            owner_address_reused ? 1u : 0u,
            owner_gate_ignored_control_events);
    }
    if (state_output_path) {
        std::printf(
            "HWBP_SCHED_REPLAY_V1_STATE frames=%" PRIu64
            " state_complete_hits=%" PRIu64
            " read_errors=%" PRIu64 " write_errors=%" PRIu64
            " path=%s\n",
            state_frames, state_complete_hits,
            state_header.state_read_errors,
            state_header.state_write_errors, state_output_path);
    }
    std::printf("HWBP_SCHED_REPLAY_V1_CLOCK_END now=%" PRIu64
                " elapsed_ms=%.3f deadline=%" PRIu64 "\n",
                loop_end_ns,
                static_cast<double>(loop_end_ns - start_ns) / 1000000.0,
                deadline_ns);
    return complete ? 0 : 6;
}
