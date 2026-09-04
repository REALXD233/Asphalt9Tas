// Build-only persistent natural-callback lifecycle payload.
//
// A future host transaction may install the one-frame bootstrap with the
// already proven FC-1 shadow-vptr mechanism.  The bootstrap restores/calls the
// original CarPhysicsState callback, then uses PhysicsContext's own deferred
// add method to register a persistent payload-owned consumer.  Removal is
// requested through payload-owned control and is performed by the consumer in
// a later natural callback through the game's deferred remove method.
//
// The default build has no real action execution.  A separately named,
// local-review-only build may compile the recovered game-owned scheduler entry;
// it still has a constant -100 arm export and no deployment/controller path.

#ifndef A9TAS_NAL_ACTION_EXECUTE
#define A9TAS_NAL_ACTION_EXECUTE 0
#endif

#ifndef A9TAS_NAL_REPLAY_EXECUTE
#define A9TAS_NAL_REPLAY_EXECUTE 0
#endif

#if A9TAS_NAL_ACTION_EXECUTE != 0 && A9TAS_NAL_ACTION_EXECUTE != 1
#error "A9TAS_NAL_ACTION_EXECUTE must be 0 or 1"
#endif

#if A9TAS_NAL_REPLAY_EXECUTE != 0 && A9TAS_NAL_REPLAY_EXECUTE != 1
#error "A9TAS_NAL_REPLAY_EXECUTE must be 0 or 1"
#endif

#if A9TAS_NAL_REPLAY_EXECUTE == 1 && A9TAS_NAL_ACTION_EXECUTE != 1
#error "per-frame natural action replay requires action execution"
#endif

#include "natural_action_callback_mailbox_v1.h"
#include "natural_action_replay_transport_v1.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sys/syscall.h>
#include <unistd.h>

namespace mailbox = a9tas::natural_action_callback_v1;
namespace replay = a9tas::natural_action_replay_v1;

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_natural_action_persistent_consumer_v1(void*, const std::uint64_t*);

