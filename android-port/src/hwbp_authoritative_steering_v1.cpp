// BUILD-ONLY natural-pre-roll authoritative steering-only executor.
//
// Search cycles are observation-only. After a bound A9NPA1 match, the next
// positive Delta selects exact replay tick zero. Replay performs exactly one
// verified fixed-delta write and two verified steering-pair writes per packet.
// Brake low bits are preserved independently at C98 and C9C. Every action,
// Nitro, brake, accelerator and transform/linear correction is absent.
// Runtime deployment requires separate explicit user authorization.

#define A9TAS_PIPELINE_ORDER_NO_MAIN
#include "hwbp_pipeline_order_observer_v1.cpp"

#include "authoritative_natural_handoff_v1.h"
#if defined(A9TAS_AUTHORITATIVE_CONTROLS_STARTLINE_DIRECT)
#include "authoritative_controls_transport_v1.h"
#endif
#include "authoritative_steering_transport_v1.h"
#include "natural_preroll_anchor_v1.h"
#if defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
#include "startline_prearm_protocol_v1.h"
#endif
#include "unified_tick_recording_v1.h"

#include <climits>
#include <cmath>
#include <limits>

namespace {

using a9tas::authoritative_bridge_v1::EventV1;
using a9tas::authoritative_natural_v1::AdvanceResultV1;
using a9tas::authoritative_natural_v1::ModeV1;
#if defined(A9TAS_AUTHORITATIVE_CONTROLS_STARTLINE_DIRECT)
using ActivePairPlanV1 = a9tas::authoritative_controls_v1::PairPlanV1;
#else
using ActivePairPlanV1 = a9tas::authoritative_steering_v1::PairPlanV1;
#endif
using a9tas::unified_tick_v1::RecordingFrameV1;
using a9tas::unified_tick_v1::RecordingHeaderV1;

constexpr char kSteeringReportMagic[8] = {
    'A', '9', 'A', 'S', 'T', '1', '\0', '\0',
};
constexpr std::uint32_t kSteeringReportVersion = 1;
constexpr std::size_t kMinimumFrames = 1;
constexpr std::size_t kMaximumFrames = 36000;
constexpr std::uint32_t kRequiredFixedUs = 16667;
#if defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY) && \
    defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
#error "search-only and startline-direct modes are mutually exclusive"
#elif defined(A9TAS_AUTHORITATIVE_CONTROLS_STARTLINE_DIRECT) && \
    !defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
#error "controls-startline mode requires startline-direct alignment"
#elif defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY)
constexpr char kAcknowledgement[] =
    "I_ACCEPT_ZERO_WRITE_ANCHOR_SEARCH_V1";
#elif defined(A9TAS_AUTHORITATIVE_CONTROLS_STARTLINE_DIRECT)
constexpr char kAcknowledgement[] =
    "I_ACCEPT_STARTLINE_DIRECT_DELTA_AND_2X_STEERING_BRAKE_WRITES_V1";
#elif defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
constexpr char kAcknowledgement[] =
    "I_ACCEPT_STARTLINE_DIRECT_DELTA_AND_2X_STEERING_WRITES_V1";
#else
constexpr char kAcknowledgement[] =
    "I_ACCEPT_RECORDING_DECLARED_DELTA_AND_2X_STEERING_WRITES_V2";
#endif

enum HeaderFlag : std::uint32_t {
    kTargetVerified = 1u << 0,
    kAddressesValidated = 1u << 1,
    kRecordingValidated = 1u << 2,
    kAnchorValidated = 1u << 3,
    kAnchorMatched = 1u << 4,
    kAuthoritativeComplete = 1u << 5,
    kWritesExact = 1u << 6,
    kOtherCapabilitiesAbsent = 1u << 7,
    kCleanDetach = 1u << 8,
};

#pragma pack(push, 1)
struct PairAuditV1 {
    std::uint64_t before;
    std::uint64_t intended;
    std::uint64_t after;
};

struct FrameAuditV1 {
    std::uint32_t selected_tick;
    std::uint32_t published_tick;
    std::int32_t cycle_tid;
    std::int32_t commit_tid;
    std::uint32_t steering_bits;
    std::uint32_t phase_action_or;
    std::int64_t observed_delta_us;
    std::int64_t applied_delta_us;
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
    PairAuditV1 c98_pair;
    PairAuditV1 c9c_pair;
};

struct ReportHeaderV1 {
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
    std::uint64_t native_body;
    std::uint64_t delta_address;
    std::uint64_t c98_address;
    std::uint64_t c9c_address;
    std::uint64_t completion_address;
    std::uint64_t callback_flags_address;
    std::uint64_t f64_address;
    std::uint64_t world_accumulator_address;
    std::uint64_t event_count;
    std::uint64_t search_cycles;
    std::uint64_t rejected_search_prefixes;
    std::uint64_t read_errors;
    std::uint64_t ptrace_errors;
    std::uint64_t semantic_errors;
    std::uint64_t delta_write_attempts;
    std::uint64_t delta_writes;
    std::uint64_t delta_write_failures;
    std::uint64_t pair_write_attempts;
    std::uint64_t pair_writes;
    std::uint64_t pair_write_failures;
    std::uint64_t gameplay_action_calls;
    std::uint64_t physics_correction_writes;
    std::uint64_t thread_additions;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
    std::uint32_t fixed_interval_us;
    std::uint32_t recording_frames;
    std::uint32_t bound_frames;
    std::uint32_t nonzero_steering_frames;
    std::uint32_t matched_search_cycle;
    std::uint32_t reserved1;
};
#pragma pack(pop)

static_assert(sizeof(PairAuditV1) == 24, "A9AST1 pair ABI");
static_assert(sizeof(FrameAuditV1) == 172, "A9AST1 frame ABI");
static_assert(sizeof(ReportHeaderV1) == 304, "A9AST1 header ABI");

bool AllZero(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index)
        if (bytes[index] != 0) return false;
    return true;
}

bool FinitePhysics(const RecordingFrameV1& frame) {
    float values[19]{};
    std::memcpy(values, frame.transform_bits, sizeof(frame.transform_bits));
    std::memcpy(values + 16, frame.linear_velocity_bits,
                sizeof(frame.linear_velocity_bits));
    for (float value : values)
        if (!std::isfinite(value) || std::fabs(value) > 1000000.0f)
            return false;
    return true;
}

