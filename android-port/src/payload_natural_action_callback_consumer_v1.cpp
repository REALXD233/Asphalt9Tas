// Passive ARM64 natural-callback consumer for the action mailbox.
//
// This artifact exports payload-owned mailbox/object/evidence storage but has
// no registration or installation entry.  Its arm export is constant -100.
// The callback can acknowledge skip-Nitro or enabled-zero commands only;
// nonzero activation counts fail closed because real action execution is
// compile-time absent from this build.

#include "natural_action_callback_mailbox_v1.h"

#include <atomic>
#include <cstdint>
#include <sys/syscall.h>
#include <unistd.h>

namespace mailbox = a9tas::natural_action_callback_v1;

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_natural_action_callback_consumer_v1(void*, const std::uint64_t*);

namespace {

constexpr bool kActionExecutionCompiled = false;
constexpr std::int32_t kRejectedBuildOnly = -100;
constexpr std::int32_t kUnexpectedObject = -201;

struct DedicatedObject {
    std::uintptr_t vptr;
};

struct alignas(64) Evidence {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint64_t callback_entries;
    std::uint64_t idle_entries;
    std::uint64_t claimed_commands;
    std::uint64_t zero_call_completions;
    std::uint64_t rejected_nonzero_commands;
    std::uint64_t failures;
    std::uint64_t last_sequence;
    std::uint32_t last_frame;
    std::uint32_t last_tid;
    std::uint32_t last_flags;
    std::uint32_t last_activations;
    std::int32_t last_result;
    std::uint32_t reserved0;
    std::uint64_t reserved[4];
};

static_assert(sizeof(DedicatedObject) == sizeof(void*),
              "natural action dedicated object ABI");
static_assert(sizeof(Evidence) == 128,
              "natural action callback evidence ABI");
static_assert(!kActionExecutionCompiled,
              "observe-only consumer must not compile action execution");

__attribute__((noinline, visibility("hidden"))) std::int64_t
UnexpectedSlot(void*, const std::uint64_t*) {
    return 0;
}

// Itanium address point is +2 qwords; callback-list slot +0x10 is index 4.
alignas(64) std::uintptr_t g_dedicated_vtable[5] = {
    0,
    0,
    reinterpret_cast<std::uintptr_t>(&UnexpectedSlot),
    reinterpret_cast<std::uintptr_t>(&UnexpectedSlot),
    reinterpret_cast<std::uintptr_t>(
        &a9tas_natural_action_callback_consumer_v1),
};

alignas(64) DedicatedObject g_dedicated_object = {
    reinterpret_cast<std::uintptr_t>(&g_dedicated_vtable[2]),
};
alignas(64) mailbox::Mailbox g_mailbox{};
alignas(64) mailbox::RuntimeState g_runtime{};
alignas(64) Evidence g_evidence = {
    .magic = {'A', '9', 'N', 'A', 'E', '1', 0, 0},
    .version = 1,
    .size = sizeof(Evidence),
    .callback_entries = 0,
    .idle_entries = 0,
    .claimed_commands = 0,
    .zero_call_completions = 0,
    .rejected_nonzero_commands = 0,
    .failures = 0,
    .last_sequence = 0,
    .last_frame = 0,
    .last_tid = 0,
    .last_flags = 0,
    .last_activations = 0,
    .last_result = 0,
    .reserved0 = 0,
    .reserved = {0, 0, 0, 0},
};

void Add(std::uint64_t* value) {
    __atomic_add_fetch(value, 1u, __ATOMIC_RELAXED);
}

void SetResult(std::int32_t value) {
    __atomic_store_n(&g_evidence.last_result, value, __ATOMIC_RELEASE);
}

__attribute__((constructor)) void InitializePassiveConsumer() {
    mailbox::Initialize(&g_mailbox);
}

}  // namespace

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_natural_action_callback_consumer_v1(void* object,
                                          const std::uint64_t*) {
    Add(&g_evidence.callback_entries);
    if (object != &g_dedicated_object) {
        Add(&g_evidence.failures);
        SetResult(kUnexpectedObject);
        return 0;
    }
    const auto actual_tid = static_cast<std::uint32_t>(syscall(__NR_gettid));
    mailbox::Command command{};
    const mailbox::Result claimed = mailbox::ClaimAtNaturalCallback(
        &g_mailbox, &g_runtime, actual_tid, &command);
    if (claimed == mailbox::Result::kIdle ||
        claimed == mailbox::Result::kPassive) {
        Add(&g_evidence.idle_entries);
        SetResult(static_cast<std::int32_t>(claimed));
        return 0;
    }
    if (claimed != mailbox::Result::kClaimed) {
        Add(&g_evidence.failures);
        SetResult(static_cast<std::int32_t>(claimed));
        return 0;
    }
    Add(&g_evidence.claimed_commands);
    g_evidence.last_sequence = command.sequence;
    g_evidence.last_frame = command.replay_frame;
    g_evidence.last_tid = actual_tid;
    g_evidence.last_flags = command.flags;
    g_evidence.last_activations = command.nitro_activations;

    if (command.nitro_activations != 0) {
        Add(&g_evidence.rejected_nonzero_commands);
        Add(&g_evidence.failures);
        const mailbox::Result result = mailbox::CompleteNaturalCallback(
            &g_mailbox, &g_runtime, command.sequence, 0, false);
        SetResult(static_cast<std::int32_t>(result));
        return 0;
    }
    const mailbox::Result result = mailbox::CompleteNaturalCallback(
        &g_mailbox, &g_runtime, command.sequence, 0, true);
    if (result == mailbox::Result::kCompleted) {
        Add(&g_evidence.zero_call_completions);
    } else {
        Add(&g_evidence.failures);
    }
    SetResult(static_cast<std::int32_t>(result));
    return 0;
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_natural_action_callback_arm_v1() {
    return kRejectedBuildOnly;
}

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_natural_action_callback_protocol_v1() {
    return 1;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_natural_action_callback_mailbox_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_mailbox);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_natural_action_callback_evidence_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_evidence);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_natural_action_callback_object_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_dedicated_object);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_natural_action_callback_vtable_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_dedicated_vtable[2]);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_natural_action_callback_mailbox_size_v1 = sizeof(g_mailbox);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_natural_action_callback_evidence_size_v1 = sizeof(g_evidence);
