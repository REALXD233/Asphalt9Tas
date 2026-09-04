// Build-only foundation for the guarded persistent natural-action controller.
// Default executable is inert. The review branch reuses FC-1/FC-2 proven
// process mechanics but is compiled only as an unlinked object.

#include <cstdio>

#ifndef A9TAS_NAL_CONTROLLER_REVIEW
#define A9TAS_NAL_CONTROLLER_REVIEW 0
#endif

#ifndef A9TAS_NAL_ACTION_CONTROLLER_REVIEW
#define A9TAS_NAL_ACTION_CONTROLLER_REVIEW 0
#endif

#ifndef A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW
#define A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW 0
#endif

#ifndef A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW
#define A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW 0
#endif

#if A9TAS_NAL_CONTROLLER_REVIEW != 0 && A9TAS_NAL_CONTROLLER_REVIEW != 1
#error "A9TAS_NAL_CONTROLLER_REVIEW must be 0 or 1"
#endif

#if A9TAS_NAL_ACTION_CONTROLLER_REVIEW != 0 && \
    A9TAS_NAL_ACTION_CONTROLLER_REVIEW != 1
#error "A9TAS_NAL_ACTION_CONTROLLER_REVIEW must be 0 or 1"
#endif

#if A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW != 0 && \
    A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW != 1
#error "A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW must be 0 or 1"
#endif

#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW != 0 && \
    A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW != 1
#error "A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW must be 0 or 1"
#endif

#if A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1 && \
    A9TAS_NAL_ACTION_CONTROLLER_REVIEW != 1
#error "sequence review requires action controller review"
#endif

#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1 && \
    A9TAS_NAL_ACTION_CONTROLLER_REVIEW != 1
#error "external replay review requires action controller review"
#endif

#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1 && \
    A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1
#error "external replay and fixed sequence reviews are mutually exclusive"
#endif

#if A9TAS_NAL_CONTROLLER_REVIEW == 1

#define A9TAS_FC2_LIVE_CANDIDATE 1
#define A9TAS_FC2_CONTROLLER_NO_MAIN 1
#include "fc2_frame_callback_transaction_controller_v1.cpp"
#define A9TAS_STARTLINE_PREARM_PROTOCOL_NO_MAIN
#include "startline_prearm_protocol_v1.cpp"

#if A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1 || \
    A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
#define A9TAS_NAL_ACTION_PAYLOAD_REVIEW 2
#elif A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
#define A9TAS_NAL_ACTION_PAYLOAD_REVIEW 1
#endif
#include "natural_action_lifecycle_elf_resolver_v1.h"
#if A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
#include "natural_action_scheduler_report_v1.h"
#else
#include "natural_action_lifecycle_report_v1.h"
#endif
#include "natural_action_lifecycle_transaction_v1.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

namespace nal_elf = a9tas::natural_action_lifecycle_elf_v1;
#if A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
namespace nal_report = a9tas::natural_action_scheduler_report_v1;
#else
namespace nal_report = a9tas::natural_action_lifecycle_report_v1;
#endif
namespace nal_tx = a9tas::natural_action_lifecycle_transaction_v1;
namespace natural_mailbox = a9tas::natural_action_callback_v1;

#if A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1
constexpr std::uint32_t kReplaySequence[] = {0, 1, 0, 2, 0};
constexpr std::uint32_t kReplaySequenceFrames = std::size(kReplaySequence);
constexpr std::uint32_t kReplaySequenceActionFrames = 2;
constexpr std::uint32_t kReplaySequenceActionCalls = 3;
#endif

#include "natural_action_runtime_identity_v1.h"

using NalControl = nal_report::PayloadControl;
using NalEvidence = nal_report::PayloadEvidence;
using NalReport = nal_report::Report;

static_assert(sizeof(NalControl) == 128);
static_assert(sizeof(NalEvidence) == 256);
static_assert(sizeof(NalReport) == 664);

enum OwnerRejectReason : std::uint32_t {
    kOwnerRejectUnknownWait = 1u << 0,
    kOwnerRejectNonzeroExit = 1u << 1,
    kOwnerRejectSignaled = 1u << 2,
    kOwnerRejectWaitState = 1u << 3,
    kOwnerRejectWrongSignal = 1u << 4,
    kOwnerRejectDr6Read = 1u << 5,
    kOwnerRejectDr0Missing = 1u << 6,
    kOwnerRejectFlagsRead = 1u << 7,
    kOwnerRejectNameRead = 1u << 8,
    kOwnerRejectNamePrefix = 1u << 9,
    kOwnerRejectMapsRead = 1u << 10,
    kOwnerRejectPhysicsIdentity = 1u << 11,
    kOwnerRejectContextVptr = 1u << 12,
    kOwnerRejectCarVptr = 1u << 13,
    kOwnerRejectListRead = 1u << 14,
    kOwnerRejectDispatch = 1u << 15,
    kOwnerRejectDeferred = 1u << 16,
    kOwnerRejectCarActive = 1u << 17,
    kOwnerRejectCarFull = 1u << 18,
    kOwnerRejectDedicated = 1u << 19,
};

bool PayloadWriteExact(int mem, std::uintptr_t address, const void* data,
                       std::size_t size, NalReport* report) {
    if (!report) return false;
    ++report->payload_write_attempts;
    if (WriteExactVerified(mem, address, data, size)) return true;
    ++report->payload_write_failures;
    return false;
}

bool InitialControl(const nal_elf::Layout& payload, NalControl* output) {
    if (!output) return false;
    NalControl control{};
    const char magic[8] = {'A', '9', 'N', 'A', 'L', '1', 0, 0};
    std::memcpy(control.magic, magic, 8);
    control.version = 1;
    control.size = sizeof(control);
    control.dedicated_object = payload.dedicated_object;
    control.dedicated_vptr = payload.dedicated_vptr;
    *output = control;
    return true;
}

bool InitialEvidence(NalEvidence* output) {
    if (!output) return false;
    NalEvidence evidence{};
    const char magic[8] = {'A', '9', 'N', 'A', 'X', '1', 0, 0};
    std::memcpy(evidence.magic, magic, 8);
    evidence.version = 1;
    evidence.size = sizeof(evidence);
    *output = evidence;
    return true;
}

bool PrepareLifecyclePayload(int mem, std::uintptr_t base,
                             std::uintptr_t context, std::uintptr_t car,
                             std::uintptr_t action_owner,
                             std::uintptr_t nitro_state,
                             std::uint32_t session_id,
                             std::uint32_t producer_tid,
                             const nal_elf::Layout& payload,
                             NalReport* report) {
    if (!report || action_owner == 0 || session_id == 0 || producer_tid == 0 ||
        payload.control_size != sizeof(NalControl) ||
        payload.evidence_size != sizeof(NalEvidence) ||
        payload.mailbox_size != sizeof(natural_mailbox::Mailbox))
        return false;
    std::array<std::uint8_t, kShadowSize> table{};
    if (!ReadExact(mem, base + kPrimaryPrefix, table.data(), table.size()) ||
        std::memcmp(table.data(), kExpectedPrefix, sizeof(kExpectedPrefix)) != 0)
        return false;
    for (std::size_t index = 0; index < std::size(kExpectedSlots); ++index) {
        std::uintptr_t slot = 0;
        std::memcpy(&slot, table.data() + kPrefixSize + index * sizeof(slot),
                    sizeof(slot));
        if (slot != base + kExpectedSlots[index]) return false;
    }
    std::uintptr_t context_vptr = 0, add = 0, remove = 0, car_vptr = 0;
    if (!ReadExact(mem, context, &context_vptr, sizeof(context_vptr)) ||
        context_vptr != base + kPhysicsContextVtable ||
        !ReadExact(mem, context_vptr + kAddVslotOffset, &add, sizeof(add)) ||
        !ReadExact(mem, context_vptr + kRemoveVslotOffset, &remove,
                   sizeof(remove)) ||
        add != base + kPhysicsContextAdd ||
        remove != base + kPhysicsContextRemove ||
        !ReadExact(mem, car, &car_vptr, sizeof(car_vptr)) ||
        car_vptr != base + kCarPhysicsPrimaryVtable)
        return false;

    NalControl expected_control{};
    NalEvidence expected_evidence{};
    natural_mailbox::Mailbox expected_mailbox{};
    if (!InitialControl(payload, &expected_control) ||
        !InitialEvidence(&expected_evidence))
        return false;
    natural_mailbox::Initialize(&expected_mailbox);
    NalControl initial_control{};
    NalEvidence initial_evidence{};
    natural_mailbox::Mailbox initial_mailbox{};
    if (!ReadExact(mem, payload.control, &initial_control,
                   sizeof(initial_control)) ||
        !ReadExact(mem, payload.evidence, &initial_evidence,
                   sizeof(initial_evidence)) ||
        !ReadExact(mem, payload.mailbox, &initial_mailbox,
                   sizeof(initial_mailbox)) ||
        std::memcmp(&initial_control, &expected_control,
                    sizeof(initial_control)) != 0 ||
        std::memcmp(&initial_evidence, &expected_evidence,
                    sizeof(initial_evidence)) != 0 ||
        std::memcmp(&initial_mailbox, &expected_mailbox,
                    sizeof(initial_mailbox)) != 0)
        return false;

    const std::uintptr_t shadow_vptr = payload.shadow + kPrefixSize;
    std::memcpy(table.data() + kPrefixSize + kCallbackSlot,
                &payload.bootstrap, sizeof(payload.bootstrap));
    NalControl control = expected_control;
    control.expected_car = car;
    control.original_car_vptr = car_vptr;
    control.shadow_car_vptr = shadow_vptr;
    control.original_car_callback = base + kOriginalCallback;
    control.physics_context = context;
    control.expected_context_vptr = context_vptr;
    control.expected_add_method = add;
    control.expected_remove_method = remove;
    control.session_id = session_id;
    control.expected_producer_tid = producer_tid;
    control.vehicle_owner = action_owner;
#if A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
    control.reserved[0] = nitro_state;
#else
    (void)nitro_state;
#endif

    if (!PayloadWriteExact(mem, payload.shadow, table.data(), table.size(),
                           report) ||
        !PayloadWriteExact(mem, payload.evidence, &expected_evidence,
                           sizeof(expected_evidence), report) ||
        !PayloadWriteExact(mem, payload.mailbox, &expected_mailbox,
                           sizeof(expected_mailbox), report) ||
        !PayloadWriteExact(mem, payload.control, &control, sizeof(control),
                           report))
        return false;
    report->shadow_vptr = shadow_vptr;
    report->session_id = session_id;
    return true;
}

