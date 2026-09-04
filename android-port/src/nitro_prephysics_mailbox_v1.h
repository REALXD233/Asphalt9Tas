#pragma once

// Two-stage, fail-closed mailbox for exact pre-physics Nitro delivery.
//
// An external replay controller publishes one immutable command per gameplay
// tick while the MainLoop+0x150 PRE_PHYSICS writer is stopped.  The owner-side
// PhysicsContext_submit_simulation_token hook stages that command only after
// the previous token and completed-frame callbacks have finished.  The
// PhysicsContext_execute_simulation_token hook then claims it exactly once on
// the game-owned physics thread, before the backend/world vslot +0x60 call.
//
// This header installs no hook, touches no game object and calls no game
// function.  Runtime arming belongs to a separately reviewed payload.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace a9tas::nitro_prephysics_v1 {

inline constexpr char kMailboxMagic[8] = {'A', '9', 'N', 'P', 'M', '1', 0, 0};
inline constexpr char kCommandMagic[8] = {'A', '9', 'N', 'P', 'C', '1', 0, 0};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaxActivations = 2;
inline constexpr std::uint32_t kFlagReplayFrame = 1u << 0;
inline constexpr std::uint64_t kSessionArmedBit = 1;
inline constexpr std::uint64_t kExecutingBit = 1ULL << 63;
inline constexpr std::uint64_t kSequenceMask = ~kExecutingBit;

enum class Result : std::int32_t {
    kIdle = 0,
    kPassive = 1,
    kStaged = 2,
    kClaimed = 3,
    kCompleted = 4,
    kNoPublishedCommand = -1,
    kUnstablePublication = -2,
    kInvalidCommand = -3,
    kWrongSession = -4,
    kSequenceRegression = -5,
    kMissingFrameCommand = -6,
    kPreviousTokenPending = -7,
    kExecuteReentry = -8,
    kWrongThread = -9,
    kCompletionMismatch = -10,
    kSemanticFailure = -11,
    kLatchedFault = -12,
};

enum Fault : std::uint32_t {
    kFaultNone = 0,
    kFaultUnstablePublication = 1u << 0,
    kFaultInvalidCommand = 1u << 1,
    kFaultWrongSession = 1u << 2,
    kFaultSequence = 1u << 3,
    kFaultMissingFrame = 1u << 4,
    kFaultPreviousPending = 1u << 5,
    kFaultExecuteReentry = 1u << 6,
    kFaultWrongThread = 1u << 7,
    kFaultCompletion = 1u << 8,
    kFaultSemantic = 1u << 9,
};

// Exactly 64 bytes.  The controller writes an inactive slot in one or more
// operations, then publishes it with one aligned 64-bit selector write.  The
// guest reads the selector twice and verifies the checksum, so a torn slot is
// never accepted as a replay command.
struct alignas(64) Command {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint64_t sequence;
    std::uint32_t session_id;
    std::uint32_t replay_frame;
    std::uint64_t vehicle_owner;
    std::uint32_t expected_frame_tid;
    std::uint32_t activations;
    std::uint32_t flags;
    std::uint32_t reserved;
    std::uint64_t checksum;
};

// The first cache line is the externally readable control/acknowledgement
// page.  Commands occupy two independent cache lines after it.
struct alignas(64) Mailbox {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint64_t published_selector;  // (sequence << 1) | slot
    std::uint64_t session_control;     // (session_id << 1) | armed
    std::uint64_t staged_sequence;
    std::uint64_t completed_sequence;
    std::int32_t stage_result;
    std::int32_t execute_result;
    std::uint32_t calls_completed;
    std::uint32_t fault_flags;
    Command slots[2];
};

static_assert(sizeof(Command) == 64, "Nitro pre-physics command ABI");
static_assert(alignof(Command) == 64, "Nitro command cache-line alignment");
static_assert(offsetof(Command, checksum) == 56, "Nitro command checksum span");
static_assert(offsetof(Mailbox, slots) == 64, "Nitro mailbox control ABI");
static_assert(sizeof(Mailbox) == 192, "Nitro mailbox ABI");
static_assert(alignof(Mailbox) == 64, "Nitro mailbox alignment");

struct alignas(64) RuntimeState {
    // 0=idle, sequence=ready, kExecutingBit|sequence=claimed.
    std::atomic<std::uint64_t> ready_control{0};
    std::atomic<std::uint64_t> last_staged_sequence{0};
    std::atomic<std::uint32_t> fault_latched{0};
    Command staged{};
};