bool LoadStrictRecording(const char* path, RecordingHeaderV1* header,
                         std::vector<RecordingFrameV1>* frames) {
    if (!path || !header || !frames) return false;
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    const bool header_read = std::fread(header, sizeof(*header), 1, file) == 1;
    const bool valid_header =
        header_read &&
        std::memcmp(header->magic, a9tas::unified_tick_v1::kMagic,
                    sizeof(header->magic)) == 0 &&
        header->version == a9tas::unified_tick_v1::kVersion &&
        header->header_size == sizeof(*header) &&
        header->frame_size == sizeof(RecordingFrameV1) &&
        header->frame_count >= kMinimumFrames &&
        header->frame_count <= kMaximumFrames &&
        header->fixed_interval_us == kRequiredFixedUs &&
        header->flags == a9tas::unified_tick_v1::kRequiredHeaderFlags &&
        header->supported_skip_mask ==
            a9tas::unified_tick_v1::kSupportedSkipMask &&
        std::memcmp(header->build_id,
                    a9tas::unified_tick_v1::kSupportedBuildId,
                    sizeof(header->build_id)) == 0 &&
        header->transform_offset == a9tas::unified_tick_v1::kTransformOffset &&
        header->linear_velocity_offset ==
            a9tas::unified_tick_v1::kLinearVelocityOffset &&
        header->angular_velocity_offset ==
            a9tas::unified_tick_v1::kAngularVelocityOffset &&
        header->transform_size == a9tas::unified_tick_v1::kTransformSize &&
        header->linear_velocity_size ==
            a9tas::unified_tick_v1::kLinearVelocitySize &&
        header->angular_velocity_size ==
            a9tas::unified_tick_v1::kAngularVelocitySize &&
        AllZero(header->reserved, sizeof(header->reserved));
    if (!valid_header) {
        std::fclose(file);
        return false;
    }
    frames->resize(header->frame_count);
    const bool body_ok =
        std::fread(frames->data(), sizeof(frames->front()), frames->size(),
                   file) == frames->size() &&
        std::fgetc(file) == EOF;
    std::fclose(file);
    if (!body_ok) return false;
    for (std::size_t index = 0; index < frames->size(); ++index) {
        const auto& frame = (*frames)[index];
        if (frame.tick != index ||
            (index != 0 && frame.monotonic_ns <
                               (*frames)[index - 1].monotonic_ns) ||
#if defined(A9TAS_AUTHORITATIVE_CONTROLS_STARTLINE_DIRECT)
            !a9tas::authoritative_controls_v1::FrameIsSteeringBrake(frame) ||
#else
            !a9tas::authoritative_steering_v1::FrameIsSteeringOnly(frame) ||
#endif
            !FinitePhysics(frame))
            return false;
    }
    return true;
}

bool LoadBoundAnchor(const char* path, const RecordingHeaderV1& recording,
                     const std::vector<RecordingFrameV1>& frames,
                     a9tas::natural_preroll_v1::AnchorV1* output) {
    if (!path || frames.empty() || !output) return false;
    FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    a9tas::natural_preroll_v1::AnchorV1 anchor{};
    const bool exact = std::fread(&anchor, sizeof(anchor), 1, file) == 1 &&
                       std::fgetc(file) == EOF;
    std::fclose(file);
    bool recording_hash = false, report_hash = false;
    for (std::size_t index = 0; index < 32; ++index) {
        recording_hash |= anchor.recording_sha256[index] != 0;
        report_hash |= anchor.report_sha256[index] != 0;
    }
    if (!exact ||
        std::memcmp(anchor.magic, a9tas::natural_preroll_v1::kMagic, 8) != 0 ||
        anchor.version != a9tas::natural_preroll_v1::kVersion ||
        anchor.size != sizeof(anchor) ||
        anchor.flags != a9tas::natural_preroll_v1::kBoundFlags ||
        anchor.fixed_interval_us != recording.fixed_interval_us ||
        anchor.frame_count != frames.size() || anchor.reserved0 != 0 ||
        std::memcmp(anchor.build_id,
                    a9tas::unified_tick_v1::kSupportedBuildId, 20) != 0 ||
        !AllZero(anchor.reserved1, sizeof(anchor.reserved1)) ||
        !AllZero(anchor.reserved2, sizeof(anchor.reserved2)) ||
        !recording_hash || !report_hash || anchor.source_pid == 0 ||
        anchor.source_library_base == 0 || anchor.source_main_object == 0 ||
        anchor.source_final_owner == 0 ||
        anchor.source_physics_context == 0 || anchor.source_native_body == 0 ||
        anchor.cycle_tid <= 0 ||
        anchor.commit_tid <= 0 || anchor.cycle_tid == anchor.commit_tid ||
        anchor.completion_before == anchor.completion_after ||
        (anchor.callback_flags & 0xffu) != 1u ||
        std::memcmp(anchor.frame0_transform, frames.front().transform_bits,
                    sizeof(anchor.frame0_transform)) != 0 ||
        std::memcmp(anchor.frame0_linear,
                    frames.front().linear_velocity_bits,
                    sizeof(anchor.frame0_linear)) != 0)
        return false;
    for (std::size_t index = 1; index < 7; ++index)
        if (anchor.events[index - 1] >= anchor.events[index]) return false;
    float values[38]{};
    std::memcpy(values, anchor.anchor_transform, 64);
    std::memcpy(values + 16, anchor.anchor_linear, 12);
    std::memcpy(values + 19, anchor.frame0_transform, 64);
    std::memcpy(values + 35, anchor.frame0_linear, 12);
    for (float value : values)
        if (!std::isfinite(value) || std::fabs(value) > 1000000.0f)
            return false;
    *output = anchor;
    return true;
}