bool ArmZeroCallMailbox(int mem, const nal_elf::Layout& payload,
                        std::uint32_t session_id, NalReport* report) {
    if (!report || session_id == 0) return false;
    natural_mailbox::Mailbox current{};
    if (!ReadExact(mem, payload.mailbox, &current, sizeof(current)) ||
        !natural_mailbox::MailboxValid(current) ||
        current.claimed_sequence != current.completed_sequence ||
        current.claimed_sequence != 0)
        return false;
    const std::uint64_t control =
        (static_cast<std::uint64_t>(session_id) << 1) |
        natural_mailbox::kArmedBit;
    return PayloadWriteExact(
        mem, payload.mailbox + offsetof(natural_mailbox::Mailbox,
                                        session_control),
        &control, sizeof(control), report);
}

bool PublishZeroCall(int mem, const nal_elf::Layout& payload,
                     const natural_mailbox::Command& command,
                     NalReport* report) {
    if (!report || !natural_mailbox::CommandValid(command)) return false;
#if A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1 || \
    A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
    if (command.nitro_activations >
            natural_mailbox::kMaximumNitroActivations ||
        (command.flags & natural_mailbox::kNitroOverrideEnabled) == 0)
        return false;
#elif A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
    if (command.nitro_activations != 1 ||
        (command.flags & natural_mailbox::kNitroOverrideEnabled) == 0)
        return false;
#else
    if (command.nitro_activations != 0) return false;
#endif
    natural_mailbox::Mailbox current{};
    if (!ReadExact(mem, payload.mailbox, &current, sizeof(current)) ||
        !natural_mailbox::MailboxValid(current) ||
        !natural_mailbox::Armed(current.session_control) ||
        natural_mailbox::SessionId(current.session_control) !=
            command.session_id ||
        current.claimed_sequence != current.completed_sequence ||
        command.sequence != current.completed_sequence + 1 ||
        command.replay_frame != current.completed_sequence)
        return false;
    const std::uint32_t slot = static_cast<std::uint32_t>(command.sequence & 1u);
    const std::uintptr_t slot_address =
        payload.mailbox + offsetof(natural_mailbox::Mailbox, slots) +
        slot * sizeof(natural_mailbox::Command);
    const std::uint64_t selector = (command.sequence << 1) | slot;
    const std::int32_t result =
        static_cast<std::int32_t>(natural_mailbox::Result::kPublished);
    // published_selector is the release/publication edge.  Write the
    // diagnostic result before it so a fast zero-call consumer cannot publish
    // kCompleted and then have the host overwrite it with kPublished.
    return PayloadWriteExact(mem, slot_address, &command, sizeof(command),
                             report) &&
           PayloadWriteExact(mem,
               payload.mailbox + offsetof(natural_mailbox::Mailbox, result),
               &result, sizeof(result), report) &&
           PayloadWriteExact(
               mem, payload.mailbox +
                        offsetof(natural_mailbox::Mailbox, published_selector),
               &selector, sizeof(selector), report);
}

bool RequestCleanRemoval(int mem, const nal_elf::Layout& payload,
                         std::uint32_t session_id, NalReport* report) {
    if (!report || session_id == 0) return false;
    natural_mailbox::Mailbox current{};
    if (!ReadExact(mem, payload.mailbox, &current, sizeof(current)) ||
        !natural_mailbox::MailboxValid(current) ||
        !natural_mailbox::Armed(current.session_control) ||
        natural_mailbox::SessionId(current.session_control) != session_id ||
        current.claimed_sequence != current.completed_sequence)
        return false;
    const std::uint64_t disarmed = static_cast<std::uint64_t>(session_id) << 1;
    const std::uint32_t requested = 1;
    return PayloadWriteExact(
               mem, payload.mailbox +
                        offsetof(natural_mailbox::Mailbox, session_control),
               &disarmed, sizeof(disarmed), report) &&
           PayloadWriteExact(
               mem, payload.control + offsetof(NalControl, remove_requested),
               &requested, sizeof(requested), report);
}

bool RegistrationEvidenceComplete(const NalEvidence& evidence) {
    const char magic[8] = {'A', '9', 'N', 'A', 'X', '1', 0, 0};
    return std::memcmp(evidence.magic, magic, 8) == 0 &&
           evidence.version == 1 && evidence.size == sizeof(evidence) &&
           evidence.bootstrap_entries == 1 && evidence.original_calls == 1 &&
           evidence.original_returns == 1 &&
           evidence.registration_attempts == 1 &&
           evidence.registration_returns == 1 && evidence.failures == 0 &&
           evidence.callback_entries == 0 && evidence.idle_entries == 0 &&
           evidence.claimed_commands == 0 && evidence.last_object == 0 &&
           evidence.last_token == 0 &&
           evidence.list_end_before_add != 0 &&
           evidence.list_end_after_add == evidence.list_end_before_add + 16u &&
           evidence.list_active_after_add == evidence.list_active_before_add &&
           evidence.dispatch_before_add == 1 &&
           evidence.deferred_after_add == 1 && evidence.last_status == 1 &&
           evidence.protocol_state == 1;
}