static_assert(sizeof(std::atomic<std::uint64_t>) == sizeof(std::uint64_t),
              "64-bit atomic ABI required");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "lock-free 64-bit atomics required");

inline std::uint64_t Checksum(const Command& command) {
    // FNV-1a is used only as a torn/corrupt publication detector, not as an
    // authenticity mechanism.  Build/object/function validation remains a
    // separate mandatory gate before any semantic call.
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&command);
    std::uint64_t hash = kOffset;
    for (std::size_t i = 0; i < offsetof(Command, checksum); ++i) {
        hash ^= bytes[i];
        hash *= kPrime;
    }
    return hash;
}

inline void InitializeMailbox(Mailbox* mailbox) {
    std::memset(mailbox, 0, sizeof(*mailbox));
    std::memcpy(mailbox->magic, kMailboxMagic, sizeof(kMailboxMagic));
    mailbox->version = kVersion;
    mailbox->size = sizeof(*mailbox);
}

inline bool MailboxIdentityValid(const Mailbox& mailbox) {
    return std::memcmp(mailbox.magic, kMailboxMagic, sizeof(kMailboxMagic)) == 0 &&
           mailbox.version == kVersion && mailbox.size == sizeof(mailbox);
}

inline bool CommandValid(const Command& command) {
    return std::memcmp(command.magic, kCommandMagic, sizeof(kCommandMagic)) == 0 &&
           command.version == kVersion && command.size == sizeof(command) &&
           command.sequence != 0 && command.sequence <= kSequenceMask &&
           command.session_id != 0 && command.expected_frame_tid != 0 &&
           command.activations <= kMaxActivations &&
           command.flags == kFlagReplayFrame && command.reserved == 0 &&
           command.vehicle_owner != 0 && command.checksum == Checksum(command);
}