bool ReadAnchorMatch(int mem, std::uintptr_t native_body,
                     const a9tas::natural_preroll_v1::AnchorV1& anchor,
                     bool* matches, float* maximum_normalized_error = nullptr,
                     std::uint32_t* worst_component = nullptr) {
    if (!matches) return false;
    *matches = false;
    float current[19]{}, expected[19]{}, frame0[19]{};
    if (!ReadExact(mem, native_body + a9tas::unified_tick_v1::kTransformOffset,
                   current, 64) ||
        !ReadExact(mem,
                   native_body +
                       a9tas::unified_tick_v1::kLinearVelocityOffset,
                   current + 16, 12))
        return false;
    std::memcpy(expected, anchor.anchor_transform, 64);
    std::memcpy(expected + 16, anchor.anchor_linear, 12);
    std::memcpy(frame0, anchor.frame0_transform, 64);
    std::memcpy(frame0 + 16, anchor.frame0_linear, 12);
    float maximum_error = 0.0f;
    std::uint32_t maximum_index = 0;
    for (std::size_t index = 0; index < 19; ++index) {
        const float step = std::fabs(frame0[index] - expected[index]);
        const float floor = index < 16 ? 0.01f : 0.5f;
        const float tolerance = std::max(floor, step * 2.0f + 0.001f);
        if (!std::isfinite(current[index])) return true;
        const float normalized_error =
            std::fabs(current[index] - expected[index]) / tolerance;
        if (normalized_error > maximum_error) {
            maximum_error = normalized_error;
            maximum_index = static_cast<std::uint32_t>(index);
        }
    }
    *matches = maximum_error <= 1.0f;
    if (maximum_normalized_error)
        *maximum_normalized_error = maximum_error;
    if (worst_component) *worst_component = maximum_index;
    return true;
}

#if defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
bool CreateStartlineReadyMarker(const char* path, pid_t pid,
                                std::uintptr_t delta_address) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    char payload[160]{};
    const int length = std::snprintf(
        payload, sizeof(payload),
        "READY_NO_ATTACH_V1 pid=%d delta=0x%" PRIxPTR
        " target_threads_attached=0 game_writes=0\n",
        static_cast<int>(pid), delta_address);
    bool ok = length > 0 && static_cast<std::size_t>(length) < sizeof(payload);
    std::size_t written = 0;
    while (ok && written < static_cast<std::size_t>(length)) {
        const ssize_t count =
            write(fd, payload + written,
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

bool PrimeDirectReplay(
    a9tas::authoritative_natural_v1::ControllerV1* controller,
    AdvanceResultV1* state) {
    if (controller == nullptr || state == nullptr) return false;
    constexpr EventV1 events[] = {
        EventV1::kDeltaNonzero,
        EventV1::kC98,
        EventV1::kC9C,
        EventV1::kPrefixCertified,
        EventV1::kF64,
        EventV1::kCallbackClose,
        EventV1::kDeferredCallbackClear,
        EventV1::kWorldCommit,
    };
    for (EventV1 event : events) {
        const bool callback_close = event == EventV1::kCallbackClose;
        *state = a9tas::authoritative_natural_v1::Advance(
            controller, event, callback_close, callback_close);
        if (!state->accepted) return false;
    }
    return state->mode == ModeV1::kReplay && state->anchor_matched &&
           state->search_cycles == 1 && !state->replay.packet_selected;
}
#endif

bool WriteFixedDeltaVerified(int mem, std::uintptr_t address,
                             std::int64_t value) {
#if defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY)
    (void)mem;
    (void)address;
    (void)value;
    return false;
#else
    if ((address & 7u) != 0 || value != kRequiredFixedUs) return false;
    if (pwrite(mem, &value, sizeof(value), static_cast<off_t>(address)) !=
        static_cast<ssize_t>(sizeof(value)))
        return false;
    std::int64_t after = 0;
    return ReadExact(mem, address, &after, sizeof(after)) && after == value;
#endif
}

bool WritePairVerified(int mem, std::uintptr_t address,
                       const ActivePairPlanV1& plan, std::uint64_t* after) {
#if defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY)
    (void)mem;
    (void)address;
    (void)plan;
    (void)after;
    return false;
#else
    if (!after || (address & 7u) != 0 ||
        static_cast<std::uint32_t>(plan.outcome) != 0)
        return false;
    if (pwrite(mem, &plan.pair_intended, sizeof(plan.pair_intended),
               static_cast<off_t>(address)) !=
        static_cast<ssize_t>(sizeof(plan.pair_intended)))
        return false;
    return ReadExact(mem, address, after, sizeof(*after)) &&
#if defined(A9TAS_AUTHORITATIVE_CONTROLS_STARTLINE_DIRECT)
           a9tas::authoritative_controls_v1::VerifyAppliedPair(plan, *after);
#else
           a9tas::authoritative_steering_v1::VerifyAppliedPair(plan, *after);
#endif
#endif
}

bool ProgramStoppedThreadV1(pid_t tid, std::uintptr_t dr0,
                            std::uintptr_t dr1, std::uintptr_t dr2,
                            std::uintptr_t dr3, unsigned long dr7) {
    return PokeDebug(tid, 7, 0) && PokeDebug(tid, 0, dr0) &&
           PokeDebug(tid, 1, dr1) && PokeDebug(tid, 2, dr2) &&
           PokeDebug(tid, 3, dr3) && PokeDebug(tid, 6, 0) &&
           PokeDebug(tid, 7, dr7);
}

bool RearmInputOwner(pid_t current_tid, pid_t owner_tid,
                     std::vector<TracedThread>* threads,
                     std::uintptr_t delta, std::uintptr_t c98,
                     std::uintptr_t c9c, std::uintptr_t world) {
    if (owner_tid <= 0) return false;
    if (owner_tid == current_tid)
        return ProgramStoppedThreadV1(owner_tid, delta, c98, c9c, world,
                                      BoundaryDr7(true));
    TracedThread* owner = FindThread(threads, owner_tid);
    if (!owner || !owner->live) return false;
    if (!owner->stopped && !StopThread(owner_tid)) return false;
    owner->stopped = true;
    if (!ProgramStoppedThreadV1(owner_tid, delta, c98, c9c, world,
                                BoundaryDr7(true)) ||
        !ContinueThread(owner_tid))
        return false;
    owner->stopped = false;
    return true;
}

bool WriteReport(const char* path, const ReportHeaderV1& header,
                 const std::vector<FrameAuditV1>& audits) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        std::remove(path);
        return false;
    }
    const bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
                    std::fwrite(audits.data(), sizeof(audits.front()),
                                audits.size(), file) == audits.size() &&
                    std::fflush(file) == 0 && std::ferror(file) == 0;
    const bool close_ok = std::fclose(file) == 0;
    if (!ok || !close_ok) std::remove(path);
    return ok && close_ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 8 || argc > 11) {
        std::fprintf(stderr,
                     "usage: %s PID LIB_BASE_HEX TIMEOUT_MS A9UTK1 A9NPA1 "
                     "REPORT ACK [CONTEXT_HEX] [MAIN_HEX] [OWNER_HEX]\n",
                     argv[0]);
        return 2;
    }
    if (std::strcmp(argv[7], kAcknowledgement) != 0 ||
        access(argv[6], F_OK) == 0) {
        std::fprintf(stderr,
                     "authorization missing or report exists; no process opened\n");
        return 2;
    }
    RecordingHeaderV1 recording{};
    std::vector<RecordingFrameV1> frames;
    a9tas::natural_preroll_v1::AnchorV1 anchor{};
    if (!LoadStrictRecording(argv[4], &recording, &frames)
#if !defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
        || !LoadBoundAnchor(argv[5], recording, frames, &anchor)
#endif
    ) {
        std::fprintf(stderr, "recording/anchor validation failed\n");
        return 3;
    }

    std::uint64_t pid_value = 0, base_value = 0, timeout_value = 0;
    std::uint64_t context_value = 0, main_value = 0, owner_value = 0;
    if (!ParseUnsigned(argv[1], 10, &pid_value) ||
        !ParseUnsigned(argv[2], 16, &base_value) ||
        !ParseUnsigned(argv[3], 10, &timeout_value) ||
        (argc >= 9 && !ParseUnsigned(argv[8], 16, &context_value)) ||
        (argc >= 10 && !ParseUnsigned(argv[9], 16, &main_value)) ||
        (argc == 11 && !ParseUnsigned(argv[10], 16, &owner_value)) ||
        pid_value == 0 || pid_value > INT32_MAX || base_value == 0 ||
        timeout_value < 1000 || timeout_value > 300000) {
        std::fprintf(stderr, "invalid arguments\n");
        return 2;
    }
    const pid_t pid = static_cast<pid_t>(pid_value);
    const auto base = static_cast<std::uintptr_t>(base_value);
    if (!VerifyTargetBuild(pid, base)) return 3;
    std::uintptr_t main_object = 0, final_owner = 0;
    if (!ResolveMainObject(pid, base, static_cast<std::uintptr_t>(main_value),
                           &main_object) ||
        !ResolveFinalOwner(pid, base, static_cast<std::uintptr_t>(owner_value),
                           &final_owner))
        return 3;
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                  static_cast<int>(pid));
#if defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
    int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