bool ZeroReceiptComplete(const NalEvidence& evidence,
                         const natural_mailbox::Mailbox& mailbox,
                         std::uint64_t frames, std::uint32_t producer_tid,
                         std::uintptr_t dedicated_object) {
#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
    if (frames == 0 || frames > a9tas::unified_tick_v1::kMaximumFrames)
        return false;
#elif A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1
    if (frames == 0 || frames > kReplaySequenceFrames) return false;
    std::uint64_t expected_action_frames = 0;
    std::uint64_t expected_action_calls = 0;
    for (std::uint64_t index = 0; index < frames; ++index) {
        if (kReplaySequence[index] != 0) ++expected_action_frames;
        expected_action_calls += kReplaySequence[index];
    }
#else
    if (frames != 1) return false;
#endif
    const bool common = evidence.failures == 0 &&
           evidence.rejected_nonzero_commands == 0 &&
           evidence.callback_entries >= frames &&
           evidence.claimed_commands == frames &&
           evidence.last_object == dedicated_object &&
           evidence.last_token != 0 &&
           evidence.last_sequence == frames &&
           evidence.last_frame + 1u == frames &&
           evidence.last_tid == producer_tid &&
           evidence.protocol_state == 2 &&
           mailbox.claimed_sequence == frames &&
           mailbox.completed_sequence == frames &&
           mailbox.result ==
               static_cast<std::int32_t>(natural_mailbox::Result::kCompleted);
#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
    const std::uint64_t action_frames =
        evidence.action_command_completions;
    const std::uint64_t action_calls = evidence.action_calls_submitted;
    const bool action_totals =
        evidence.zero_call_completions + action_frames == frames &&
        action_frames <= frames && action_calls >= action_frames &&
        action_calls <= action_frames *
                            natural_mailbox::kMaximumNitroActivations;
    const bool queue_proof =
        action_calls == 0 ||
        (evidence.action_queue_end_before != 0 &&
         evidence.action_queue_end_after != 0 &&
         (evidence.reserved & 0xFFFF000000000000ULL) ==
             0xA9E2000000000000ULL);
    const std::uint32_t last_calls = mailbox.calls_submitted;
    return common && action_totals && queue_proof && last_calls <= 2 &&
           evidence.last_status == (last_calls == 0 ? 2 : 4);
#elif A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1
    const std::uint32_t last_calls = kReplaySequence[frames - 1u];
    return common &&
           evidence.zero_call_completions == frames - expected_action_frames &&
           evidence.action_command_completions == expected_action_frames &&
           evidence.action_calls_submitted == expected_action_calls &&
           (expected_action_calls == 0 ||
            (evidence.action_queue_end_before != 0 &&
             evidence.action_queue_end_after != 0)) &&
           (expected_action_calls == 0 ||
            (evidence.reserved & 0xFFFF000000000000ULL) ==
                0xA9E2000000000000ULL) &&
           evidence.last_status == (last_calls == 0 ? 2 : 4) &&
           mailbox.calls_submitted == last_calls;
#elif A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
    return common && evidence.zero_call_completions == 0 &&
           evidence.action_command_completions == 1 &&
           evidence.action_calls_submitted == 1 &&
           evidence.action_queue_end_before != 0 &&
           evidence.action_queue_end_after != 0 &&
           (evidence.reserved & 0xFFFF000000000000ULL) ==
               0xA9E1000000000000ULL &&
           evidence.last_status == 4 && mailbox.calls_submitted == 1;
#else
    return common && evidence.zero_call_completions == frames &&
           evidence.last_status == 2 && mailbox.calls_submitted == 0;
#endif
}

bool RemovalEvidenceComplete(const NalEvidence& evidence) {
    return evidence.failures == 0 && evidence.removal_attempts == 1 &&
           evidence.removal_returns == 1 &&
           evidence.dispatch_before_remove == 1 &&
           evidence.deferred_after_remove == 1 && evidence.last_status == 3 &&
           evidence.protocol_state == 3;
}

bool WriteLifecycleReportExclusive(const char* path, const NalReport& report) {
    if (!path) return false;
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                O_NOFOLLOW,
                        0600);
    if (fd < 0) return false;
    struct stat state {};
    if (fstat(fd, &state) != 0 || !S_ISREG(state.st_mode) ||
        state.st_nlink != 1) {
        close(fd);
        std::remove(path);
        return false;
    }
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        std::remove(path);
        return false;
    }
    const bool ok = std::fwrite(&report, sizeof(report), 1, file) == 1 &&
                    std::fflush(file) == 0 && fsync(fileno(file)) == 0 &&
                    std::ferror(file) == 0;
    const bool close_ok = std::fclose(file) == 0;
    if (!ok || !close_ok) {
        std::remove(path);
        return false;
    }
    return true;
}

#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
bool CreateExternalReplayReadyMarker(
    const char* path, pid_t pid, std::uint64_t generation,
    std::uint32_t session_id, std::uintptr_t mailbox_address,
    std::uint32_t frame_count) {
    if (!path || pid <= 0 || generation == 0 || session_id == 0 ||
        mailbox_address == 0 || frame_count < 2 || access(path, F_OK) == 0)
        return false;
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                  O_NOFOLLOW,
                        0600);
    if (fd < 0) return false;
    char text[256]{};
    const int length = std::snprintf(
        text, sizeof(text),
        "NAL_EXTERNAL_REPLAY_READY pid=%d generation=%" PRIu64
        " session=%u mailbox=0x%" PRIxPTR
        " frames=%u tracer=0 published=0 completed=0\n",
        pid, generation, session_id, mailbox_address, frame_count);
    bool ok = length > 0 && static_cast<std::size_t>(length) < sizeof(text);
    std::size_t done = 0;
    while (ok && done < static_cast<std::size_t>(length)) {
        const ssize_t count =
            write(fd, text + done, static_cast<std::size_t>(length) - done);
        if (count <= 0)
            ok = false;
        else
            done += static_cast<std::size_t>(count);
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok) unlink(path);
    return ok;
}

bool WaitForExternalReplayMarkerRemoval(const char* path,
                                        std::uint64_t deadline_ns) {
    if (!path) return false;
    while (MonotonicNs() < deadline_ns) {
        if (access(path, F_OK) != 0) return errno == ENOENT;
        usleep(1000);
    }
    return false;
}
#endif

#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
constexpr char kLifecycleAcknowledgement[] =
    "I_ACCEPT_EXTERNAL_FRAME_REPLAY_LIFECYCLE_REVIEW_V1";
#elif A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1
constexpr char kLifecycleAcknowledgement[] =
    "I_ACCEPT_FIVE_FRAME_ACTION_SEQUENCE_REVIEW_V1";
#elif A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
constexpr char kLifecycleAcknowledgement[] =
    "I_ACCEPT_ONE_ACTION_LIFECYCLE_REVIEW_V1";
#else
constexpr char kLifecycleAcknowledgement[] =
    "I_ACCEPT_ZERO_CALL_LIFECYCLE_REVIEW_V1";
#endif

bool ReadProcessGeneration(pid_t pid, std::uint64_t* output) {
    if (!output) return false;
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
    FILE* file = std::fopen(path, "re");
    if (!file) return false;
    char line[4096]{};
    const bool read = std::fgets(line, sizeof(line), file) != nullptr;
    std::fclose(file);
    if (!read) return false;
    char* cursor = std::strrchr(line, ')');
    if (!cursor || cursor[1] != ' ') return false;
    cursor += 2;
    for (int field = 3; field <= 22; ++field) {
        while (*cursor == ' ') ++cursor;
        if (*cursor == '\0') return false;
        char* end = cursor;
        while (*end != '\0' && *end != ' ') ++end;
        if (field == 22) {
            const char saved = *end;
            *end = '\0';
            std::uint64_t value = 0;
            const bool ok = ParseUnsigned(cursor, 10, &value) && value != 0;
            *end = saved;
            if (!ok) return false;
            *output = value;
            return true;
        }
        cursor = end;
    }
    return false;
}

bool EstablishPausedDeltaBaseline(
    int mem, std::uintptr_t delta_address,
    a9tas::startline_prearm_v1::ProtocolV1* protocol) {
    if (mem < 0 || delta_address == 0 || (delta_address & 7u) != 0 ||
        !protocol)
        return false;
    std::int64_t delta_us = 0;
    if (!ReadExact(mem, delta_address, &delta_us, sizeof(delta_us))) {
        std::fprintf(stderr,
                     "NAL_DELTA_BASELINE_FAIL reason=read_initial address=0x%" PRIxPTR "\n",
                     delta_address);
        return false;
    }
    if (!a9tas::startline_prearm_v1::Initialize(protocol, delta_us).accepted) {
        std::fprintf(stderr,
                     "NAL_DELTA_BASELINE_FAIL reason=initial_nonzero value=%" PRId64 "\n",
                     delta_us);
        return false;
    }
    for (int sample = 0; sample < 7; ++sample) {
        usleep(2000);
        if (!ReadExact(mem, delta_address, &delta_us, sizeof(delta_us))) {
            std::fprintf(stderr,
                         "NAL_DELTA_BASELINE_FAIL reason=read_sample sample=%d\n",
                         sample + 2);
            return false;
        }
        const auto observed =
            a9tas::startline_prearm_v1::ObserveBeforeAttach(protocol, delta_us);
        if (!observed.accepted || observed.permit_attach) {
            std::fprintf(stderr,
                         "NAL_DELTA_BASELINE_FAIL reason=sample_changed sample=%d value=%" PRId64 "\n",
                         sample + 2, delta_us);
            return false;
        }
    }
    return protocol->phase ==
               a9tas::startline_prearm_v1::PhaseV1::kReadyNoAttach &&
           protocol->paused_zero_observations == 8;
}

bool WaitForPositiveDelta(
    int mem, std::uintptr_t delta_address,
    a9tas::startline_prearm_v1::ProtocolV1* protocol,
    std::uint64_t deadline_ns) {
    while (MonotonicNs() < deadline_ns) {
        std::int64_t delta_us = 0;
        if (!ReadExact(mem, delta_address, &delta_us, sizeof(delta_us)))
            return false;
        const auto observed =
            a9tas::startline_prearm_v1::ObserveBeforeAttach(protocol, delta_us);
        if (!observed.accepted) return false;
        if (observed.permit_attach && protocol->resume_observations == 1 &&
            protocol->attach_permissions == 1)
            return true;
        usleep(1000);
    }
    return false;
}