namespace {

constexpr bool kActionExecutionCompiled = A9TAS_NAL_ACTION_EXECUTE == 1;
constexpr bool kReplayExecutionCompiled = A9TAS_NAL_REPLAY_EXECUTE == 1;
constexpr std::int32_t kRejectedBuildOnly = -100;
constexpr std::size_t kPrimaryShadowSize = 0x908;
constexpr std::size_t kCallbackListOffset = 0x180;
constexpr std::size_t kDispatchingOffset = 0x1A0;
constexpr std::size_t kDeferredOffset = 0x1A1;
constexpr std::size_t kAddVslotOffset = 0x50;
constexpr std::size_t kRemoveVslotOffset = 0x58;
#if A9TAS_NAL_ACTION_EXECUTE == 1
constexpr std::uintptr_t kCarPhysicsPrimaryVtableRva = 0x7EE8D18;
constexpr std::uintptr_t kGameActionDispatchRva = 0x367B414;
constexpr std::size_t kCommandQueueOffset = 0x1360;
constexpr std::size_t kDirectModeOffset = 0x1378;
constexpr std::uintptr_t kActionDispatchAdjustMetadataDelta = 0x230;
constexpr std::uintptr_t kActionDispatchVtableRva = 0x7EEFE68;
constexpr std::uintptr_t kActionDispatchVfunc158Rva = 0x36A9CAC;
constexpr std::size_t kMaxObservedCommandTokens = 4096;
#endif

using FrameCallback = std::int64_t (*)(void*, const std::uint64_t*);
using ListMutation = void (*)(void*, void* const*);

enum Status : std::int32_t {
    kPassive = 0,
    kRegistered = 1,
    kZeroCallCompleted = 2,
    kRemovalRequested = 3,
    kActionCompleted = 4,
    kActionSubmitted = 5,
    kInvalidControl = -201,
    kUnexpectedObject = -202,
    kUnexpectedVptr = -203,
    kUnexpectedContext = -204,
    kUnexpectedMutationMethod = -205,
    kOutsideDispatch = -206,
    kListShape = -207,
    kDuplicateBootstrap = -208,
    kMailboxFailure = -209,
    kNonzeroRejected = -210,
    kRemovalUnsafe = -211,
    kActionSubmissionFailure = -212,
};

struct alignas(64) Control {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uintptr_t expected_car;
    std::uintptr_t original_car_vptr;
    std::uintptr_t shadow_car_vptr;
    std::uintptr_t original_car_callback;
    std::uintptr_t physics_context;
    std::uintptr_t expected_context_vptr;
    std::uintptr_t expected_add_method;
    std::uintptr_t expected_remove_method;
    std::uintptr_t dedicated_object;
    std::uintptr_t dedicated_vptr;
    std::uint32_t session_id;
    std::uint32_t expected_producer_tid;
    std::uint64_t vehicle_owner;
    std::uint32_t remove_requested;
    std::uint32_t reserved0;
    std::uint64_t reserved[1];
};

struct alignas(64) Evidence {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint64_t bootstrap_entries;
    std::uint64_t original_calls;
    std::uint64_t original_returns;
    std::uint64_t registration_attempts;
    std::uint64_t registration_returns;
    std::uint64_t callback_entries;
    std::uint64_t idle_entries;
    std::uint64_t claimed_commands;
    std::uint64_t zero_call_completions;
    std::uint64_t rejected_nonzero_commands;
    std::uint64_t removal_attempts;
    std::uint64_t removal_returns;
    std::uint64_t failures;
    std::uintptr_t last_object;
    std::uintptr_t last_token;
    std::uint64_t last_sequence;
    std::uint32_t last_frame;
    std::uint32_t last_tid;
    std::int32_t last_status;
    std::uint32_t protocol_state;
    std::uintptr_t list_end_before_add;
    std::uintptr_t list_active_before_add;
    std::uintptr_t list_end_after_add;
    std::uintptr_t list_active_after_add;
    std::uintptr_t list_end_after_remove;
    std::uintptr_t list_active_after_remove;
    std::uint8_t dispatch_before_add;
    std::uint8_t deferred_after_add;
    std::uint8_t dispatch_before_remove;
    std::uint8_t deferred_after_remove;
    std::uint8_t reserved_bytes[4];
#if A9TAS_NAL_ACTION_EXECUTE == 1
    std::uint64_t action_command_completions;
    std::uint64_t action_calls_submitted;
    std::uintptr_t action_queue_end_before;
    std::uintptr_t action_queue_end_after;
    std::uint64_t reserved[1];
#else
    std::uint64_t reserved[5];
#endif
};

struct CallbackListPrefix {
    std::uintptr_t begin;
    std::uintptr_t end;
    std::uintptr_t capacity_end;
    std::uintptr_t active_end;
};

struct DedicatedObject {
    std::uintptr_t vptr;
};

static_assert(sizeof(Control) == 128, "persistent callback control ABI");
static_assert(sizeof(Evidence) == 256, "persistent callback evidence ABI");
static_assert(sizeof(CallbackListPrefix) == 32, "callback list prefix ABI");
static_assert(sizeof(DedicatedObject) == sizeof(void*),
              "dedicated callback object ABI");
#if A9TAS_NAL_ACTION_EXECUTE == 1
static_assert(kActionExecutionCompiled,
              "action review build must compile scheduler submission");
#else
static_assert(!kActionExecutionCompiled,
              "default lifecycle build must not execute actions");
#endif
#if A9TAS_NAL_REPLAY_EXECUTE == 1
static_assert(kReplayExecutionCompiled && kActionExecutionCompiled,
              "replay payload must execute per-frame natural actions");
#else
static_assert(!kReplayExecutionCompiled,
              "single-Gate/passive payload must not compile replay mode");
#endif

__attribute__((noinline, visibility("hidden"))) std::int64_t
UnexpectedSlot(void*, const std::uint64_t*) {
    return 0;
}

alignas(64) std::uintptr_t g_dedicated_vtable[5] = {
    0,
    0,
    reinterpret_cast<std::uintptr_t>(&UnexpectedSlot),
    reinterpret_cast<std::uintptr_t>(&UnexpectedSlot),
    reinterpret_cast<std::uintptr_t>(
        &a9tas_natural_action_persistent_consumer_v1),
};
alignas(64) DedicatedObject g_dedicated_object = {
    reinterpret_cast<std::uintptr_t>(&g_dedicated_vtable[2]),
};
alignas(64) Control g_control = {
    .magic = {'A', '9', 'N', 'A', 'L', '1', 0, 0},
    .version = 1,
    .size = sizeof(Control),
    .expected_car = 0,
    .original_car_vptr = 0,
    .shadow_car_vptr = 0,
    .original_car_callback = 0,
    .physics_context = 0,
    .expected_context_vptr = 0,
    .expected_add_method = 0,
    .expected_remove_method = 0,
    .dedicated_object = reinterpret_cast<std::uintptr_t>(&g_dedicated_object),
    .dedicated_vptr = reinterpret_cast<std::uintptr_t>(&g_dedicated_vtable[2]),
    .session_id = 0,
    .expected_producer_tid = 0,
    .vehicle_owner = 0,
    .remove_requested = 0,
    .reserved0 = 0,
    .reserved = {0},
};
alignas(64) Evidence g_evidence = {
    .magic = {'A', '9', 'N', 'A', 'X', '1', 0, 0},
    .version = 1,
    .size = sizeof(Evidence),
    .bootstrap_entries = 0,
    .original_calls = 0,
    .original_returns = 0,
    .registration_attempts = 0,
    .registration_returns = 0,
    .callback_entries = 0,
    .idle_entries = 0,
    .claimed_commands = 0,
    .zero_call_completions = 0,
    .rejected_nonzero_commands = 0,
    .removal_attempts = 0,
    .removal_returns = 0,
    .failures = 0,
    .last_object = 0,
    .last_token = 0,
    .last_sequence = 0,
    .last_frame = 0,
    .last_tid = 0,
    .last_status = 0,
    .protocol_state = 0,
    .list_end_before_add = 0,
    .list_active_before_add = 0,
    .list_end_after_add = 0,
    .list_active_after_add = 0,
    .list_end_after_remove = 0,
    .list_active_after_remove = 0,
    .dispatch_before_add = 0,
    .deferred_after_add = 0,
    .dispatch_before_remove = 0,
    .deferred_after_remove = 0,
    .reserved_bytes = {0, 0, 0, 0},
#if A9TAS_NAL_ACTION_EXECUTE == 1
    .action_command_completions = 0,
    .action_calls_submitted = 0,
    .action_queue_end_before = 0,
    .action_queue_end_after = 0,
    .reserved = {0},
#else
    .reserved = {0, 0, 0, 0, 0},
#endif
};
alignas(64) mailbox::Mailbox g_mailbox{};
alignas(64) mailbox::RuntimeState g_runtime{};
alignas(64) std::uint8_t g_primary_shadow[kPrimaryShadowSize] = {0xA9};
std::atomic<std::uint32_t> g_bootstrap_state{0};
std::atomic<std::uint32_t> g_registration_state{0};
std::atomic<std::uint32_t> g_removal_state{0};
#if A9TAS_NAL_ACTION_EXECUTE == 1
#if A9TAS_NAL_REPLAY_EXECUTE == 0
std::atomic<std::uint64_t> g_pending_action_sequence{0};
std::uint32_t g_pending_action_calls = 0;
std::uint32_t g_pending_initial_nitro_mode = 0;
std::uint8_t g_pending_initial_nitro_active = 0;
constexpr std::uint64_t kNitroTransitionProof = 0xA9E1000000000000ULL;
#else
constexpr std::uint64_t kActionSubmissionProof = 0xA9E2000000000000ULL;
#endif
#endif

template <typename T>
T Load(const T* value) {
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

template <typename T>
void Store(T* target, T value) {
    __atomic_store_n(target, value, __ATOMIC_RELEASE);
}

void Add(std::uint64_t* value) {
    __atomic_add_fetch(value, 1u, __ATOMIC_RELAXED);
}

void Fail(Status status) {
    Add(&g_evidence.failures);
    Store(&g_evidence.last_status, static_cast<std::int32_t>(status));
}

bool ControlValid(const Control& control) {
    const bool common = control.magic[0] == 'A' && control.magic[1] == '9' &&
           control.magic[2] == 'N' && control.magic[3] == 'A' &&
           control.magic[4] == 'L' && control.magic[5] == '1' &&
           control.magic[6] == 0 && control.magic[7] == 0 &&
           control.version == 1 && control.size == sizeof(Control) &&
           control.expected_car != 0 && control.original_car_vptr != 0 &&
           control.shadow_car_vptr != 0 &&
           control.original_car_callback != 0 &&
           control.physics_context != 0 &&
           control.expected_context_vptr != 0 &&
           control.expected_add_method != 0 &&
           control.expected_remove_method != 0 &&
           control.dedicated_object ==
               reinterpret_cast<std::uintptr_t>(&g_dedicated_object) &&
           control.dedicated_vptr ==
               reinterpret_cast<std::uintptr_t>(&g_dedicated_vtable[2]) &&
           control.session_id != 0 && control.expected_producer_tid != 0 &&
           control.vehicle_owner != 0 && control.remove_requested <= 1 &&
           control.reserved0 == 0;
#if A9TAS_NAL_ACTION_EXECUTE == 1
    // The action scheduler interface is a subobject of the live car-physics
    // object.  Resolve it independently on the host, but require that the
    // recovered complete-object owner is the same callback object.  The
    // dispatch path below has always required this equality as well.
    return common && control.vehicle_owner == control.expected_car &&
           control.original_car_vptr >= kCarPhysicsPrimaryVtableRva &&
           control.reserved[0] != 0;
#else
    return common && control.reserved[0] == 0;
#endif
}

#if A9TAS_NAL_ACTION_EXECUTE == 1
struct CommandQueueSnapshot {
    std::uintptr_t begin;
    std::uintptr_t end;
    std::uintptr_t capacity_end;
    std::size_t count;
};

bool ReadCommandQueue(std::uintptr_t owner, CommandQueueSnapshot* output) {
    if (!output || owner == 0 || (owner & 7u) != 0) return false;
    const auto* queue = reinterpret_cast<const std::uintptr_t*>(
        owner + kCommandQueueOffset);
    const std::uintptr_t begin = Load(&queue[0]);
    const std::uintptr_t end = Load(&queue[1]);
    const std::uintptr_t capacity = Load(&queue[2]);
    if (begin > end || end > capacity ||
        ((end - begin) & (sizeof(std::uintptr_t) - 1u)) != 0 ||
        ((capacity - begin) & (sizeof(std::uintptr_t) - 1u)) != 0)
        return false;
    const std::size_t count =
        static_cast<std::size_t>((end - begin) / sizeof(std::uintptr_t));
    if (count > kMaxObservedCommandTokens) return false;
    *output = {begin, end, capacity, count};
    return true;
}

bool ActionOwnerValid(std::uintptr_t owner, std::uintptr_t base) {
    if (owner == 0 || (owner & 7u) != 0 || base == 0) return false;
    const std::uintptr_t command_interface = Load(
        reinterpret_cast<const std::uintptr_t*>(owner + 0x30));
    if (command_interface == 0 || (command_interface & 7u) != 0) return false;
    const std::uintptr_t interface_head = Load(
        reinterpret_cast<const std::uintptr_t*>(command_interface));
    if (interface_head < kActionDispatchAdjustMetadataDelta) return false;
    const std::int64_t adjustment = Load(
        reinterpret_cast<const std::int64_t*>(
            interface_head - kActionDispatchAdjustMetadataDelta));
    const auto signed_interface = static_cast<std::intptr_t>(command_interface);
    if ((adjustment > 0 && signed_interface > INTPTR_MAX - adjustment) ||
        (adjustment < 0 && signed_interface < INTPTR_MIN - adjustment))
        return false;
    const auto signed_dispatch = signed_interface + adjustment;
    if (signed_dispatch <= 0) return false;
    const auto dispatch_this = static_cast<std::uintptr_t>(signed_dispatch);
    const std::uintptr_t dispatch_vtable = Load(
        reinterpret_cast<const std::uintptr_t*>(dispatch_this));
    if (dispatch_vtable != base + kActionDispatchVtableRva) return false;
    const std::uintptr_t dispatch_vfunc = Load(
        reinterpret_cast<const std::uintptr_t*>(dispatch_vtable + 0x158));
    return dispatch_vfunc == base + kActionDispatchVfunc158Rva;
}

bool ReadNitroState(const Control& control, std::uint8_t* active,
                    std::uint32_t* mode) {
    if (!active || !mode || control.reserved[0] == 0) return false;
    *active = Load(reinterpret_cast<const std::uint8_t*>(
        control.reserved[0] + 0x188));
    *mode = Load(reinterpret_cast<const std::uint32_t*>(
        control.reserved[0] + 0x18C));
    return *active <= 1;
}

__attribute__((noinline, visibility("hidden"))) bool SubmitGameOwnedActions(
    const Control& control, std::uint32_t activations,
    std::uint32_t* calls_submitted) {
    if (!calls_submitted || activations == 0 ||
        activations > mailbox::kMaximumNitroActivations ||
        control.vehicle_owner != control.expected_car ||
        control.original_car_vptr < kCarPhysicsPrimaryVtableRva)
        return false;
    *calls_submitted = 0;
    const std::uintptr_t owner = control.vehicle_owner;
    const std::uintptr_t base =
        control.original_car_vptr - kCarPhysicsPrimaryVtableRva;
    const std::uintptr_t dispatch_address = base + kGameActionDispatchRva;
    if (!ActionOwnerValid(owner, base) ||
        Load(reinterpret_cast<const std::uint8_t*>(owner +
                                                   kDirectModeOffset)) != 0)
        return false;
    CommandQueueSnapshot before{};
    if (!ReadCommandQueue(owner, &before)) return false;
    g_evidence.action_queue_end_before = before.end;
    using DispatchAction = void (*)(void*);
    const auto dispatch = reinterpret_cast<DispatchAction>(dispatch_address);
    CommandQueueSnapshot previous = before;
    for (std::uint32_t index = 0; index < activations; ++index) {
        if (Load(reinterpret_cast<const std::uint8_t*>(
                     owner + kDirectModeOffset)) != 0)
            return false;
        dispatch(reinterpret_cast<void*>(owner));
        CommandQueueSnapshot after{};
        if (!ReadCommandQueue(owner, &after) ||
            after.count != previous.count + 1u)
            return false;
        previous = after;
        *calls_submitted = index + 1u;
    }
    g_evidence.action_queue_end_after = previous.end;
    return previous.count == before.count + activations;
}
#endif

CallbackListPrefix ReadList(std::uintptr_t context) {
    const auto* list = reinterpret_cast<const CallbackListPrefix*>(
        context + kCallbackListOffset);
    return {Load(&list->begin), Load(&list->end), Load(&list->capacity_end),
            Load(&list->active_end)};
}

bool ListValid(const CallbackListPrefix& list) {
    return list.begin != 0 && list.begin <= list.active_end &&
           list.active_end <= list.end && list.end <= list.capacity_end &&
           ((list.end - list.begin) & 15u) == 0 &&
           ((list.active_end - list.begin) & 15u) == 0 &&
           list.end - list.begin <= 4096u * 16u;
}

bool Methods(const Control& control, ListMutation* add, ListMutation* remove) {
    const auto* context_vptr = reinterpret_cast<const std::uintptr_t*>(
        control.physics_context);
    const std::uintptr_t vptr = Load(context_vptr);
    if (vptr != control.expected_context_vptr) return false;
    const auto* table = reinterpret_cast<const std::uintptr_t*>(vptr);
    const std::uintptr_t add_address =
        Load(&table[kAddVslotOffset / sizeof(std::uintptr_t)]);
    const std::uintptr_t remove_address =
        Load(&table[kRemoveVslotOffset / sizeof(std::uintptr_t)]);
    if (add_address != control.expected_add_method ||
        remove_address != control.expected_remove_method)
        return false;
    *add = reinterpret_cast<ListMutation>(add_address);
    *remove = reinterpret_cast<ListMutation>(remove_address);
    return true;
}

bool DuringDispatch(const Control& control) {
    return Load(reinterpret_cast<const std::uint8_t*>(
                    control.physics_context + kDispatchingOffset)) == 1;
}

bool RequestRemoval(const Control& control, ListMutation remove,
                    bool failure_cleanup) {
    if (!DuringDispatch(control)) {
        Fail(kOutsideDispatch);
        return false;
    }
    if (!failure_cleanup &&
        (mailbox::Armed(mailbox::LoadAcquire(&g_mailbox.session_control)) ||
         mailbox::LoadAcquire(&g_mailbox.claimed_sequence) !=
             mailbox::LoadAcquire(&g_mailbox.completed_sequence))) {
        Fail(kRemovalUnsafe);
        return false;
    }
    std::uint32_t expected = 0;
    if (!g_removal_state.compare_exchange_strong(
            expected, 1, std::memory_order_acq_rel,
            std::memory_order_acquire))
        return false;
    const CallbackListPrefix before = ReadList(control.physics_context);
    if (!ListValid(before)) {
        Fail(kListShape);
        g_removal_state.store(3, std::memory_order_release);
        return false;
    }
    void* object = &g_dedicated_object;
    Add(&g_evidence.removal_attempts);
    remove(reinterpret_cast<void*>(control.physics_context), &object);
    Add(&g_evidence.removal_returns);
    const CallbackListPrefix after = ReadList(control.physics_context);
    g_evidence.list_end_after_remove = after.end;
    g_evidence.list_active_after_remove = after.active_end;
    g_evidence.dispatch_before_remove = 1;
    g_evidence.deferred_after_remove = Load(
        reinterpret_cast<const std::uint8_t*>(
            control.physics_context + kDeferredOffset));
    if (!ListValid(after) || g_evidence.deferred_after_remove != 1) {
        Fail(kListShape);
        g_removal_state.store(3, std::memory_order_release);
        return false;
    }
    g_removal_state.store(2, std::memory_order_release);
    Store(&g_evidence.protocol_state, 3u);
    Store(&g_evidence.last_status,
          static_cast<std::int32_t>(kRemovalRequested));
    return true;
}

__attribute__((constructor)) void InitializePayload() {
    mailbox::Initialize(&g_mailbox);
}

}  // namespace

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_natural_action_registration_bootstrap_v1(
    void* car, const std::uint64_t* frame_token) {
    const Control control = g_control;
    if (!car) {
        Fail(kUnexpectedObject);
        return 0;
    }
    auto* car_vptr = reinterpret_cast<std::uintptr_t*>(car);
    const std::uintptr_t observed = Load(car_vptr);
    if (control.original_car_vptr != 0 && control.shadow_car_vptr != 0 &&
        observed == control.shadow_car_vptr)
        Store(car_vptr, control.original_car_vptr);
    if (!ControlValid(control) ||
        reinterpret_cast<std::uintptr_t>(car) != control.expected_car) {
        Fail(kInvalidControl);
        return 0;
    }
    if (observed != control.shadow_car_vptr) {
        Fail(kUnexpectedVptr);
        return 0;
    }
    std::uint32_t expected_bootstrap = 0;
    if (!g_bootstrap_state.compare_exchange_strong(
            expected_bootstrap, 1, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        Fail(kDuplicateBootstrap);
        return 0;
    }
    Add(&g_evidence.bootstrap_entries);
    const auto original =
        reinterpret_cast<FrameCallback>(control.original_car_callback);
    Add(&g_evidence.original_calls);
    const std::int64_t result = original(car, frame_token);
    Add(&g_evidence.original_returns);

    ListMutation add = nullptr;
    ListMutation remove = nullptr;
    if (!Methods(control, &add, &remove) || !DuringDispatch(control)) {
        Fail(kUnexpectedMutationMethod);
        return result;
    }
    const CallbackListPrefix before = ReadList(control.physics_context);
    if (!ListValid(before) ||
        Load(reinterpret_cast<const std::uint8_t*>(
                 control.physics_context + kDeferredOffset)) != 0) {
        Fail(kListShape);
        return result;
    }
    g_evidence.list_end_before_add = before.end;
    g_evidence.list_active_before_add = before.active_end;
    g_evidence.dispatch_before_add = 1;
    void* object = &g_dedicated_object;
    Add(&g_evidence.registration_attempts);
    add(reinterpret_cast<void*>(control.physics_context), &object);
    Add(&g_evidence.registration_returns);
    const CallbackListPrefix after = ReadList(control.physics_context);
    g_evidence.list_end_after_add = after.end;
    g_evidence.list_active_after_add = after.active_end;
    g_evidence.deferred_after_add = Load(
        reinterpret_cast<const std::uint8_t*>(
            control.physics_context + kDeferredOffset));
    const bool added = ListValid(after) &&
                       after.end == before.end + 16u &&
                       after.active_end == before.active_end &&
                       g_evidence.deferred_after_add == 1;
    if (!added) {
        Fail(kListShape);
        if (ListValid(after) && after.end >= before.end + 16u)
            (void)RequestRemoval(control, remove, true);
        return result;
    }
    g_registration_state.store(2, std::memory_order_release);
    Store(&g_evidence.protocol_state, 1u);
    Store(&g_evidence.last_status, static_cast<std::int32_t>(kRegistered));
    return result;
}

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_natural_action_persistent_consumer_v1(
    void* object, const std::uint64_t* frame_token) {
    const Control control = g_control;
    Add(&g_evidence.callback_entries);
    g_evidence.last_object = reinterpret_cast<std::uintptr_t>(object);
    g_evidence.last_token = reinterpret_cast<std::uintptr_t>(frame_token);
    if (!ControlValid(control) || object != &g_dedicated_object ||
        Load(reinterpret_cast<const std::uintptr_t*>(object)) !=
            control.dedicated_vptr ||
        g_registration_state.load(std::memory_order_acquire) != 2) {
        Fail(kInvalidControl);
        return 0;
    }
    ListMutation add = nullptr;
    ListMutation remove = nullptr;
    if (!Methods(control, &add, &remove) || !DuringDispatch(control)) {
        Fail(kUnexpectedMutationMethod);
        return 0;
    }
    if (Load(&g_control.remove_requested) == 1) {
        (void)RequestRemoval(control, remove, false);
        return 0;
    }

    const std::uint32_t tid =
        static_cast<std::uint32_t>(syscall(__NR_gettid));
#if A9TAS_NAL_ACTION_EXECUTE == 1 && A9TAS_NAL_REPLAY_EXECUTE == 0
    const std::uint64_t pending =
        g_pending_action_sequence.load(std::memory_order_acquire);
    if (pending != 0) {
        std::uint8_t active = 0;
        std::uint32_t mode = 0;
        if (!ReadNitroState(control, &active, &mode)) {
            Fail(kActionSubmissionFailure);
            return 0;
        }
        // The single-activation Gate is only armed from an idle Nitro state.
        // Do not acknowledge mere scheduler insertion: require the real game
        // state machine to transition inactive -> active on a later callback.
        if (g_pending_initial_nitro_active == 0 && active == 1) {
            if (mailbox::CompleteNaturalCallback(
                    &g_mailbox, &g_runtime, pending,
                    g_pending_action_calls, true) !=
                mailbox::Result::kCompleted) {
                Fail(kMailboxFailure);
                return 0;
            }
            Add(&g_evidence.action_command_completions);
            g_evidence.reserved[0] =
                kNitroTransitionProof | static_cast<std::uint64_t>(mode);
            Store(&g_evidence.protocol_state, 2u);
            Store(&g_evidence.last_status,
                  static_cast<std::int32_t>(kActionCompleted));
            g_pending_action_sequence.store(0, std::memory_order_release);
        } else if (mode != g_pending_initial_nitro_mode) {
            // A mode-only change without activation is evidence of a different
            // state-machine path, not success for this idle one-click Gate.
            Fail(kActionSubmissionFailure);
        }
        return 0;
    }
#endif
    mailbox::Command command{};
    const mailbox::Result claim = mailbox::ClaimAtNaturalCallback(
        &g_mailbox, &g_runtime, tid, &command);
    if (claim == mailbox::Result::kIdle ||
        claim == mailbox::Result::kPassive) {
        Add(&g_evidence.idle_entries);
        return 0;
    }
    if (claim != mailbox::Result::kClaimed) {
        Fail(kMailboxFailure);
        mailbox::StoreRelease(
            &g_mailbox.session_control,
            static_cast<std::uint64_t>(control.session_id) << 1);
        (void)RequestRemoval(control, remove, true);
        return 0;
    }
    Add(&g_evidence.claimed_commands);
    g_evidence.last_sequence = command.sequence;
    g_evidence.last_frame = command.replay_frame;
    g_evidence.last_tid = tid;
    if (command.nitro_activations != 0) {
#if A9TAS_NAL_ACTION_EXECUTE == 1
        std::uint32_t calls_submitted = 0;
        std::uint8_t initial_active = 0;
        std::uint32_t initial_mode = 0;
        if (!ReadNitroState(control, &initial_active, &initial_mode)
#if A9TAS_NAL_REPLAY_EXECUTE == 0
            || initial_active != 0
#endif
        ) {
            (void)mailbox::CompleteNaturalCallback(
                &g_mailbox, &g_runtime, command.sequence, 0, false);
            Fail(kActionSubmissionFailure);
            return 0;
        }
        if (!SubmitGameOwnedActions(control, command.nitro_activations,
                                    &calls_submitted)) {
            (void)mailbox::CompleteNaturalCallback(
                &g_mailbox, &g_runtime, command.sequence, calls_submitted,
                false);
            Fail(kActionSubmissionFailure);
            mailbox::StoreRelease(
                &g_mailbox.session_control,
                static_cast<std::uint64_t>(control.session_id) << 1);
            (void)RequestRemoval(control, remove, true);
            return 0;
        }
        __atomic_add_fetch(&g_evidence.action_calls_submitted,
                           calls_submitted, __ATOMIC_RELAXED);
#if A9TAS_NAL_REPLAY_EXECUTE == 1
        // AluTasV2's authoritative per-frame value is the number of real
        // action calls, not the eventual Nitro colour. SubmitGameOwnedActions
        // already proves one exact game-queue growth per call. Complete this
        // frame immediately so a later game state transition cannot delay the
        // next authoritative replay frame.
        std::uint8_t immediate_active = 0;
        std::uint32_t immediate_mode = 0;
        std::uint32_t transition_proof = 0;
        if (ReadNitroState(control, &immediate_active, &immediate_mode)) {
            const replay::NitroSnapshot before{
                initial_active, {0, 0, 0}, initial_mode};
            const replay::NitroSnapshot after{
                immediate_active, {0, 0, 0}, immediate_mode};
            transition_proof = replay::ProveTransition(
                before, after, calls_submitted);
        }
        if (mailbox::CompleteNaturalCallback(
                &g_mailbox, &g_runtime, command.sequence,
                calls_submitted, true) != mailbox::Result::kCompleted) {
            Fail(kMailboxFailure);
            return 0;
        }
        Add(&g_evidence.action_command_completions);
        g_evidence.reserved[0] = kActionSubmissionProof |
            (static_cast<std::uint64_t>(transition_proof) << 32) |
            static_cast<std::uint64_t>(immediate_mode);
        Store(&g_evidence.protocol_state, 2u);
        Store(&g_evidence.last_status,
              static_cast<std::int32_t>(kActionCompleted));
        return 0;
#else
        g_pending_action_calls = calls_submitted;
        g_pending_initial_nitro_mode = initial_mode;
        g_pending_initial_nitro_active = initial_active;
        g_pending_action_sequence.store(command.sequence,
                                        std::memory_order_release);
        Store(&g_evidence.last_status,
              static_cast<std::int32_t>(kActionSubmitted));
        return 0;
#endif
#else
        Add(&g_evidence.rejected_nonzero_commands);
        Fail(kNonzeroRejected);
        (void)mailbox::CompleteNaturalCallback(
            &g_mailbox, &g_runtime, command.sequence, 0, false);
        mailbox::StoreRelease(
            &g_mailbox.session_control,
            static_cast<std::uint64_t>(control.session_id) << 1);
        (void)RequestRemoval(control, remove, true);
        return 0;
#endif
    }
    const mailbox::Result completed = mailbox::CompleteNaturalCallback(
        &g_mailbox, &g_runtime, command.sequence, 0, true);
    if (completed != mailbox::Result::kCompleted) {
        Fail(kMailboxFailure);
        mailbox::StoreRelease(
            &g_mailbox.session_control,
            static_cast<std::uint64_t>(control.session_id) << 1);
        (void)RequestRemoval(control, remove, true);
        return 0;
    }
    Add(&g_evidence.zero_call_completions);
    Store(&g_evidence.protocol_state, 2u);
    Store(&g_evidence.last_status,
          static_cast<std::int32_t>(kZeroCallCompleted));
    return 0;
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_natural_action_lifecycle_arm_v1() {
    return kRejectedBuildOnly;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_natural_action_lifecycle_shadow_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_primary_shadow);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_natural_action_lifecycle_control_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_control);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_natural_action_lifecycle_evidence_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_evidence);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_natural_action_lifecycle_mailbox_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_mailbox);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_natural_action_lifecycle_object_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_dedicated_object);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_natural_action_lifecycle_vtable_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_dedicated_vtable[2]);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_natural_action_lifecycle_control_size_v1 = sizeof(g_control);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_natural_action_lifecycle_evidence_size_v1 = sizeof(g_evidence);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_natural_action_lifecycle_mailbox_size_v1 = sizeof(g_mailbox);