#else
    int mem = open(mem_path, O_RDWR | O_CLOEXEC);
#endif
    if (mem < 0) return 4;

    std::uintptr_t context = 0, adapter = 0, world = 0;
    a9tas::vehicle_state_v1::Layout vehicle{};
    a9tas::vehicle_state_v1::BackendLayout backend{};
    if (!ResolvePhysicsContext(pid, mem, base,
                               static_cast<std::uintptr_t>(context_value),
                               &context, &adapter, &world) ||
        !a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle) ||
        !a9tas::vehicle_state_v1::ResolveBackendLayout(mem, base, vehicle,
                                                       &backend)) {
        close(mem);
        return 3;
    }
    const std::uintptr_t delta = main_object + kAccumulatorOffset;
    const std::uintptr_t c98 = final_owner + kC98Offset;
    const std::uintptr_t c9c = final_owner + kC9COffset;
    const std::uintptr_t completion = context + kContextCompletionTokenOffset;
    const std::uintptr_t callback_flags = context + kContextCallbackFlagsOffset;
    const std::uintptr_t f64 = vehicle.physics_base + kCarPhysicsF64Offset;
    const std::uintptr_t world_accumulator = world + kWorldAccumulatorOffset;
    float live_interval = 0.0f;
    std::vector<Mapping> maps;
    if ((delta & 7u) != 0 || (c98 & 7u) != 0 || c9c != c98 + 4 ||
        !ReadMaps(pid, &maps) || !PipelineWritable(maps, delta, 8) ||
        !PipelineWritable(maps, c98, 8) ||
        !PipelineWritable(maps, completion, 8) ||
        !PipelineWritable(maps, callback_flags, 2) ||
        !PipelineWritable(maps, f64, 4) ||
        !PipelineWritable(maps, world_accumulator, 4) ||
        !PipelineWritable(maps, backend.native_pose_address, 64) ||
        !PipelineWritable(maps, backend.native_linear_address, 12) ||
        !ReadExact(mem, context + kContextFixedIntervalOffset, &live_interval,
                   sizeof(live_interval)) ||
        !std::isfinite(live_interval) ||
        static_cast<std::uint32_t>(std::llround(live_interval * 1000000.0)) !=
            kRequiredFixedUs) {
        close(mem);
        return 3;
    }