bool CreateReadyMarker(const char* path, pid_t pid, std::uint64_t generation,
                       std::uintptr_t delta_address) {
    if (!path || access(path, F_OK) == 0) return false;
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    char text[192]{};
    const int length = std::snprintf(
        text, sizeof(text),
        "NAL_READY_NO_ATTACH pid=%d generation=%" PRIu64
        " delta=0x%" PRIxPTR
        " paused_zero_samples=8 target_threads_attached=0 game_writes=0\n",
        pid, generation, delta_address);
    bool ok = length > 0 && static_cast<std::size_t>(length) < sizeof(text);
    std::size_t done = 0;
    while (ok && done < static_cast<std::size_t>(length)) {
        const ssize_t count =
            write(fd, text + done, static_cast<std::size_t>(length) - done);
        if (count <= 0) ok = false;
        else done += static_cast<std::size_t>(count);
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (!ok) unlink(path);
    return ok;
}

bool WaitForArmMarker(const char* path, pid_t pid, std::uint64_t generation,
                      std::uint64_t deadline_ns) {
    if (!path || pid <= 0 || generation == 0) return false;
    char expected[128]{};
    const int expected_length = std::snprintf(
        expected, sizeof(expected), "NAL_ARM %d %" PRIu64 "\n", pid,
        generation);
    if (expected_length <= 0 ||
        static_cast<std::size_t>(expected_length) >= sizeof(expected))
        return false;

    while (MonotonicNs() < deadline_ns) {
        struct stat before{};
        if (lstat(path, &before) != 0) {
            if (errno != ENOENT) return false;
            usleep(1000);
            continue;
        }
        if (!S_ISREG(before.st_mode) || before.st_nlink != 1 ||
            before.st_size < 0 || before.st_size > 127) {
            usleep(1000);
            continue;
        }
        const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            usleep(1000);
            continue;
        }
        struct stat after{};
        char actual[128]{};
        std::size_t done = 0;
        bool ok = fstat(fd, &after) == 0 && S_ISREG(after.st_mode) &&
                  after.st_nlink == 1 && before.st_dev == after.st_dev &&
                  before.st_ino == after.st_ino &&
                  after.st_size == expected_length;
        while (ok && done < static_cast<std::size_t>(expected_length)) {
            const ssize_t count = read(
                fd, actual + done,
                static_cast<std::size_t>(expected_length) - done);
            if (count <= 0) ok = false;
            else done += static_cast<std::size_t>(count);
        }
        char extra = 0;
        if (ok && read(fd, &extra, 1) != 0) ok = false;
        close(fd);
        if (ok && done == static_cast<std::size_t>(expected_length) &&
            std::memcmp(actual, expected,
                        static_cast<std::size_t>(expected_length)) == 0)
            return true;
        // A pathname can become visible before the shell finishes writing it.
        // Never accept malformed content; allow the exact marker to settle
        // within the bounded arm phase instead of attaching early.
        usleep(1000);
    }
    return false;
}

bool ContinueNalOwner(WatchedThread* owner, NalReport* report) {
    if (!owner || !report || !PokeDebug(owner->tid, 6, 0) ||
        !ContinueThread(owner->tid)) {
        if (report) ++report->ptrace_errors;
        return false;
    }
    owner->stopped = false;
    return true;
}

bool WaitOwnerStop(WatchedThread* owner, int mem,
                   std::uintptr_t callback_flags,
                   std::uint64_t deadline_ns, unsigned long* hits,
                   std::uint16_t* flags, NalReport* report) {
    if (!owner || !hits || !flags || !report) return false;
    while (MonotonicNs() < deadline_ns) {
        int status = 0;
        const pid_t waited = waitpid(owner->tid, &status, __WALL | WNOHANG);
        if (waited == 0) {
            usleep(100);
            continue;
        }
        if (waited < 0) {
            if (errno == EINTR) continue;
            ++report->ptrace_errors;
            return false;
        }
        owner->stopped = WIFSTOPPED(status);
        unsigned long dr6 = 0;
        if (!owner->stopped || WSTOPSIG(status) != SIGTRAP ||
            !PeekDebug(owner->tid, 6, &dr6) || (dr6 & 1u) == 0 ||
            !ReadExact(mem, callback_flags, flags, sizeof(*flags))) {
            ++report->semantic_errors;
            return false;
        }
        *hits = dr6 & 0xFu;
        return true;
    }
    ++report->semantic_errors;
    return false;
}

bool WaitOwnerFlagTransition(WatchedThread* owner, int mem,
                             std::uintptr_t callback_flags,
                             std::uint8_t expected_value,
                             const char* phase,
                             std::uint64_t deadline_ns,
                             NalReport* report) {
    if (!owner || !phase || expected_value > 1 || !report) return false;
    std::uint32_t repeated_state_writes = 0;
    while (MonotonicNs() < deadline_ns) {
        unsigned long hits = 0;
        std::uint16_t flags = 0;
        if (!WaitOwnerStop(owner, mem, callback_flags, deadline_ns, &hits,
                           &flags, report))
            return false;
        const std::uint8_t observed = static_cast<std::uint8_t>(flags & 0xFFu);
        std::fprintf(stderr,
                     "NAL_REGISTRATION_EDGE phase=%s expected=%u observed=%u "
                     "hits=0x%lx repeated=%u\n",
                     phase, expected_value, observed, hits,
                     repeated_state_writes);
        if (hits != 1 || observed > 1) return false;
        if (observed == expected_value) return true;
        // Hardware watches report writes, not value changes. The dispatcher
        // can write its current 0/1 state more than once. Preserve the exact
        // 1->0->1 lifecycle ordering while ignoring bounded same-state writes.
        if (++repeated_state_writes > 64 ||
            !ContinueNalOwner(owner, report))
            return false;
    }
    return false;
}

bool ResumeNalWorkersExcept(std::vector<WatchedThread>* threads,
                            pid_t owner_tid, NalReport* report) {
    if (!threads || owner_tid <= 0 || !report) return false;
    bool owner_found = false;
    // Reuse the proven FC3 release order: validate the complete frozen ledger
    // before continuing any worker, keep the callback owner stopped, then
    // release every other live task before continuing the owner.
    for (const auto& thread : *threads) {
        if (!thread.live) continue;
        if (thread.tid == owner_tid) owner_found = true;
        if (!thread.stopped) {
            std::fprintf(stderr,
                         "NAL_WORKER_RELEASE_FAIL reason=not_stopped tid=%d "
                         "owner=%d\n",
                         thread.tid, owner_tid);
            ++report->ptrace_errors;
            return false;
        }
    }
    if (!owner_found) {
        std::fprintf(stderr,
                     "NAL_WORKER_RELEASE_FAIL reason=owner_missing owner=%d\n",
                     owner_tid);
        ++report->semantic_errors;
        return false;
    }
    for (auto& thread : *threads) {
        if (!thread.live || thread.tid == owner_tid) continue;
        if (!ContinueThread(thread.tid)) {
            std::fprintf(stderr,
                         "NAL_WORKER_RELEASE_FAIL reason=continue tid=%d "
                         "owner=%d errno=%d\n",
                         thread.tid, owner_tid, errno);
            ++report->ptrace_errors;
            return false;
        }
        thread.stopped = false;
    }
    std::fprintf(stderr,
                 "NAL_WORKERS_RELEASED owner=%d ledger=%zu\n", owner_tid,
                 threads->size());
    return true;
}

bool NalTaskGone(pid_t pid, pid_t tid) {
    char path[96]{};
    const int length = std::snprintf(path, sizeof(path), "/proc/%d/task/%d",
                                     static_cast<int>(pid),
                                     static_cast<int>(tid));
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(path))
        return false;
    errno = 0;
    return access(path, F_OK) != 0 && errno == ENOENT;
}