inline std::uint64_t LoadAcquire(const std::uint64_t* value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

inline void StoreRelease(std::uint64_t* target, std::uint64_t value) {
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

inline void StoreRelease(std::int32_t* target, Result value) {
    __atomic_store_n(target, static_cast<std::int32_t>(value), __ATOMIC_RELEASE);
}

inline void StoreRelease(std::uint32_t* target, std::uint32_t value) {
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

inline void LatchFault(Mailbox* mailbox, RuntimeState* state,
                       std::uint32_t fault) {
    __atomic_fetch_or(&mailbox->fault_flags, fault, __ATOMIC_ACQ_REL);
    state->fault_latched.fetch_or(fault, std::memory_order_acq_rel);
}

inline bool SessionArmed(std::uint64_t control) {
    return (control & kSessionArmedBit) != 0;
}

inline std::uint32_t SessionId(std::uint64_t control) {
    return static_cast<std::uint32_t>(control >> 1);
}

inline Result StageAtSubmit(Mailbox* mailbox, RuntimeState* state) {
    if (!MailboxIdentityValid(*mailbox)) {
        LatchFault(mailbox, state, kFaultInvalidCommand);
        StoreRelease(&mailbox->stage_result, Result::kInvalidCommand);
        return Result::kInvalidCommand;
    }
    const std::uint64_t session = LoadAcquire(&mailbox->session_control);
    if (!SessionArmed(session)) {
        StoreRelease(&mailbox->stage_result, Result::kPassive);
        return Result::kPassive;
    }
    if (state->fault_latched.load(std::memory_order_acquire) != 0) {
        StoreRelease(&mailbox->stage_result, Result::kLatchedFault);
        return Result::kLatchedFault;
    }
    if (state->ready_control.load(std::memory_order_acquire) != 0) {
        LatchFault(mailbox, state, kFaultPreviousPending);
        StoreRelease(&mailbox->stage_result, Result::kPreviousTokenPending);
        return Result::kPreviousTokenPending;
    }

    const std::uint64_t first = LoadAcquire(&mailbox->published_selector);
    if (first == 0) {
        LatchFault(mailbox, state, kFaultMissingFrame);
        StoreRelease(&mailbox->stage_result, Result::kNoPublishedCommand);
        return Result::kNoPublishedCommand;
    }
    const std::uint32_t slot = static_cast<std::uint32_t>(first & 1u);
    const std::uint64_t selector_sequence = first >> 1;
    Command snapshot{};
    std::memcpy(&snapshot, &mailbox->slots[slot], sizeof(snapshot));
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint64_t second = LoadAcquire(&mailbox->published_selector);
    if (first != second) {
        LatchFault(mailbox, state, kFaultUnstablePublication);
        StoreRelease(&mailbox->stage_result, Result::kUnstablePublication);
        return Result::kUnstablePublication;
    }
    if (!CommandValid(snapshot) || snapshot.sequence != selector_sequence) {
        LatchFault(mailbox, state, kFaultInvalidCommand);
        StoreRelease(&mailbox->stage_result, Result::kInvalidCommand);
        return Result::kInvalidCommand;
    }
    if (snapshot.session_id != SessionId(session)) {
        LatchFault(mailbox, state, kFaultWrongSession);
        StoreRelease(&mailbox->stage_result, Result::kWrongSession);
        return Result::kWrongSession;
    }

    const std::uint64_t last =
        state->last_staged_sequence.load(std::memory_order_acquire);
    if (snapshot.sequence <= last) {
        const bool duplicate = snapshot.sequence == last;
        LatchFault(mailbox, state,
                   duplicate ? kFaultMissingFrame : kFaultSequence);
        const Result result = duplicate ? Result::kMissingFrameCommand
                                        : Result::kSequenceRegression;
        StoreRelease(&mailbox->stage_result, result);
        return result;
    }

    state->staged = snapshot;
    state->last_staged_sequence.store(snapshot.sequence,
                                      std::memory_order_release);
    state->ready_control.store(snapshot.sequence, std::memory_order_release);
    StoreRelease(&mailbox->staged_sequence, snapshot.sequence);
    StoreRelease(&mailbox->stage_result, Result::kStaged);
    return Result::kStaged;
}

inline Result ClaimAtExecute(Mailbox* mailbox, RuntimeState* state,
                             std::uint32_t actual_tid, Command* output) {
    std::uint64_t ready = state->ready_control.load(std::memory_order_acquire);
    if (ready == 0) {
        StoreRelease(&mailbox->execute_result, Result::kIdle);
        return Result::kIdle;
    }
    if ((ready & kExecutingBit) != 0) {
        LatchFault(mailbox, state, kFaultExecuteReentry);
        StoreRelease(&mailbox->execute_result, Result::kExecuteReentry);
        return Result::kExecuteReentry;
    }
    const std::uint64_t claimed = ready | kExecutingBit;
    if (!state->ready_control.compare_exchange_strong(
            ready, claimed, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        LatchFault(mailbox, state, kFaultExecuteReentry);
        StoreRelease(&mailbox->execute_result, Result::kExecuteReentry);
        return Result::kExecuteReentry;
    }
    const Command snapshot = state->staged;
    if (snapshot.sequence != (claimed & kSequenceMask)) {
        LatchFault(mailbox, state, kFaultCompletion);
        StoreRelease(&mailbox->execute_result, Result::kCompletionMismatch);
        state->ready_control.store(0, std::memory_order_release);
        return Result::kCompletionMismatch;
    }
    if (snapshot.expected_frame_tid != actual_tid) {
        LatchFault(mailbox, state, kFaultWrongThread);
        StoreRelease(&mailbox->calls_completed, 0);
        StoreRelease(&mailbox->completed_sequence, snapshot.sequence);
        StoreRelease(&mailbox->execute_result, Result::kWrongThread);
        state->ready_control.store(0, std::memory_order_release);
        return Result::kWrongThread;
    }
    *output = snapshot;
    StoreRelease(&mailbox->execute_result, Result::kClaimed);
    return Result::kClaimed;
}

inline Result CompleteExecution(Mailbox* mailbox, RuntimeState* state,
                                std::uint64_t sequence,
                                std::uint32_t calls_completed,
                                bool semantic_ok) {
    const std::uint64_t expected = kExecutingBit | sequence;
    if (state->ready_control.load(std::memory_order_acquire) != expected ||
        sequence == 0 || calls_completed > kMaxActivations ||
        (semantic_ok && calls_completed != state->staged.activations)) {
        LatchFault(mailbox, state, kFaultCompletion);
        StoreRelease(&mailbox->execute_result, Result::kCompletionMismatch);
        state->ready_control.store(0, std::memory_order_release);
        return Result::kCompletionMismatch;
    }
    const Result result = semantic_ok ? Result::kCompleted
                                      : Result::kSemanticFailure;
    if (!semantic_ok) LatchFault(mailbox, state, kFaultSemantic);
    StoreRelease(&mailbox->calls_completed, calls_completed);
    StoreRelease(&mailbox->completed_sequence, sequence);
    StoreRelease(&mailbox->execute_result, result);
    state->ready_control.store(0, std::memory_order_release);
    return result;
}

}  // namespace a9tas::nitro_prephysics_v1
