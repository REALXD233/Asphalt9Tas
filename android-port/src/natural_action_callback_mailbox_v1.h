#pragma once

// Pure mailbox contract for faithful, game-owned action submission.
//
// The host publishes exactly one immutable command for each authoritative
// replay frame after selecting that frame.  A naturally invoked
// PhysicsContext callback claims the command on the certified producer TID,
// calls the game's existing action-dispatch entry 0/1/2 times, and records a
// completion receipt before returning to the game's next-token submission.
//
// This header contains no process access, hook installation, game address,
// function pointer, or input synthesis.  Runtime transport and the semantic
// action call remain separately reviewed gates.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace a9tas::natural_action_callback_v1 {

inline constexpr char kMailboxMagic[8] = {'A', '9', 'N', 'A', 'M', '1', 0, 0};
inline constexpr char kCommandMagic[8] = {'A', '9', 'N', 'A', 'C', '1', 0, 0};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaximumNitroActivations = 2;
inline constexpr std::uint64_t kArmedBit = 1;

enum CommandFlag : std::uint32_t {
    kReplayFrame = 1u << 0,
    kNitroOverrideEnabled = 1u << 1,
};

enum class Result : std::int32_t {
    kIdle = 0,
    kPassive = 1,
    kPublished = 2,
    kClaimed = 3,
    kCompleted = 4,
    kInvalidMailbox = -1,
    kInvalidCommand = -2,
    kWrongSession = -3,
    kWrongProducerThread = -4,
    kSequenceError = -5,
    kPreviousFramePending = -6,
    kCallbackReentry = -7,
    kCompletionMismatch = -8,
    kSemanticFailure = -9,
    kLatchedFault = -10,
};

struct alignas(64) Command {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint64_t sequence;
    std::uint32_t session_id;
    std::uint32_t replay_frame;
    std::uint64_t vehicle_owner;
    std::uint32_t expected_producer_tid;
    std::uint32_t nitro_activations;
    std::uint32_t flags;
    std::uint32_t reserved;
    std::uint64_t checksum;
};

// One control cache line followed by two immutable command cache lines.
struct alignas(64) Mailbox {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint64_t session_control;     // (session_id << 1) | armed
    std::uint64_t published_selector;  // (sequence << 1) | slot
    std::uint64_t claimed_sequence;
    std::uint64_t completed_sequence;
    std::uint32_t claimed_frame;
    std::uint32_t completed_frame;
    std::uint32_t calls_submitted;
    std::int32_t result;
    Command slots[2];
};

struct RuntimeState {
    std::atomic<std::uint64_t> last_claimed_sequence{0};
    std::atomic<std::uint32_t> fault_latched{0};
    std::atomic<std::uint32_t> callback_active{0};
    Command claimed{};
};

static_assert(sizeof(Command) == 64, "natural action command ABI");
static_assert(alignof(Command) == 64, "natural action command alignment");
static_assert(offsetof(Command, checksum) == 56,
              "natural action checksum span");
static_assert(offsetof(Mailbox, slots) == 64,
              "natural action control ABI");
static_assert(sizeof(Mailbox) == 192, "natural action mailbox ABI");
static_assert(alignof(Mailbox) == 64, "natural action mailbox alignment");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "64-bit lock-free atomics required");

inline std::uint64_t Checksum(const Command& command) {
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&command);
    std::uint64_t value = kOffset;
    for (std::size_t index = 0; index < offsetof(Command, checksum); ++index) {
        value ^= bytes[index];
        value *= kPrime;
    }
    return value;
}