bool RestoreAndDetachNal(pid_t pid, std::vector<WatchedThread>* threads,
                         std::uint32_t* retired, std::uint32_t* detached) {
    if (pid <= 0 || !threads || !retired || !detached) return false;
    bool ok = true;
    *detached = 0;
    for (auto& thread : *threads) {
        if (!thread.live) continue;
        if (!thread.stopped && !StopThread(thread.tid)) {
            if (NalTaskGone(pid, thread.tid)) {
                thread.live = false;
                ++*retired;
                std::fprintf(stderr,
                             "NAL_CLEANUP_RETIRED stage=stop tid=%d retired=%u\n",
                             thread.tid, *retired);
                continue;
            }
            std::fprintf(stderr,
                         "NAL_CLEANUP_FAIL stage=stop tid=%d errno=%d\n",
                         thread.tid, errno);
            ok = false;
            continue;
        }
        thread.stopped = true;
        const bool restored = RestoreDebug(thread.tid, thread.original);
        const bool detached_ok = restored &&
            ptrace(PTRACE_DETACH, thread.tid, nullptr, nullptr) != -1;
        if (!detached_ok) {
            if (NalTaskGone(pid, thread.tid)) {
                ++*retired;
                std::fprintf(stderr,
                             "NAL_CLEANUP_RETIRED stage=detach tid=%d retired=%u\n",
                             thread.tid, *retired);
            } else {
                std::fprintf(stderr,
                             "NAL_CLEANUP_FAIL stage=detach tid=%d restored=%u "
                             "errno=%d\n",
                             thread.tid, restored, errno);
                ok = false;
            }
        } else {
            ++*detached;
        }
        thread.live = false;
        thread.stopped = false;
    }
    return ok;
}

bool AttachLifecycleThreads(pid_t pid, std::uintptr_t flags_address,
                            std::vector<WatchedThread>* threads,
                            std::uint32_t* retired) {
    if (!threads || !retired) return false;
    for (const pid_t tid : ListThreads(pid)) {
        if (FindWatched(threads, tid)) continue;
        WatchedThread thread{};
        if (AttachOne(tid, flags_address, true, &thread)) {
            threads->push_back(thread);
            continue;
        }
        char task_path[96]{};
        std::snprintf(task_path, sizeof(task_path), "/proc/%d/task/%d",
                      static_cast<int>(pid), static_cast<int>(tid));
        if (access(task_path, F_OK) != 0 && errno == ENOENT) {
            ++*retired;
            std::fprintf(stderr,
                         "NAL_ATTACH_RETIRED tid=%d watched=%zu retired=%u\n",
                         tid, threads->size(), *retired);
            continue;
        }
        std::fprintf(stderr,
                     "NAL_ATTACH_FAIL tid=%d watched=%zu errno=%d\n", tid,
                     threads->size(), errno);
        return false;
    }
    return !threads->empty();
}

bool FindInitialOwner(pid_t pid, int mem, std::uintptr_t base,
                      std::uintptr_t context, std::uintptr_t car,
                      std::uintptr_t dedicated, std::uint64_t deadline_ns,
                      std::vector<WatchedThread>* threads,
                       WatchedThread** owner, std::uint32_t* retired,
                       NalReport* report) {
    std::uint64_t next_rescan = MonotonicNs() + 250000000ULL;
    while (MonotonicNs() < deadline_ns && *owner == nullptr) {
        if (MonotonicNs() >= next_rescan) {
            const std::size_t before = threads->size();
            if (!AttachLifecycleThreads(pid, context + kCallbackFlagsOffset,
                                        threads, retired)) {
                ++report->ptrace_errors;
                return false;
            }
            if (threads->size() != before)
                std::fprintf(stderr,
                             "NAL_OWNER_RESCAN added=%zu watched=%zu retired=%u\n",
                             threads->size() - before, threads->size(),
                             *retired);
            next_rescan = MonotonicNs() + 250000000ULL;
        }
        int status = 0;
        const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
        if (tid == 0) {
            usleep(100);
            continue;
        }
        if (tid < 0) {
            if (errno == EINTR) continue;
            ++report->ptrace_errors;
            return false;
        }
        WatchedThread* thread = FindWatched(threads, tid);
        // Same proven rule as FC-1: a clean exit from an unknown TID can be a
        // stale enumerate/seize notification. It owns no restorable debug
        // state and cannot be the stopped callback writer.
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) != 0) {
                report->reject_reasons |= kOwnerRejectNonzeroExit;
                ++report->semantic_errors;
                std::fprintf(stderr,
                             "NAL_OWNER_REJECT reason=0x%x tid=%d status=0x%x\n",
                             report->reject_reasons, tid, status);
                return false;
            }
            if (thread && thread->live) {
                thread->live = false;
                thread->stopped = false;
                ++*retired;
            }
            continue;
        }
        if (!thread || !thread->live) {
            report->reject_reasons |= kOwnerRejectUnknownWait;
            ++report->semantic_errors;
            std::fprintf(stderr,
                         "NAL_OWNER_REJECT reason=0x%x tid=%d status=0x%x\n",
                         report->reject_reasons, tid, status);
            return false;
        }
        if (WIFSIGNALED(status)) {
            report->reject_reasons |= kOwnerRejectSignaled;
            ++report->semantic_errors;
            std::fprintf(stderr,
                         "NAL_OWNER_REJECT reason=0x%x tid=%d status=0x%x\n",
                         report->reject_reasons, tid, status);
            return false;
        }
        if (!WIFSTOPPED(status)) {
            report->reject_reasons |= kOwnerRejectWaitState;
            ++report->semantic_errors;
            std::fprintf(stderr,
                         "NAL_OWNER_REJECT reason=0x%x tid=%d status=0x%x\n",
                         report->reject_reasons, tid, status);
            return false;
        }
        thread->stopped = true;
        unsigned long dr6 = 0;
        std::uint16_t flags = 0;
        const bool signal_ok = WSTOPSIG(status) == SIGTRAP;
        const bool dr6_ok = signal_ok && PeekDebug(tid, 6, &dr6);
        const bool dr0_set = dr6_ok && (dr6 & 1u) != 0;
        const bool flags_ok = dr0_set &&
            ReadExact(mem, context + kCallbackFlagsOffset, &flags,
                      sizeof(flags));
        if (!signal_ok || !dr6_ok || !dr0_set || !flags_ok) {
            if (!signal_ok)
                report->reject_reasons |= kOwnerRejectWrongSignal;
            else if (!dr6_ok)
                report->reject_reasons |= kOwnerRejectDr6Read;
            else if (!dr0_set)
                report->reject_reasons |= kOwnerRejectDr0Missing;
            else
                report->reject_reasons |= kOwnerRejectFlagsRead;
            ++report->semantic_errors;
            std::fprintf(stderr,
                         "NAL_OWNER_REJECT reason=0x%x tid=%d status=0x%x "
                         "dr6=0x%lx\n",
                         report->reject_reasons, tid, status, dr6);
            return false;
        }
        if ((flags & 0xFFu) != 1) {
            if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid)) {
                ++report->ptrace_errors;
                return false;
            }
            thread->stopped = false;
            continue;
        }
        std::string name;
        std::vector<Mapping> maps;
        CallbackListHeader header{};
        std::uintptr_t adapter = 0, world = 0, context_vptr = 0, car_vptr = 0;
        std::uint32_t car_active = UINT32_MAX, car_full = UINT32_MAX;
        std::uint32_t dedicated_full = UINT32_MAX;
        const bool name_read = ReadThreadName(pid, tid, &name);
        const bool name_prefix = name_read && name.rfind("Thread-", 0) == 0;
        const bool maps_read = ReadMaps(pid, &maps);
        const bool physics_ok = maps_read &&
            ValidatePhysicsContext(mem, maps, base, context, &adapter, &world);
        const bool context_vptr_ok =
            ReadExact(mem, context, &context_vptr, sizeof(context_vptr)) &&
            context_vptr == base + kPhysicsContextVtable;
        const bool car_vptr_ok =
            ReadExact(mem, car, &car_vptr, sizeof(car_vptr)) &&
            car_vptr == base + kCarPhysicsPrimaryVtable;
        const bool list_read = maps_read &&
            ReadCallbackList(mem, maps, context + kCallbackListOffset, &header);
        const bool dispatch_ok = list_read && header.dispatching == 1;
        const bool deferred_ok = list_read && header.deferred == 0;
        const bool car_active_ok = list_read &&
            CountObject(mem, header, car, true, &car_active) && car_active == 1;
        const bool car_full_ok = list_read &&
            CountObject(mem, header, car, false, &car_full) && car_full == 1;
        const bool dedicated_ok = list_read &&
            CountObject(mem, header, dedicated, false, &dedicated_full) &&
            dedicated_full == 0;
        if (!name_read || !name_prefix || !maps_read || !physics_ok ||
            !context_vptr_ok || !car_vptr_ok || !list_read || !dispatch_ok ||
            !deferred_ok || !car_active_ok || !car_full_ok || !dedicated_ok) {
            if (!name_read) report->reject_reasons |= kOwnerRejectNameRead;
            if (name_read && !name_prefix)
                report->reject_reasons |= kOwnerRejectNamePrefix;
            if (!maps_read) report->reject_reasons |= kOwnerRejectMapsRead;
            if (maps_read && !physics_ok)
                report->reject_reasons |= kOwnerRejectPhysicsIdentity;
            if (!context_vptr_ok)
                report->reject_reasons |= kOwnerRejectContextVptr;
            if (!car_vptr_ok) report->reject_reasons |= kOwnerRejectCarVptr;
            if (maps_read && !list_read)
                report->reject_reasons |= kOwnerRejectListRead;
            if (list_read && !dispatch_ok)
                report->reject_reasons |= kOwnerRejectDispatch;
            if (list_read && !deferred_ok)
                report->reject_reasons |= kOwnerRejectDeferred;
            if (list_read && !car_active_ok)
                report->reject_reasons |= kOwnerRejectCarActive;
            if (list_read && !car_full_ok)
                report->reject_reasons |= kOwnerRejectCarFull;
            if (list_read && !dedicated_ok)
                report->reject_reasons |= kOwnerRejectDedicated;
            ++report->semantic_errors;
            std::fprintf(
                stderr,
                "NAL_OWNER_REJECT reason=0x%x tid=%d name=%s dispatch=%u "
                "deferred=%u car_active=%u car_full=%u dedicated=%u "
                "car_vptr=0x%" PRIxPTR "\n",
                report->reject_reasons, tid,
                name_read ? name.c_str() : "<unreadable>", header.dispatching,
                header.deferred, car_active, car_full, dedicated_full, car_vptr);
            return false;
        }
        *owner = thread;
    }
    return *owner != nullptr;
}

