// Host-only, read-only A9NPS1 recorder at the live-certified Gate 2 boundary.
//
// Four hardware data breakpoints retain the exact ordering proof while this
// first recorder is validated.  A raw native transform+linear snapshot is
// taken after callback-list close and committed to the recording only after
// the next inner-world accumulator write proves the cycle completed.
//
// Target-process writes are limited to x86_64 debug registers.  This program
// opens /proc/PID/mem O_RDONLY, never patches guest code, never single-steps,
// never invokes a game function and never writes gameplay state.

#define A9TAS_PIPELINE_ORDER_NO_MAIN
#include "hwbp_pipeline_order_observer_v1.cpp"

#include "native_physics_recording_v1.h"

#include <optional>

namespace {

using a9tas::native_physics_v1::RecordingFrameV1;
using a9tas::native_physics_v1::RecordingHeaderV1;

constexpr std::size_t kMaximumFrames = 36000;

enum class CaptureState {
    kWaiting,
    kCompleted,
    kCallbackOpen,
    kPublished,
    kCallbackClosed,
};

const char* CaptureStateName(CaptureState state) {
    switch (state) {
        case CaptureState::kWaiting:
            return "waiting";
        case CaptureState::kCompleted:
            return "completed";
        case CaptureState::kCallbackOpen:
            return "callback_open";
        case CaptureState::kPublished:
            return "published";
        case CaptureState::kCallbackClosed:
            return "callback_closed";
    }
    return "unknown";
}

bool NativeIdentityAlive(
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

bool RawComponentsFinite(const RecordingFrameV1& frame) {
    std::uint32_t bits[19]{};
    std::memcpy(bits, frame.transform_bits, sizeof(frame.transform_bits));
    std::memcpy(bits + 16, frame.linear_velocity_bits,
                sizeof(frame.linear_velocity_bits));
    for (std::uint32_t value : bits) {
        float component = 0.0f;
        std::memcpy(&component, &value, sizeof(component));
        if (!std::isfinite(component) || std::fabs(component) > 1000000.0f)
            return false;
    }
    return true;
}

bool ReadNativeFrame(
    int mem, std::uint64_t tick, std::uint64_t monotonic_ns,
    const a9tas::vehicle_state_v1::Layout& vehicle,
    const a9tas::vehicle_state_v1::BackendLayout& backend,
    std::uintptr_t base, RecordingFrameV1* output) {
    RecordingFrameV1 frame{};
    frame.tick = tick;
    frame.monotonic_ns = monotonic_ns;
    frame.flags =
        a9tas::native_physics_v1::kFrameCapturedAtCertifiedBoundary;
    if (!NativeIdentityAlive(mem, vehicle, backend, base) ||
        !ReadExact(mem, backend.native_pose_address, frame.transform_bits,
                   sizeof(frame.transform_bits)) ||
        !ReadExact(mem, backend.native_linear_address,
                   frame.linear_velocity_bits,
                   sizeof(frame.linear_velocity_bits)) ||
        !RawComponentsFinite(frame))
        return false;
    *output = frame;
    return true;
}

bool WriteRecording(const char* path,
                    const std::vector<RecordingFrameV1>& frames) {
    if (frames.empty() || frames.size() > kMaximumFrames) return false;
    RecordingHeaderV1 header{};
    std::memcpy(header.magic, a9tas::native_physics_v1::kMagic,
                sizeof(header.magic));
    header.version = a9tas::native_physics_v1::kVersion;
    header.header_size = sizeof(header);
    header.frame_size = sizeof(RecordingFrameV1);
    header.frame_count = static_cast<std::uint32_t>(frames.size());
    std::memcpy(header.build_id,
                a9tas::native_physics_v1::kSupportedBuildId,
                sizeof(header.build_id));
    header.transform_offset = a9tas::native_physics_v1::kTransformOffset;
    header.linear_velocity_offset =
        a9tas::native_physics_v1::kLinearVelocityOffset;
    header.transform_size = a9tas::native_physics_v1::kTransformSize;
    header.linear_velocity_size =
        a9tas::native_physics_v1::kLinearVelocitySize;
    header.flags = a9tas::native_physics_v1::kRequiredHeaderFlags;

    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const bool ok =
        std::fwrite(&header, sizeof(header), 1, file) == 1 &&
        std::fwrite(frames.data(), sizeof(RecordingFrameV1), frames.size(),
                    file) == frames.size() &&
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
    if (argc < 5 || argc > 7) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX DURATION_MS OUT_PATH "
                     "[PHYSICS_CONTEXT_HEX] [TARGET_FRAMES]\n",
                     argv[0]);
        return 2;
    }
    std::uint64_t pid_value = 0;
    std::uint64_t base_value = 0;
    std::uint64_t duration_value = 0;
    std::uint64_t context_value = 0;
    std::uint64_t target_frames_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &duration_value) ||
        (argc >= 6 && !ParseUnsigned(argv[5], 16, &context_value)) ||
        (argc == 7 && !ParseUnsigned(argv[6], 10, &target_frames_value)) ||
        pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
        base_value == 0 || duration_value < 100 ||
        duration_value > 300000 || target_frames_value > kMaximumFrames) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    const auto duration_ms = static_cast<std::uint64_t>(duration_value);
    const auto target_frames =
        static_cast<std::size_t>(target_frames_value);

    if (!VerifyTargetBuild(pid, base)) {
        std::fprintf(stderr, "unsupported build; no thread was attached\n");
        return 3;
    }
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
    const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) return 4;

    std::uintptr_t context = 0;
    std::uintptr_t adapter = 0;
    std::uintptr_t world = 0;
    if (!ResolvePhysicsContext(pid, mem, base,
                               static_cast<std::uintptr_t>(context_value),
                               &context, &adapter, &world)) {
        close(mem);
        return 3;
    }
    a9tas::vehicle_state_v1::Layout vehicle{};
    a9tas::vehicle_state_v1::BackendLayout backend{};
    if (!a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle) ||
        !a9tas::vehicle_state_v1::ResolveBackendLayout(mem, base, vehicle,
                                                       &backend) ||
        !NativeIdentityAlive(mem, vehicle, backend, base)) {
        std::fprintf(stderr,
                     "exact player native-body resolution failed stage=%u\n",
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
        !PipelineWritable(maps, backend.native_pose_address,
                          a9tas::native_physics_v1::kTransformSize) ||
        !PipelineWritable(maps, backend.native_linear_address,
                          a9tas::native_physics_v1::kLinearVelocitySize)) {
        std::fprintf(stderr, "recording address validation failed\n");
        close(mem);
        return 3;
    }

    std::vector<TracedThread> threads;
    std::uint64_t ptrace_errors = 0;
    std::uint64_t read_errors = 0;
    std::uint64_t unexpected_stops = 0;
    std::uint64_t semantic_errors = 0;
    std::uint64_t event_count = 0;
    const std::size_t initial = AttachNewThreads(
        pid, completion, callback_flags, f64, accumulator, &threads,
        &ptrace_errors, PipelineDr7());
    if (initial == 0) {
        close(mem);
        return 7;
    }

    std::printf(
        "NATIVE_PHYSICS_RECORDER_V1 pid=%d context=0x%" PRIxPTR
        " car_state=0x%" PRIxPTR " native=0x%" PRIxPTR
        " transform=0x%" PRIxPTR " linear=0x%" PRIxPTR
        " threads=%zu duration_ms=%" PRIu64 " target_frames=%zu"
        " mem_mode=read-only write_scope=debug-registers-only\n",
        static_cast<int>(pid), context, vehicle.physics_base,
        backend.native_body, backend.native_pose_address,
        backend.native_linear_address, threads.size(), duration_ms,
        target_frames);
    std::fflush(stdout);

    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + duration_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 250000000ULL;
    std::uint32_t accounted_threads = 0;
    CaptureState state = CaptureState::kWaiting;
    pid_t owner_tid = 0;
    std::optional<RecordingFrameV1> pending;
    std::vector<RecordingFrameV1> frames;
    frames.reserve(static_cast<std::size_t>(duration_ms / 10 + 16));

    while (MonotonicNs() < deadline_ns && semantic_errors == 0 &&
           frames.size() < kMaximumFrames &&
           (target_frames == 0 || frames.size() < target_frames)) {
        const std::uint64_t now_ns = MonotonicNs();
        if (now_ns >= next_rescan_ns) {
            std::uint64_t failures = 0;
            AttachNewThreads(pid, completion, callback_flags, f64,
                             accumulator, &threads, &failures, PipelineDr7());
            ptrace_errors += failures;
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
            if (errno != ECHILD) ++ptrace_errors;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
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
            ++event_count;
            std::uint16_t flags_value = 0;
            std::uint32_t f64_bits = 0;
            std::uint32_t accumulator_bits = 0;
            if (!ReadExact(mem, callback_flags, &flags_value,
                           sizeof(flags_value)) ||
                !ReadExact(mem, f64, &f64_bits, sizeof(f64_bits)) ||
                !ReadExact(mem, accumulator, &accumulator_bits,
                           sizeof(accumulator_bits))) {
                ++read_errors;
            } else {
                const auto fail = [&](const char* reason) {
                    if (semantic_errors == 0) {
                        std::fprintf(
                            stderr,
                            "semantic_fault reason=%s event=%" PRIu64
                            " frames=%zu state=%s dr6=0x%lx tid=%d owner=%d"
                            " callback_flags=0x%04x f64=0x%08x"
                            " accumulator=0x%08x\n",
                            reason, event_count, frames.size(),
                            CaptureStateName(state), dr6,
                            static_cast<int>(tid), static_cast<int>(owner_tid),
                            flags_value, f64_bits, accumulator_bits);
                    }
                    ++semantic_errors;
                };
                if (dr6 & 1UL) {
                    if (state != CaptureState::kWaiting)
                        fail("completion_before_cycle_commit");
                    else {
                        state = CaptureState::kCompleted;
                        owner_tid = tid;
                    }
                }
                if ((dr6 & 2UL) && semantic_errors == 0) {
                    const std::uint8_t dispatching =
                        static_cast<std::uint8_t>(flags_value & 0xFFu);
                    if (state == CaptureState::kWaiting) {
                        // The capture may begin in the middle of an existing
                        // cycle. Match the certified parser: ignore all
                        // non-completion prefix events until the next complete
                        // cycle starts.
                    } else if (state == CaptureState::kCompleted &&
                               dispatching == 1) {
                        if (tid != owner_tid)
                            fail("callback_open_wrong_tid");
                        else
                            state = CaptureState::kCallbackOpen;
                    } else if (state == CaptureState::kPublished &&
                               dispatching == 0) {
                        RecordingFrameV1 frame{};
                        if (tid != owner_tid ||
                            !ReadNativeFrame(mem, frames.size(), MonotonicNs(),
                                             vehicle, backend, base, &frame)) {
                            ++read_errors;
                        } else {
                            pending = frame;
                            state = CaptureState::kCallbackClosed;
                        }
                    } else if (state == CaptureState::kCallbackClosed &&
                               dispatching == 0) {
                        // Expected deferred-flag clear at context+0x1A1.
                    } else {
                        fail("unexpected_callback_flag_transition");
                    }
                }
                if ((dr6 & 4UL) && semantic_errors == 0) {
                    float value = 0.0f;
                    std::memcpy(&value, &f64_bits, sizeof(value));
                    if (state == CaptureState::kWaiting) {
                        // Ignore an incomplete prefix publication.
                    } else if (state != CaptureState::kCallbackOpen ||
                        tid != owner_tid || !std::isfinite(value) ||
                        std::fabs(value) > 1000000.0f)
                        fail("unexpected_f64_publication");
                    else
                        state = CaptureState::kPublished;
                }
                if ((dr6 & 8UL) && semantic_errors == 0) {
                    float value = 0.0f;
                    std::memcpy(&value, &accumulator_bits, sizeof(value));
                    if (!std::isfinite(value) || std::fabs(value) > 60.0f) {
                        fail("invalid_accumulator_value");
                    } else if (state == CaptureState::kCallbackClosed) {
                        if (!pending)
                            fail("callback_closed_without_pending_frame");
                        else {
                            frames.push_back(*pending);
                            pending.reset();
                            state = CaptureState::kWaiting;
                            owner_tid = 0;
                        }
                    } else if (state != CaptureState::kWaiting) {
                        fail("accumulator_before_callback_close");
                    }
                }
            }
            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
                ++ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        } else {
            ++unexpected_stops;
            const int deliver = signal == SIGTRAP ? 0 : signal;
            if (!ContinueThread(tid, deliver))
                ++ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        }
    }

    for (auto& thread : threads) {
        if (!thread.live) continue;
        if (!ClearAndDetach(thread.tid, thread.stopped))
            ++ptrace_errors;
        else
            ++accounted_threads;
        thread.live = false;
        thread.stopped = false;
    }
    close(mem);

    const bool clean = !frames.empty() && read_errors == 0 &&
                       ptrace_errors == 0 && unexpected_stops == 0 &&
                       semantic_errors == 0 &&
                       accounted_threads == threads.size();
    bool output_ok = false;
    if (clean) output_ok = WriteRecording(argv[4], frames);
    if (!output_ok) std::remove(argv[4]);
    std::printf(
        "NATIVE_PHYSICS_RECORDER_V1_DONE frames=%zu read_errors=%" PRIu64
        " ptrace_errors=%" PRIu64 " unexpected_stops=%" PRIu64
        " semantic_errors=%" PRIu64 " clean=%u path=%s\n",
        frames.size(), read_errors, ptrace_errors, unexpected_stops,
        semantic_errors, clean && output_ok, argv[4]);
    return clean && output_ok ? 0 : 8;
}