inline void StoreRelease(std::uint64_t* target, std::uint64_t value) {
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

inline void StoreRelease(std::uint32_t* target, std::uint32_t value) {
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

inline void StoreRelease(std::int32_t* target, Result value) {
    __atomic_store_n(target, static_cast<std::int32_t>(value),
                     __ATOMIC_RELEASE);
}

inline std::uint64_t LoadAcquire(const std::uint64_t* source) {
    return __atomic_load_n(source, __ATOMIC_ACQUIRE);
}

inline std::uint32_t SessionId(std::uint64_t control) {
    return static_cast<std::uint32_t>(control >> 1);
}

inline bool Armed(std::uint64_t control) {
    return (control & kArmedBit) != 0;
}

inline void Initialize(Mailbox* mailbox) {
    std::memset(mailbox, 0, sizeof(*mailbox));
    std::memcpy(mailbox->magic, kMailboxMagic, sizeof(kMailboxMagic));
    mailbox->version = kVersion;
    mailbox->size = sizeof(*mailbox);
}

inline bool MailboxValid(const Mailbox& mailbox) {
    return std::memcmp(mailbox.magic, kMailboxMagic, sizeof(kMailboxMagic)) ==
               0 &&
           mailbox.version == kVersion && mailbox.size == sizeof(mailbox);
}

inline bool CommandValid(const Command& command) {
    const std::uint32_t allowed_flags =
        kReplayFrame | kNitroOverrideEnabled;
    const bool nitro_enabled =
        (command.flags & kNitroOverrideEnabled) != 0;
    return std::memcmp(command.magic, kCommandMagic,
                       sizeof(kCommandMagic)) == 0 &&
           command.version == kVersion && command.size == sizeof(command) &&
           command.sequence != 0 && command.session_id != 0 &&
           command.vehicle_owner != 0 && command.expected_producer_tid != 0 &&
           command.nitro_activations <= kMaximumNitroActivations &&
           (command.flags & kReplayFrame) != 0 &&
           (command.flags & ~allowed_flags) == 0 && command.reserved == 0 &&
           (nitro_enabled || command.nitro_activations == 0) &&
           command.checksum == Checksum(command);
}

inline void LatchFault(Mailbox* mailbox, RuntimeState* state, Result result) {
    state->fault_latched.store(1, std::memory_order_release);
    StoreRelease(&mailbox->result, result);
}

// Host-side reference publication.  A real external-memory publisher must
// preserve this inactive-slot-then-selector order and the same validations.
inline Result Publish(Mailbox* mailbox, const Command& command) {
    if (!MailboxValid(*mailbox)) return Result::kInvalidMailbox;
    const std::uint64_t session = LoadAcquire(&mailbox->session_control);
    if (!Armed(session)) return Result::kPassive;
    if (!CommandValid(command)) return Result::kInvalidCommand;
    if (command.session_id != SessionId(session)) return Result::kWrongSession;
    if (LoadAcquire(&mailbox->claimed_sequence) !=
        LoadAcquire(&mailbox->completed_sequence))
        return Result::kPreviousFramePending;
    const std::uint64_t completed = LoadAcquire(&mailbox->completed_sequence);
    if (command.sequence != completed + 1 ||
        command.replay_frame != completed)
        return Result::kSequenceError;
    const std::uint32_t slot = static_cast<std::uint32_t>(command.sequence & 1);
    std::memcpy(&mailbox->slots[slot], &command, sizeof(command));
    // The selector is the sole publication edge.  Set the diagnostic state
    // before exposing the new sequence so a fast consumer cannot complete the
    // command and then have kCompleted overwritten by a late host store.
    StoreRelease(&mailbox->result, Result::kPublished);
    std::atomic_thread_fence(std::memory_order_release);
    StoreRelease(&mailbox->published_selector,
                 (command.sequence << 1) | slot);
    return Result::kPublished;
}

inline Result ClaimAtNaturalCallback(Mailbox* mailbox, RuntimeState* state,
                                     std::uint32_t actual_producer_tid,
                                     Command* output) {
    if (!mailbox || !state || !output || !MailboxValid(*mailbox))
        return Result::kInvalidMailbox;
    const std::uint64_t session = LoadAcquire(&mailbox->session_control);
    if (!Armed(session)) return Result::kPassive;
    if (state->fault_latched.load(std::memory_order_acquire) != 0)
        return Result::kLatchedFault;
    const std::uint64_t first = LoadAcquire(&mailbox->published_selector);
    if (first == 0 ||
        (first >> 1) ==
            state->last_claimed_sequence.load(std::memory_order_acquire))
        return Result::kIdle;
    const std::uint32_t slot = static_cast<std::uint32_t>(first & 1);
    Command snapshot{};
    std::memcpy(&snapshot, &mailbox->slots[slot], sizeof(snapshot));
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint64_t second = LoadAcquire(&mailbox->published_selector);
    if (first != second || !CommandValid(snapshot) ||
        snapshot.sequence != (first >> 1)) {
        LatchFault(mailbox, state, Result::kInvalidCommand);
        return Result::kInvalidCommand;
    }
    if (snapshot.session_id != SessionId(session)) {
        LatchFault(mailbox, state, Result::kWrongSession);
        return Result::kWrongSession;
    }
    const std::uint64_t last =
        state->last_claimed_sequence.load(std::memory_order_acquire);
    if (snapshot.sequence != last + 1 || snapshot.replay_frame != last) {
        LatchFault(mailbox, state, Result::kSequenceError);
        return Result::kSequenceError;
    }
    if (snapshot.expected_producer_tid != actual_producer_tid) {
        LatchFault(mailbox, state, Result::kWrongProducerThread);
        return Result::kWrongProducerThread;
    }
    std::uint32_t expected_inactive = 0;
    if (!state->callback_active.compare_exchange_strong(
            expected_inactive, 1, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        LatchFault(mailbox, state, Result::kCallbackReentry);
        return Result::kCallbackReentry;
    }
    state->claimed = snapshot;
    state->last_claimed_sequence.store(snapshot.sequence,
                                       std::memory_order_release);
    StoreRelease(&mailbox->claimed_frame, snapshot.replay_frame);
    StoreRelease(&mailbox->claimed_sequence, snapshot.sequence);
    StoreRelease(&mailbox->result, Result::kClaimed);
    *output = snapshot;
    return Result::kClaimed;
}

inline Result CompleteNaturalCallback(Mailbox* mailbox, RuntimeState* state,
                                      std::uint64_t sequence,
                                      std::uint32_t calls_submitted,
                                      bool semantic_ok) {
    if (!mailbox || !state ||
        state->callback_active.load(std::memory_order_acquire) != 1 ||
        state->claimed.sequence != sequence || sequence == 0 ||
        calls_submitted > kMaximumNitroActivations ||
        (semantic_ok &&
         calls_submitted != state->claimed.nitro_activations)) {
        if (mailbox && state)
            LatchFault(mailbox, state, Result::kCompletionMismatch);
        return Result::kCompletionMismatch;
    }
    if (!semantic_ok) {
        LatchFault(mailbox, state, Result::kSemanticFailure);
        state->callback_active.store(0, std::memory_order_release);
        return Result::kSemanticFailure;
    }
    StoreRelease(&mailbox->calls_submitted, calls_submitted);
    StoreRelease(&mailbox->completed_frame, state->claimed.replay_frame);
    StoreRelease(&mailbox->completed_sequence, sequence);
    StoreRelease(&mailbox->result, Result::kCompleted);
    state->callback_active.store(0, std::memory_order_release);
    return Result::kCompleted;
}

inline bool CompletionMatches(const Mailbox& mailbox,
                              std::uint64_t sequence,
                              std::uint32_t replay_frame,
                              std::uint32_t calls_submitted) {
    return MailboxValid(mailbox) && sequence != 0 &&
           LoadAcquire(&mailbox.completed_sequence) == sequence &&
           __atomic_load_n(&mailbox.completed_frame, __ATOMIC_ACQUIRE) ==
               replay_frame &&
           __atomic_load_n(&mailbox.calls_submitted, __ATOMIC_ACQUIRE) ==
               calls_submitted &&
           __atomic_load_n(&mailbox.result, __ATOMIC_ACQUIRE) ==
               static_cast<std::int32_t>(Result::kCompleted);
}

}  // namespace a9tas::natural_action_callback_v1