bool PollForZeroReceipt(int mem, const nal_elf::Layout& payload,
                        std::uint64_t expected_frames,
                        std::uint32_t producer_tid,
                        std::uint64_t deadline_ns, NalReport* report) {
    while (MonotonicNs() < deadline_ns) {
        NalEvidence evidence{};
        natural_mailbox::Mailbox mailbox{};
        if (!ReadExact(mem, payload.evidence, &evidence, sizeof(evidence)) ||
            !ReadExact(mem, payload.mailbox, &mailbox, sizeof(mailbox))) {
            ++report->read_errors;
            return false;
        }
        if (ZeroReceiptComplete(evidence, mailbox, expected_frames, producer_tid,
                                payload.dedicated_object)) {
            report->evidence = evidence;
            report->mailbox_claimed_sequence = mailbox.claimed_sequence;
            report->mailbox_completed_sequence = mailbox.completed_sequence;
#if A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
            report->action_frames = expected_frames;
#else
            report->zero_call_frames = expected_frames;
#endif
            return true;
        }
        if (evidence.failures != 0 || evidence.rejected_nonzero_commands != 0 ||
            mailbox.result < 0) {
            ++report->semantic_errors;
            return false;
        }
        usleep(1000);
    }
    ++report->semantic_errors;
    return false;
}

bool PollForNaturalRemoval(pid_t pid, int mem, std::uintptr_t callback_list,
                           const nal_elf::Layout& payload,
                           std::uint64_t deadline_ns, NalReport* report) {
    while (MonotonicNs() < deadline_ns) {
        NalEvidence evidence{};
        std::vector<Mapping> maps;
        CallbackListHeader header{};
        std::uint32_t count = UINT32_MAX;
        if (!ReadExact(mem, payload.evidence, &evidence, sizeof(evidence)) ||
            !ReadMaps(pid, &maps) ||
            !ReadCallbackList(mem, maps, callback_list, &header)) {
            usleep(1000);
            continue;
        }
        if (evidence.failures != 0) {
            ++report->semantic_errors;
            return false;
        }
        if (RemovalEvidenceComplete(evidence) && header.dispatching == 0 &&
            header.deferred == 0 &&
            CountObject(mem, header, payload.dedicated_object, false, &count) &&
            count == 0) {
            const std::uint64_t entries = evidence.callback_entries;
            usleep(20000);
            NalEvidence stable{};
            if (!ReadExact(mem, payload.evidence, &stable, sizeof(stable)) ||
                stable.callback_entries != entries ||
                !RemovalEvidenceComplete(stable)) {
                ++report->semantic_errors;
                return false;
            }
            report->evidence = stable;
            return true;
        }
        usleep(1000);
    }
    ++report->semantic_errors;
    return false;
}

int RunLifecycleReview(int argc, char** argv) {
#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
    if (argc != 11 || std::strcmp(argv[10], kLifecycleAcknowledgement) != 0) {
        std::fprintf(stderr,
            "usage: %s PID START_TIME LIB_BASE_HEX TIMEOUT_MS REPORT "
            "READY_MARKER ARM_MARKER EXTERNAL_READY_MARKER FRAME_COUNT ACK\n",
            argv[0]);
        return 2;
    }
#else
    if (argc != 9 || std::strcmp(argv[8], kLifecycleAcknowledgement) != 0) {
        std::fprintf(stderr,
            "usage: %s PID START_TIME LIB_BASE_HEX TIMEOUT_MS REPORT "
            "READY_MARKER ARM_MARKER ACK\n", argv[0]);
        return 2;
    }
#endif
    std::uint64_t parsed[4]{};
    const int bases[4] = {10, 10, 16, 10};
    for (int index = 0; index < 4; ++index)
        if (!ParseUnsigned(argv[index + 1], bases[index], &parsed[index]))
            return 2;
#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
    std::uint64_t external_frames_value = 0;
    if (!ParseUnsigned(argv[9], 10, &external_frames_value)) return 2;
    constexpr std::uint64_t kMaximumTimeoutMs = 300000;
#else
    constexpr std::uint64_t kMaximumTimeoutMs = 5000;
#endif
    if (parsed[0] == 0 || parsed[0] > INT32_MAX || parsed[1] == 0 ||
        parsed[2] == 0 || parsed[3] < 500 ||
        parsed[3] > kMaximumTimeoutMs ||
        access(argv[5], F_OK) == 0 || access(argv[6], F_OK) == 0 ||
        access(argv[7], F_OK) == 0
#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
        || access(argv[8], F_OK) == 0 || external_frames_value < 2 ||
        external_frames_value > a9tas::unified_tick_v1::kMaximumFrames
#endif
        )
        return 2;
    const pid_t pid = static_cast<pid_t>(parsed[0]);
    const std::uint64_t generation = parsed[1];
    const std::uintptr_t base = static_cast<std::uintptr_t>(parsed[2]);
    const std::uint64_t timeout_ns = parsed[3] * 1000000ULL;
#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
    const std::uint32_t external_frames =
        static_cast<std::uint32_t>(external_frames_value);
#endif
    NalReport report{};
#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
    const char magic[8] = {'A', '9', 'N', 'A', 'R', '6', 0, 0};
#elif A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1
    const char magic[8] = {'A', '9', 'N', 'A', 'R', '5', 0, 0};
#elif A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
    const char magic[8] = {'A', '9', 'N', 'A', 'S', 'R', '1', 0};
#else
    const char magic[8] = {'A', '9', 'N', 'A', 'L', 'R', '1', 0};
#endif
    std::memcpy(report.magic, magic, 8);
    report.version = 1;
    report.size = sizeof(report);
    report.pid = pid;
    report.process_generation = generation;
    report.game_library_base = base;
    std::uint64_t observed_generation = 0;
    std::uint32_t tracer = UINT32_MAX;
    if (!ReadProcessGeneration(pid, &observed_generation) ||
        observed_generation != generation || !VerifyTargetBuild(pid, base) ||
        !ReadTracerPid(pid, &tracer) || tracer != 0) {
        std::fprintf(stderr, "NAL_PREFLIGHT_FAIL stage=process_identity\n");
        return 3;
    }
    report.flags |= nal_report::kGameIdentityPinned;
    char mem_path[64]{};
    std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem < 0) {
        std::fprintf(stderr, "NAL_PREFLIGHT_FAIL stage=open_read_mem\n");
        return 4;
    }
    nal_elf::Layout payload{};
    const char* payload_failure = nullptr;
    if (!nal_elf::Resolve(pid, mem, &payload, &payload_failure)) {
        std::fprintf(stderr,
                     "NAL_PREFLIGHT_FAIL stage=payload_resolve detail=%s\n",
                     payload_failure ? payload_failure : "unknown");
        close(mem);
        return 3;
    }
    report.payload_load_bias = payload.load_bias;
    report.payload_shadow = payload.shadow;
    report.payload_control = payload.control;
    report.payload_evidence = payload.evidence;
    report.payload_mailbox = payload.mailbox;
    report.dedicated_object = payload.dedicated_object;
    report.dedicated_vptr = payload.dedicated_vptr;
    report.bootstrap = payload.bootstrap;
    report.persistent_consumer = payload.persistent_consumer;
    report.fail_safe_callback = payload.fail_safe_callback;
    std::memcpy(report.payload_sha256, payload.file_sha256, 32);
    std::memcpy(report.payload_build_id, payload.build_id, 20);
    std::uintptr_t context = 0, car = 0;
    if (!ResolveIdentity(pid, mem, base, payload.dedicated_object, &context,
                         &car)) {
        std::fprintf(stderr, "NAL_PREFLIGHT_FAIL stage=physics_identity\n");
        close(mem);
        return 3;
    }
    report.physics_context = context;
    report.callback_list = context + kCallbackListOffset;
    report.callback_flags = context + kCallbackFlagsOffset;
    report.car_physics_state = car;
    report.original_vptr = base + kCarPhysicsPrimaryVtable;