#if defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
    a9tas::startline_prearm_v1::ProtocolV1 startline{};
    std::int64_t paused_delta = 0;
    if (!ReadExact(mem, delta, &paused_delta, sizeof(paused_delta)) ||
        !a9tas::startline_prearm_v1::Initialize(&startline, paused_delta)
             .accepted) {
        std::fprintf(stderr,
                     "startline paused-zero baseline missing; no attach\n");
        close(mem);
        return 5;
    }
    for (int sample = 0; sample < 7; ++sample) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!ReadExact(mem, delta, &paused_delta, sizeof(paused_delta)) ||
            !a9tas::startline_prearm_v1::
                 ObserveBeforeAttach(&startline, paused_delta).accepted ||
            startline.phase !=
                a9tas::startline_prearm_v1::PhaseV1::kReadyNoAttach) {
            std::fprintf(stderr,
                         "startline baseline changed during validation; "
                         "no attach\n");
            close(mem);
            return 5;
        }
    }
    char ready_path[96]{};
    std::snprintf(ready_path, sizeof(ready_path),
                  "/data/local/tmp/a9tas_startline_ready_%d",
                  static_cast<int>(pid));
    if (access(ready_path, F_OK) == 0 ||
        !CreateStartlineReadyMarker(ready_path, pid, delta)) {
        std::fprintf(stderr, "stale/unwritable startline ready marker\n");
        close(mem);
        return 5;
    }
    std::printf(
        "STARTLINE_READY_NO_ATTACH pid=%d ready=%s paused_zero_samples=%u "
        "target_threads_attached=0 game_writes=0\n",
        static_cast<int>(pid), ready_path,
        startline.paused_zero_observations);
    std::fflush(stdout);
    bool resume_observed = false;
    const std::uint64_t resume_deadline =
        MonotonicNs() + timeout_value * 1000000ULL;
    while (MonotonicNs() < resume_deadline) {
        if (!ReadExact(mem, delta, &paused_delta, sizeof(paused_delta))) break;
        const auto observed =
            a9tas::startline_prearm_v1::ObserveBeforeAttach(&startline,
                                                             paused_delta);
        if (!observed.accepted) break;
        if (observed.permit_attach) {
            resume_observed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    unlink(ready_path);
    close(mem);
    if (!resume_observed) {
        std::fprintf(stderr,
                     "startline resume edge not observed; no attach\n");
        return 5;
    }
    mem = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem < 0) return 4;
    std::printf(
        "STARTLINE_RESUME_OBSERVED delta_us=%" PRId64
        " attach_permission=1 attach_attempts=0 game_writes=0\n",
        startline.resume_delta_us);
    std::fflush(stdout);
#endif

    auto* controller = a9tas::authoritative_natural_v1::Create(
        frames.data(), frames.size(), recording.fixed_interval_us,
        frames.size() - 1);
    if (!controller) {
        close(mem);
        return 3;
    }
    AdvanceResultV1 state =
        a9tas::authoritative_natural_v1::Inspect(controller);
#if defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
    if (!PrimeDirectReplay(controller, &state)) {
        a9tas::authoritative_natural_v1::Destroy(controller);
        close(mem);
        return 3;
    }
#endif
#if defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY)
    float best_anchor_error = std::numeric_limits<float>::infinity();
    std::uint32_t best_anchor_component = UINT32_MAX;
#endif

    ReportHeaderV1 report{};
    std::memcpy(report.magic, kSteeringReportMagic, 8);
    report.version = kSteeringReportVersion;
    report.header_size = sizeof(report);
    report.frame_audit_size = sizeof(FrameAuditV1);
    report.flags = kTargetVerified | kAddressesValidated |
                   kRecordingValidated | kAnchorValidated |
                   kOtherCapabilitiesAbsent;
#if defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
    // In this report variant the legacy anchor bits carry the symmetric
    // startline-prearm and resume-edge proofs. No A9NPA1 comparator is used.
    report.flags |= kAnchorMatched;
    report.search_cycles = state.search_cycles;
    report.matched_search_cycle = state.matched_cycle;
#endif
    std::memcpy(report.build_id, a9tas::unified_tick_v1::kSupportedBuildId, 20);
    report.pid = pid;
    report.library_base = base;
    report.main_object = main_object;
    report.final_owner = final_owner;
    report.physics_context = context;
    report.native_body = backend.native_body;
    report.delta_address = delta;
    report.c98_address = c98;
    report.c9c_address = c9c;
    report.completion_address = completion;
    report.callback_flags_address = callback_flags;
    report.f64_address = f64;
    report.world_accumulator_address = world_accumulator;
    report.fixed_interval_us = recording.fixed_interval_us;
    report.recording_frames = frames.size();

    std::vector<TracedThread> threads;
    std::uint64_t attach_failures = 0;
    const std::size_t initial = AttachNewThreads(
        pid, delta, c98, c9c, world_accumulator, &threads, &attach_failures,
        BoundaryDr7(true));
    report.initial_threads = initial;
    report.ptrace_errors += attach_failures;
    if (initial == 0 || attach_failures != 0) {
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        a9tas::authoritative_natural_v1::Destroy(controller);
        close(mem);
        return 7;
    }
#if defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
    if (!a9tas::startline_prearm_v1::MarkAttached(&startline).accepted) {
        for (auto& thread : threads)
            if (thread.live) ClearAndDetach(thread.tid, thread.stopped);
        a9tas::authoritative_natural_v1::Destroy(controller);
        close(mem);
        return 7;
    }
#endif

    std::printf("AUTHORITATIVE_STEERING_V2_ARMED pid=%d frames=%zu "
                "max_delta=%zu max_pairs=%zu search_writes=0 "
#if defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
                "alignment=startline_first_complete_cycle\n",
#else
                "alignment=bound_physical_anchor\n",
#endif
                static_cast<int>(pid), frames.size(), frames.size(),
                frames.size() * 2);
    std::fflush(stdout);

    std::vector<FrameAuditV1> audits;
    audits.reserve(frames.size());
    FrameAuditV1 pending{};
    RecordingFrameV1 selected{};
    bool selected_valid = false;
    pid_t cycle_tid = 0, post_tid = 0;
    std::uint32_t accounted_threads = 0;
    bool complete = false;
    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns =
        start_ns + timeout_value * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 250000000ULL;
    const auto fault = [&](const char* reason, pid_t tid, unsigned long dr6) {
        std::fprintf(stderr,
                     "authoritative_steering_fault reason=%s event=%" PRIu64
                     " mode=%u stage=%u frame=%u tid=%d owner=%d dr6=0x%lx\n",
                     reason, report.event_count,
                     static_cast<unsigned>(state.mode), state.phase_stage,
                     state.phase_frame_index, static_cast<int>(tid),
                     static_cast<int>(cycle_tid), dr6);
        ++report.semantic_errors;
    };
    const auto feed = [&](EventV1 event, bool comparator = false,
                          bool anchor_match = false, bool payload_equal = false,
                          bool consumed = false, std::uint32_t tick = 0) {
        state = a9tas::authoritative_natural_v1::Advance(
            controller, event, comparator, anchor_match, payload_equal,
            consumed, tick);
        pending.phase_action_or |= state.replay.phase_actions;
        return state.accepted;
    };

    while (MonotonicNs() < deadline_ns && report.read_errors == 0 &&
           report.ptrace_errors == 0 && report.semantic_errors == 0 &&
           report.delta_write_failures == 0 &&
           report.pair_write_failures == 0 && !complete) {
        const std::uint64_t now = MonotonicNs();
        if (now >= next_rescan_ns) {
            std::uint64_t failures = 0;
            report.thread_additions += AttachNewThreads(
                pid, delta, c98, c9c, world_accumulator, &threads, &failures,
                BoundaryDr7(true));
            report.ptrace_errors += failures;
            next_rescan_ns = now + 250000000ULL;
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
                ++accounted_threads;
                if (tid == cycle_tid) fault("owner_exited", tid, 0);
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
            const bool searching = state.mode == ModeV1::kSearching;
            const std::uint32_t wait_world = searching ? 7u : 8u;
            const std::uint32_t waiting = 0u;
            if ((hit & (hit - 1)) != 0) {
                fault("ambiguous_multi_hit", tid, dr6);
            } else if (hit == 8UL && state.phase_stage == wait_world) {
                std::uint32_t bits = 0;
                float value = 0.0f;
                if (!ReadExact(mem, world_accumulator, &bits, sizeof(bits))) {
                    ++report.read_errors;
                } else {
                    std::memcpy(&value, &bits, sizeof(value));
                    if (!std::isfinite(value) || std::fabs(value) > 60.0f ||
                        tid == cycle_tid || pending.deferred_clear_event == 0) {
                        fault("unexpected_world_commit", tid, dr6);
                    } else if (searching) {
                        if (!feed(EventV1::kWorldCommit)) {
                            fault("search_world_rejected", tid, dr6);
                        } else {
                            report.search_cycles = state.search_cycles;
                            if (state.anchor_matched) {
                                report.flags |= kAnchorMatched;
                                report.matched_search_cycle = state.matched_cycle;
                            }
#if defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY)
                            if (state.anchor_matched) {
                                complete = true;
                            } else if (!RearmInputOwner(
                                           tid, post_tid, &threads, delta,
                                           c98, c9c, world_accumulator)) {
                                ++report.ptrace_errors;
                            } else {
                                pending = {};
                                cycle_tid = post_tid = 0;
                            }
#else
                            if (!RearmInputOwner(tid, post_tid, &threads, delta,
                                                 c98, c9c,
                                                 world_accumulator)) {
                                ++report.ptrace_errors;
                            } else {
                                pending = {};
                                cycle_tid = post_tid = 0;
                            }
#endif
                        }
                    } else {
                        const std::uint32_t tick = pending.selected_tick;
                        if (!feed(EventV1::kWorldCommit, false, false, false,
                                  true, tick) ||
                            !state.replay.committed) {
                            fault("replay_world_rejected", tid, dr6);
                        } else {
#if defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
                            if (tick == 0) {
                                const auto tick_zero =
                                    a9tas::startline_prearm_v1::
                                        ObserveCompleteCycle(&startline);
                                if (!tick_zero.accepted ||
                                    !tick_zero.commit_tick_zero) {
                                    fault("startline_tick_zero_rejected", tid,
                                          dr6);
                                }
                            }
                            if (report.semantic_errors == 0) {
#endif
                            pending.commit_tid = tid;
                            pending.world_commit_event = report.event_count;
                            pending.published_tick = state.replay.current_tick - 1;
                            audits.push_back(pending);
                            report.bound_frames = state.replay.committed_frames;
                            if (state.mode == ModeV1::kComplete) {
                                complete = true;
                            } else if (!RearmInputOwner(
                                           tid, post_tid, &threads, delta, c98,
                                           c9c, world_accumulator)) {
                                ++report.ptrace_errors;
                            } else {
                                pending = {};
                                selected_valid = false;
                                cycle_tid = post_tid = 0;
                            }
#if defined(A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT)
                            }
#endif
                        }
                    }
                }
            } else if (hit == 8UL && state.phase_stage == waiting) {
                // Independent world commit outside an open cycle.
            } else if (tid == post_tid) {
                if (tid != cycle_tid) {
                    fault("post_phase_wrong_owner", tid, dr6);
                } else if (hit == 2UL) {
                    std::uint16_t flags = 0;
                    if (!ReadExact(mem, callback_flags, &flags, sizeof(flags))) {
                        ++report.read_errors;
                    } else if (searching && state.phase_stage == 7u &&
                               (flags & 0xffu) != 0) {
                        // A new callback opened before the candidate produced
                        // a world commit. Search is observation-only, so this
                        // prefix is incomplete rather than fatal. Drop it and
                        // re-arm the owner for the next complete cycle. Replay
                        // never takes this recovery path because writes may
                        // already have occurred in that mode.
                        ++report.rejected_search_prefixes;
                        if (!a9tas::authoritative_natural_v1::
                                 RejectSearchPrefix(controller) ||
                            !RearmInputOwner(tid, post_tid, &threads, delta,
                                             c98, c9c, world_accumulator)) {
                            fault("search_world_gap_recovery_failed", tid,
                                  dr6);
                        } else {
                            state = a9tas::authoritative_natural_v1::
                                Inspect(controller);
                            pending = {};
                            cycle_tid = post_tid = 0;
                        }
                    } else if ((flags & 0xffu) != 0) {
                        // 0x0101 while waiting for close is a deferred-list
                        // bookkeeping write, not the close boundary itself.
                        const bool deferred_bookkeeping =
                            flags == 0x0101u &&
                            ((searching && state.phase_stage == 5u) ||
                             (!searching && state.phase_stage == 6u));
                        if (!deferred_bookkeeping) {
                            fault("callback_not_closed", tid, dr6);
                        }
                    } else if ((searching && state.phase_stage == 5u) ||
                               (!searching && state.phase_stage == 6u)) {
                        bool match = false;
#if defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY)
                        float normalized_error = 0.0f;
                        std::uint32_t worst_component = UINT32_MAX;
                        const bool anchor_read =
                            !searching || ReadAnchorMatch(
                                mem, backend.native_body, anchor, &match,
                                &normalized_error, &worst_component);
                        if (searching && anchor_read &&
                            normalized_error < best_anchor_error) {
                            best_anchor_error = normalized_error;
                            best_anchor_component = worst_component;
                            report.matched_search_cycle = state.search_cycles;
                        }
#else
                        const bool anchor_read =
                            !searching || ReadAnchorMatch(
                                mem, backend.native_body, anchor, &match);
#endif
                        if (searching && !anchor_read) {
                            ++report.read_errors;
                        } else if (!feed(EventV1::kCallbackClose, true, match,
                                         false))
                            fault("callback_close_rejected", tid, dr6);
                        else
                            pending.callback_close_event = report.event_count;
                    } else if ((searching && state.phase_stage == 6u) ||
                               (!searching && state.phase_stage == 7u)) {
                        if (!feed(EventV1::kDeferredCallbackClear))
                            fault("deferred_clear_rejected", tid, dr6);
                        else
                            pending.deferred_clear_event = report.event_count;
                    } else if ((searching && state.phase_stage == 7u) ||
                               (!searching && state.phase_stage == 8u)) {
                        // The two callback-list bytes can clear separately.
                        // A duplicate zero after the accepted deferred clear
                        // does not advance the authoritative phase.
                    } else {
                        fault("unexpected_callback_stage", tid, dr6);
                    }
                } else if (hit == 4UL) {
                    std::uint32_t bits = 0;
                    float value = 0.0f;
                    if (!ReadExact(mem, f64, &bits, sizeof(bits))) {
                        ++report.read_errors;
                    } else {
                        std::memcpy(&value, &bits, sizeof(value));
                        if (!std::isfinite(value) ||
                            std::fabs(value) > 1000000.0f ||
                            !feed(EventV1::kF64))
                            fault("f64_rejected", tid, dr6);
                        else
                            pending.f64_event = report.event_count;
                    }
                } else {
                    fault("unexpected_post_phase_dr", tid, dr6);
                }
            } else if (hit == 1UL) {
                std::int64_t observed = 0;
                if (!ReadExact(mem, delta, &observed, sizeof(observed))) {
                    ++report.read_errors;
                } else if (state.phase_stage == waiting && observed == 0) {
                    if (!feed(EventV1::kDeltaZero))
                        fault("waiting_zero_rejected", tid, dr6);
                } else if (state.phase_stage == waiting && observed > 0 &&
                           observed <= 1000000) {
                    if (!feed(EventV1::kDeltaNonzero)) {
                        fault("delta_open_rejected", tid, dr6);
                    } else {
                        cycle_tid = tid;
                        pending = {};
                        pending.cycle_tid = tid;
                        pending.observed_delta_us = observed;
                        pending.delta_event = report.event_count;
                        if (!ReadExact(mem, completion,
                                       &pending.completion_before,
                                       sizeof(pending.completion_before))) {
                            ++report.read_errors;
                        } else if (state.mode == ModeV1::kReplay) {
                            if (!state.replay.packet_selected ||
                                !state.replay.selected_packet_valid) {
                                fault("selected_packet_missing", tid, dr6);
                            } else {
                                selected = state.replay.selected_packet;
                                selected_valid = true;
                                pending.selected_tick = selected.tick;
                                std::memcpy(&pending.steering_bits,
                                            &selected.steering,
                                            sizeof(pending.steering_bits));
                                pending.applied_delta_us = kRequiredFixedUs;
                                ++report.delta_write_attempts;
                                if (!WriteFixedDeltaVerified(
                                        mem, delta, kRequiredFixedUs)) {
                                    ++report.delta_write_failures;
                                    fault("fixed_delta_write_failed", tid, dr6);
                                } else {
                                    ++report.delta_writes;
                                    if (pending.steering_bits != 0)
                                        ++report.nonzero_steering_frames;
                                }
                            }
                        }
                    }
                } else if (state.phase_stage == 1u && tid == cycle_tid &&
                           observed == 0) {
                    if (!feed(EventV1::kDeltaZero) ||
                        (!searching && !state.replay.selection_rolled_back)) {
                        fault("pause_rollback_rejected", tid, dr6);
                    } else {
                        pending = {};
                        selected_valid = false;
                        cycle_tid = 0;
                    }
                } else {
                    fault("unexpected_delta", tid, dr6);
                }
            } else if (hit == 2UL || hit == 4UL) {
                if (state.phase_stage == waiting) {
                    // Incomplete initial/new-thread prefix.
                } else if (tid != cycle_tid) {
                    fault("input_cross_thread", tid, dr6);
                } else {
                    const EventV1 event =
                        hit == 2UL ? EventV1::kC98 : EventV1::kC9C;
                    const bool replay = state.mode == ModeV1::kReplay;
                    if (replay && !selected_valid) {
                        fault("input_without_selected_packet", tid, dr6);
                    } else if (!feed(event)) {
                        fault("input_order", tid, dr6);
                    } else {
                        PairAuditV1* pair_audit =
                            hit == 2UL ? &pending.c98_pair : &pending.c9c_pair;
                        if (replay) {
                            std::uint64_t before = 0, after = 0;
                            if (!ReadExact(mem, c98, &before, sizeof(before))) {
                                ++report.read_errors;
                            } else {
                                const ActivePairPlanV1 plan =
#if defined(A9TAS_AUTHORITATIVE_CONTROLS_STARTLINE_DIRECT)
                                    a9tas::authoritative_controls_v1::PlanPair(
                                        selected, before);
#else
                                    a9tas::authoritative_steering_v1::PlanPair(
                                        selected, before);
#endif
                                ++report.pair_write_attempts;
                                if (!WritePairVerified(mem, c98, plan, &after)) {
                                    ++report.pair_write_failures;
                                    fault("steering_pair_write_failed", tid,
                                          dr6);
                                } else {
                                    ++report.pair_writes;
                                    pair_audit->before = before;
                                    pair_audit->intended = plan.pair_intended;
                                    pair_audit->after = after;
                                }
                            }
                        }
                        if (hit == 2UL) {
                            pending.c98_event = report.event_count;
                        } else {
                            pending.c9c_event = report.event_count;
                            std::uint16_t flags = 0;
                            if (!ReadExact(mem, completion,
                                           &pending.completion_after,
                                           sizeof(pending.completion_after)) ||
                                !ReadExact(mem, callback_flags, &flags,
                                           sizeof(flags))) {
                                ++report.read_errors;
                            } else if (searching &&
                                       (pending.completion_after ==
                                            pending.completion_before ||
                                        (flags & 0xffu) != 1u)) {
                                ++report.rejected_search_prefixes;
                                if (!a9tas::authoritative_natural_v1::
                                        RejectSearchPrefix(controller)) {
                                    fault("search_prefix_reset_failed", tid,
                                          dr6);
                                } else {
                                    state = a9tas::authoritative_natural_v1::
                                        Inspect(controller);
                                    pending = {};
                                    cycle_tid = 0;
                                }
                            } else if (pending.completion_after ==
                                       pending.completion_before ||
                                       (flags & 0xffu) != 1u) {
                                fault("prefix_certificate_failed", tid, dr6);
                            } else if (!feed(EventV1::kPrefixCertified)) {
                                fault("prefix_rejected", tid, dr6);
                            } else {
                                pending.prefix_event = report.event_count;
                                pending.callback_flags_at_prefix = flags;
                                if (!ProgramStoppedThreadV1(
                                        tid, completion, callback_flags, f64,
                                        world_accumulator, PipelineDr7())) {
                                    ++report.ptrace_errors;
                                } else {
                                    post_tid = tid;
                                }
                            }
                        }
                    }
                }
            } else {
                fault("unexpected_input_dr", tid, dr6);
            }
            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
                ++report.ptrace_errors;
            else if (tracked)
                tracked->stopped = false;
        } else {
            fault("unexpected_stop", tid, dr6);
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
    }
    report.final_threads = accounted_threads;
    if (report.final_threads ==
        report.initial_threads + report.thread_additions)
        report.flags |= kCleanDetach;
    a9tas::authoritative_natural_v1::Destroy(controller);
    close(mem);

#if !defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY)
    const std::size_t required_frames = frames.size();
    std::uint32_t expected_nonzero = 0;
    for (const auto& frame : frames) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &frame.steering, sizeof(bits));
        expected_nonzero += bits != 0;
    }
    if (complete && report.bound_frames == required_frames)
        report.flags |= kAuthoritativeComplete;
    if (report.delta_write_attempts == required_frames &&
        report.delta_writes == required_frames &&
        report.delta_write_failures == 0 &&
        report.pair_write_attempts == required_frames * 2 &&
        report.pair_writes == required_frames * 2 &&
        report.pair_write_failures == 0)
        report.flags |= kWritesExact;