#if A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
    std::vector<Mapping> action_maps;
    NalActionQueueIdentity action_queue{};
    std::uintptr_t action_owner = 0;
    std::uintptr_t physics_owner = 0;
    std::uintptr_t nitro_state = 0;
    if (!ReadMaps(pid, &action_maps) ||
        !ResolveUniqueNalActionOwner(mem, action_maps, base, &action_owner,
                                     &action_queue) ||
        !ResolveFinalOwner(pid, base, 0, &physics_owner) ||
        !ResolveNalNitroState(mem, action_maps, base, physics_owner,
                             &nitro_state) ||
        action_owner != car) {
        std::fprintf(stderr, "NAL_PREFLIGHT_FAIL stage=action_identity\n");
        close(mem);
        return 3;
    }
    std::fprintf(stderr,
                 "NAL_ACTION_IDENTITY owner=0x%" PRIxPTR
                 " car=0x%" PRIxPTR " queue_count=%" PRIu64
                 " direct=%u nitro_state=0x%" PRIxPTR "\n",
                 action_owner, car, action_queue.count,
                 action_queue.direct_mode, nitro_state);
#else
    const std::uintptr_t action_owner = car;
    const std::uintptr_t nitro_state = 0;
#endif
    report.flags |= nal_report::kPayloadIdentityPinned;
    std::uintptr_t main_object = 0;
    a9tas::startline_prearm_v1::ProtocolV1 prearm{};
    if (!ResolveMainObject(pid, base, 0, &main_object) ||
        !EstablishPausedDeltaBaseline(mem, main_object + kAccumulatorOffset,
                                      &prearm)) {
        std::fprintf(stderr,
                     "NAL_PREFLIGHT_FAIL stage=paused_delta_baseline main=0x%" PRIxPTR "\n",
                     main_object);
        close(mem);
        return 3;
    }
    report.ready_no_attach_ns = MonotonicNs();
    report.flags |= nal_report::kReadyNoAttach;
    if (!CreateReadyMarker(argv[6], pid, generation,
                           main_object + kAccumulatorOffset)) {
        std::fprintf(stderr, "NAL_PREFLIGHT_FAIL stage=ready_marker\n");
        close(mem);
        return 4;
    }
    std::printf("NAL_READY_NO_ATTACH pid=%d generation=%" PRIu64
                " delta=0x%" PRIxPTR " paused_zero_samples=8\n", pid,
                generation, main_object + kAccumulatorOffset);
    std::fflush(stdout);
    const std::uint64_t arm_deadline = MonotonicNs() + timeout_ns;
    if (!WaitForArmMarker(argv[7], pid, generation, arm_deadline)) {
        std::fprintf(stderr, "NAL_PREFLIGHT_FAIL stage=arm_marker\n");
        close(mem);
        return 5;
    }
    std::printf("NAL_ARM_ACCEPTED pid=%d generation=%" PRIu64
                " target_threads_attached=0 game_writes=0\n", pid,
                generation);
    std::fflush(stdout);
    const std::uint64_t prearm_deadline = MonotonicNs() + timeout_ns;
    if (!WaitForPositiveDelta(mem, main_object + kAccumulatorOffset, &prearm,
                              prearm_deadline)) {
        close(mem);
        return 5;
    }
    close(mem);
    report.resume_observed_ns = MonotonicNs();
    report.flags |= nal_report::kSingleResumeObserved;

    mem = open(mem_path, O_RDWR | O_CLOEXEC);
    if (mem < 0) return 4;
    std::vector<WatchedThread> threads;
    std::uint32_t retired = 0;
    WatchedThread* owner = nullptr;
    if (!AttachLifecycleThreads(pid, report.callback_flags, &threads,
                                &retired)) {
        ++report.ptrace_errors;
    } else {
        const std::uint64_t owner_deadline = MonotonicNs() + timeout_ns;
        if (!FindInitialOwner(pid, mem, base, context, car,
                              payload.dedicated_object, owner_deadline,
                              &threads, &owner, &retired, &report)) {
            if (report.ptrace_errors == 0 && report.semantic_errors == 0)
                ++report.semantic_errors;
            std::fprintf(stderr,
                         "NAL_OWNER_FAIL watched=%zu retired=%u ptrace=%" PRIu64
                         " semantic=%" PRIu64 "\n",
                         threads.size(), retired, report.ptrace_errors,
                         report.semantic_errors);
        }
    }
    report.initial_threads =
        static_cast<std::uint32_t>(threads.size()) + retired;
    a9tas::fc2_report_v1::Report mechanics{};
    if (owner && report.ptrace_errors == 0 && report.semantic_errors == 0) {
        const pid_t owner_tid = owner->tid;
        if (!FreezeAllExcept(pid, owner_tid, report.callback_flags, &threads)) {
            ++report.ptrace_errors;
        } else {
            owner = FindWatched(&threads, owner_tid);
            report.initial_threads =
                static_cast<std::uint32_t>(threads.size()) + retired;
            report.owner_tid = owner_tid;
            report.flags |= nal_report::kOwnerThreadPinned;
            const std::uint32_t session =
                static_cast<std::uint32_t>((generation ^ owner_tid) | 1u);
            if (!owner || !owner->stopped ||
                !PrepareLifecyclePayload(mem, base, context, car, action_owner,
                                         nitro_state, session,
                                         owner_tid, payload, &report) ||
                !ArmDedicatedWatch(
                    owner_tid, payload.evidence +
#if A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
                        offsetof(NalEvidence, action_command_completions)) ||
#else
                        offsetof(NalEvidence, zero_call_completions)) ||
#endif
                !InstallShadowVptrWhileFrozen(mem, car, report.original_vptr,
                                              report.shadow_vptr, &mechanics)) {
                ++report.semantic_errors;
            } else {
                report.game_write_attempts = mechanics.game_write_attempts;
                report.game_write_failures = mechanics.game_write_failures;
                report.flags |= nal_report::kSingleSwapWritten;
                const std::uint64_t registration_deadline =
                    MonotonicNs() + timeout_ns;
                if (ResumeNalWorkersExcept(&threads, owner_tid, &report) &&
                    ContinueNalOwner(owner, &report)) {
                    if (!WaitOwnerFlagTransition(
                            owner, mem, report.callback_flags, 0,
                            "bootstrap_close", registration_deadline,
                            &report) ||
                        !ContinueNalOwner(owner, &report) ||
                        !WaitOwnerFlagTransition(
                            owner, mem, report.callback_flags, 1,
                            "registration_open", registration_deadline,
                            &report)) {
                        ++report.semantic_errors;
                    } else {
                        NalEvidence evidence{};
                        std::vector<Mapping> maps;
                        CallbackListHeader header{};
                        std::uint32_t count = UINT32_MAX;
                        std::uintptr_t registered_vptr = 0;
                        const bool evidence_read = ReadExact(
                            mem, payload.evidence, &evidence, sizeof(evidence));
                        if (evidence_read) report.evidence = evidence;
                        const bool evidence_ok = evidence_read &&
                            RegistrationEvidenceComplete(evidence);
                        const bool car_vptr_ok =
                            ReadExact(mem, car, &registered_vptr,
                                      sizeof(registered_vptr)) &&
                            registered_vptr == report.original_vptr;
                        const bool maps_ok = ReadMaps(pid, &maps);
                        const bool list_ok = maps_ok && ReadCallbackList(
                            mem, maps, report.callback_list, &header);
                        const bool count_ok = list_ok && CountObject(
                            mem, header, payload.dedicated_object, true,
                            &count) && count == 1;
                        const bool proof_ok = evidence_ok && car_vptr_ok &&
                                              list_ok && count_ok;
                        const bool mailbox_ok = proof_ok && ArmZeroCallMailbox(
                            mem, payload, session, &report);
                        std::fprintf(stderr,
                            "NAL_REGISTRATION_PROOF evidence_read=%u "
                            "evidence_ok=%u car_vptr_ok=%u maps_ok=%u "
                            "list_ok=%u count_ok=%u count=%u mailbox_ok=%u "
                            "bootstrap=%" PRIu64 " registration=%" PRIu64
                            "/%" PRIu64 " failures=%" PRIu64 "\n",
                            evidence_read, evidence_ok, car_vptr_ok, maps_ok,
                            list_ok, count_ok, count, mailbox_ok,
                            evidence.bootstrap_entries,
                            evidence.registration_attempts,
                            evidence.registration_returns, evidence.failures);
                        if (!mailbox_ok) {
                            ++report.semantic_errors;
                        } else {
#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
                            report.registration_proved_ns = MonotonicNs();
                            report.mailbox_armed_ns =
                                report.registration_proved_ns;
                            report.flags |=
                                nal_report::kRegistrationAcknowledged |
                                nal_report::kOriginalVptrAfterRegistration |
                                nal_report::kMailboxArmed;
#else
                            natural_mailbox::Command command{};
                            std::memcpy(command.magic,
                                        natural_mailbox::kCommandMagic, 8);
                            command.version = natural_mailbox::kVersion;
                            command.size = sizeof(command);
                            command.sequence = 1;
                            command.session_id = session;
                            command.replay_frame = 0;
                            command.vehicle_owner = car;
                            command.expected_producer_tid = owner_tid;
#if A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1
                            command.nitro_activations = kReplaySequence[0];
                            command.flags = natural_mailbox::kReplayFrame |
                                            natural_mailbox::kNitroOverrideEnabled;
#elif A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
                            command.nitro_activations = 1;
                            command.flags = natural_mailbox::kReplayFrame |
                                            natural_mailbox::kNitroOverrideEnabled;
#else
                            command.flags = natural_mailbox::kReplayFrame;
#endif
                            command.checksum = natural_mailbox::Checksum(command);
                            if (!PublishZeroCall(mem, payload, command,
                                                 &report)) {
                                ++report.semantic_errors;
                            } else {
                                report.registration_proved_ns = MonotonicNs();
                                report.mailbox_armed_ns = report.registration_proved_ns;
                                report.flags |=
                                    nal_report::kRegistrationAcknowledged |
                                    nal_report::kOriginalVptrAfterRegistration |
                                    nal_report::kMailboxArmed;
                            }
#endif
                        }
                    }
                }
            }
        }
    }
    std::uint32_t detached = 0;
    if (RestoreAndDetachNal(pid, &threads, &retired, &detached) &&
        detached + retired == report.initial_threads) {
        report.final_threads = detached + retired;
        report.flags |= nal_report::kRegisteredDetachClean |
                        nal_report::kFinalDetachClean;
    } else {
        ++report.ptrace_errors;
    }
    bool all_receipts = report.ptrace_errors == 0 &&
                        report.semantic_errors == 0;