#endif
#if defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY)
    std::memcpy(&report.reserved1, &best_anchor_error,
                sizeof(report.reserved1));
    const std::uint32_t required_flags =
        kTargetVerified | kAddressesValidated | kRecordingValidated |
        kAnchorValidated | kAnchorMatched | kOtherCapabilitiesAbsent |
        kCleanDetach;
#else
    const std::uint32_t required_flags =
        kTargetVerified | kAddressesValidated | kRecordingValidated |
        kAnchorValidated | kAnchorMatched | kAuthoritativeComplete |
        kWritesExact | kOtherCapabilitiesAbsent | kCleanDetach;
#endif
#if !defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY)
    bool audits_exact = audits.size() == required_frames;
    for (std::size_t index = 0; audits_exact && index < audits.size(); ++index)
        audits_exact = audits[index].selected_tick == index &&
                       audits[index].published_tick == index &&
                       audits[index].c98_pair.after ==
                           audits[index].c98_pair.intended &&
                       audits[index].c9c_pair.after ==
                           audits[index].c9c_pair.intended;
#endif
#if defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY)
    const bool success =
        complete && report.flags == required_flags && audits.empty() &&
        report.bound_frames == 0 && report.search_cycles > 0 &&
        report.matched_search_cycle < report.search_cycles &&
        report.delta_write_attempts == 0 && report.delta_writes == 0 &&
        report.pair_write_attempts == 0 && report.pair_writes == 0 &&
        report.gameplay_action_calls == 0 &&
        report.physics_correction_writes == 0 && report.read_errors == 0 &&
        report.ptrace_errors == 0 && report.semantic_errors == 0;
#else
    const bool success =
        complete && report.flags == required_flags && audits_exact &&
        report.search_cycles > 0 &&
        report.nonzero_steering_frames == expected_nonzero &&
        report.gameplay_action_calls == 0 &&
        report.physics_correction_writes == 0 && report.read_errors == 0 &&
        report.ptrace_errors == 0 && report.semantic_errors == 0;
#endif
    // Always preserve a diagnostic report after the process has been safely
    // detached. Strict validation still distinguishes complete from partial.
    const bool report_ok = WriteReport(argv[6], report, audits);
#if defined(A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY)
    std::printf(
        "AUTHORITATIVE_SEARCH_ONLY_DONE matched=%u search=%" PRIu64
        " rejected=%" PRIu64 " writes=%" PRIu64 "/%" PRIu64
        " best_error=%g best_component=%u best_cycle=%u"
        " errors=%" PRIu64 "/%" PRIu64 "/%" PRIu64
        " clean=%u report=%s\n",
        complete ? 1u : 0u, report.search_cycles,
        report.rejected_search_prefixes, report.delta_writes,
        report.pair_writes, static_cast<double>(best_anchor_error),
        best_anchor_component, report.matched_search_cycle,
        report.read_errors, report.ptrace_errors, report.semantic_errors,
        (report.flags & kCleanDetach) != 0, argv[6]);
#else
    std::printf(
        "AUTHORITATIVE_STEERING_V2_DONE complete=%u bound=%u/%zu "
        "delta=%" PRIu64 "/%zu pairs=%" PRIu64
        "/%zu search=%" PRIu64 " rejected=%" PRIu64
        " nonzero=%u/%u errors=%" PRIu64
        "/%" PRIu64 "/%" PRIu64 " clean=%u report=%s\n",
        complete ? 1u : 0u, report.bound_frames, required_frames,
        report.delta_writes, required_frames, report.pair_writes,
        required_frames * 2, report.search_cycles,
        report.rejected_search_prefixes, report.nonzero_steering_frames,
        expected_nonzero, report.read_errors,
        report.ptrace_errors, report.semantic_errors,
        (report.flags & kCleanDetach) != 0, argv[6]);
#endif
    return success && report_ok ? 0 : 8;
}