#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
    if (all_receipts &&
        CreateExternalReplayReadyMarker(
            argv[8], pid, generation, report.session_id, payload.mailbox,
            external_frames)) {
        std::printf(
            "NAL_EXTERNAL_REPLAY_READY pid=%d generation=%" PRIu64
            " session=%u mailbox=0x%" PRIxPTR " frames=%u tracer=0\n",
            pid, generation, report.session_id, payload.mailbox,
            external_frames);
        std::fflush(stdout);
        const std::uint64_t external_deadline = MonotonicNs() + timeout_ns;
        all_receipts = WaitForExternalReplayMarkerRemoval(
                           argv[8], external_deadline) &&
                       PollForZeroReceipt(
                           mem, payload, external_frames, report.owner_tid,
                           external_deadline, &report);
    } else {
        all_receipts = false;
        ++report.semantic_errors;
    }
#elif A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1
    for (std::uint32_t frame = 0;
         all_receipts && frame < kReplaySequenceFrames; ++frame) {
        if (frame != 0) {
            natural_mailbox::Command command{};
            std::memcpy(command.magic, natural_mailbox::kCommandMagic, 8);
            command.version = natural_mailbox::kVersion;
            command.size = sizeof(command);
            command.sequence = static_cast<std::uint64_t>(frame) + 1u;
            command.session_id = report.session_id;
            command.replay_frame = frame;
            command.vehicle_owner = car;
            command.expected_producer_tid = report.owner_tid;
            command.nitro_activations = kReplaySequence[frame];
            command.flags = natural_mailbox::kReplayFrame |
                            natural_mailbox::kNitroOverrideEnabled;
            command.checksum = natural_mailbox::Checksum(command);
            if (!PublishZeroCall(mem, payload, command, &report)) {
                ++report.semantic_errors;
                all_receipts = false;
                break;
            }
        }
        const std::uint64_t receipt_deadline = MonotonicNs() + timeout_ns;
        all_receipts = PollForZeroReceipt(
            mem, payload, static_cast<std::uint64_t>(frame) + 1u,
            report.owner_tid, receipt_deadline, &report);
    }
#else
    const std::uint64_t receipt_deadline = MonotonicNs() + timeout_ns;
    all_receipts = all_receipts && PollForZeroReceipt(
        mem, payload, 1, report.owner_tid, receipt_deadline, &report);
#endif
    if (all_receipts) {
#if A9TAS_NAL_ACTION_CONTROLLER_REVIEW == 1
        report.action_receipt_ns = MonotonicNs();
        report.flags |= nal_report::kActionReceipt;
#else
        report.zero_receipt_ns = MonotonicNs();
        report.flags |= nal_report::kZeroCallReceipt;
#endif
        if (RequestCleanRemoval(mem, payload, report.session_id, &report)) {
            report.removal_requested_ns = MonotonicNs();
            const std::uint64_t removal_deadline = MonotonicNs() + timeout_ns;
            if (PollForNaturalRemoval(pid, mem, report.callback_list, payload,
                                      removal_deadline, &report)) {
                report.final_proof_ns = MonotonicNs();
                report.cleanup_disposition = static_cast<std::uint32_t>(
                    nal_report::CleanupDisposition::kNaturalRemovalProved);
                report.flags |= nal_report::kRemovalAcknowledged |
                                nal_report::kDedicatedObjectAbsent |
                                nal_report::kCallbackCountStable |
                                nal_report::kOriginalVptrFinal;
            }
        }
    }
    close(mem);
    std::uintptr_t final_vptr = 0;
    mem = open(mem_path, O_RDONLY | O_CLOEXEC);
    if (mem >= 0) {
        if (!ReadExact(mem, car, &final_vptr, sizeof(final_vptr)) ||
            final_vptr != report.original_vptr)
            report.flags &= ~nal_report::kOriginalVptrFinal;
        close(mem);
    }
    observed_generation = 0;
    tracer = UINT32_MAX;
    if (kill(pid, 0) == 0 &&
        ReadProcessGeneration(pid, &observed_generation) &&
        observed_generation == generation && ReadTracerPid(pid, &tracer) &&
        tracer == 0)
        report.flags |= nal_report::kProcessAliveTracerClear;
    report.final_phase = report.flags == nal_report::kRequiredFlags
        ? static_cast<std::uint32_t>(nal_tx::Phase::kClean)
        : static_cast<std::uint32_t>(nal_tx::Phase::kForceStopRequired);
    const bool report_ok = WriteLifecycleReportExclusive(argv[5], report);
    const bool success = report.flags == nal_report::kRequiredFlags &&
                         report.final_phase ==
                             static_cast<std::uint32_t>(nal_tx::Phase::kClean) &&
                         report.game_write_attempts == 1 &&
                         report.game_write_failures == 0 &&
                         report.ptrace_errors == 0 && report.read_errors == 0 &&
                         report.semantic_errors == 0
#if A9TAS_NAL_EXTERNAL_REPLAY_CONTROLLER_REVIEW == 1
                         && report.action_frames == external_frames &&
                         report.evidence.claimed_commands == external_frames &&
                         report.evidence.zero_call_completions +
                                 report.evidence.action_command_completions ==
                             external_frames
#elif A9TAS_NAL_SEQUENCE_CONTROLLER_REVIEW == 1
                         && report.action_frames == kReplaySequenceFrames &&
                         report.evidence.action_command_completions ==
                             kReplaySequenceActionFrames &&
                         report.evidence.action_calls_submitted ==
                             kReplaySequenceActionCalls
#endif
                         ;
    std::printf("NAL_DONE success=%u flags=0x%x owner=%d report=%s\n",
                success, report.flags, report.owner_tid, argv[5]);
    return success && report_ok ? 0 : 8;
}

}  // namespace

int main(int argc, char** argv) {
    return RunLifecycleReview(argc, argv);
}

#else

int main() {
    std::puts("NATURAL_ACTION_LIFECYCLE_CONTROLLER_BUILD_ONLY "
              "runtime=disabled return=-100 device_access=0 attach=0 "
              "game_writes=0 action_calls=0");
    return 100;
}

#endif
