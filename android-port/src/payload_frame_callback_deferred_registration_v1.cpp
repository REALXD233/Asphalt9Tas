// FC-2 build-only proof for natural deferred registration/removal.
//
// This payload extends the FC-1 one-frame pass-through contract without adding
// a deployment path.  Its arm export is permanently rejected.  A separately
// reviewed host gate may populate the payload-owned control/shadow storage and
// perform the same single CarPhysicsState vptr transaction proven by FC-1.
//
// On that one natural wrapper entry:
//   1. restore the exact original CarPhysicsState vptr;
//   2. call the exact original callback once and preserve its result;
//   3. while PhysicsContext is still dispatching, call its game-owned vslot
//      +0x50 add-unique method with a payload-owned dedicated callback object.
// The dedicated callback is observe-only.  On its first later natural frame it
// records the token and calls the game-owned vslot +0x58 remove method while
// dispatching, so the game's own deferred compaction ends payload lifetime.

#include <atomic>
#include <cstddef>
#include <cstdint>

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_fc2_dedicated_observer_v1(void*, const std::uint64_t*);

namespace {

constexpr std::uintptr_t kPrimaryPrefixRva = 0x7EE8CC0;
constexpr std::uintptr_t kPrimaryAddressPointRva = 0x7EE8D18;
constexpr std::uintptr_t kNextAddressPointRva = 0x7EE95C8;
[[maybe_unused]] constexpr std::uintptr_t kOriginalCallbackRva = 0x367D66C;
[[maybe_unused]] constexpr std::uintptr_t kPhysicsContextVtableRva = 0x8103830;
[[maybe_unused]] constexpr std::uintptr_t kPhysicsContextAddRva = 0x38B77CC;
[[maybe_unused]] constexpr std::uintptr_t kPhysicsContextRemoveRva = 0x38B7840;
constexpr std::size_t kPrimaryPrefixSize = 0x58;
constexpr std::size_t kPrimaryShadowSize = 0x908;
constexpr std::size_t kCallbackSlotOffset = 0x10;
constexpr std::size_t kCallbackListOffset = 0x180;
constexpr std::size_t kDispatchingOffset = 0x1A0;
constexpr std::size_t kDeferredOffset = 0x1A1;
constexpr std::size_t kAddVslotOffset = 0x50;
constexpr std::size_t kRemoveVslotOffset = 0x58;

static_assert(kPrimaryAddressPointRva - kPrimaryPrefixRva ==
                  kPrimaryPrefixSize,
              "FC-2 primary prefix boundary");
static_assert(kNextAddressPointRva - kPrimaryPrefixRva ==
                  kPrimaryShadowSize,
              "FC-2 primary vtable boundary");
static_assert(kPrimaryPrefixSize + kCallbackSlotOffset + sizeof(void*) <=
                  kPrimaryShadowSize,
              "FC-2 bootstrap slot lies inside primary shadow");
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free,
              "FC-2 requires lock-free pointer atomics");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "FC-2 requires lock-free state atomics");

using FrameCallback = std::int64_t (*)(void*, const std::uint64_t*);
using ListMutation = void (*)(void*, void* const*);

enum Status : std::int32_t {
  kStatusPassive = 0,
  kStatusRegistered = 1,
  kStatusRemovalRequested = 2,
  kStatusRejectedBuildOnly = -100,
  kStatusUnexpectedCar = -201,
  kStatusInvalidControl = -202,
  kStatusUnexpectedCarVptr = -203,
  kStatusUnexpectedContext = -204,
  kStatusUnexpectedContextVptr = -205,
  kStatusUnexpectedMutationSlot = -206,
  kStatusRegistrationOutsideDispatch = -207,
  kStatusRegistrationShape = -208,
  kStatusUnexpectedDedicatedObject = -209,
  kStatusUnexpectedDedicatedVptr = -210,
  kStatusRemovalOutsideDispatch = -211,
  kStatusRemovalDidNotDefer = -212,
  kStatusDuplicateBootstrap = -213,
  kStatusDuplicateDedicatedEntry = -214,
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
  std::uint64_t reserved[4];
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
  std::uint64_t dedicated_entries;
  std::uint64_t removal_attempts;
  std::uint64_t removal_returns;
  std::uint64_t recursive_entries;
  std::uint64_t failures;
  std::uintptr_t last_car;
  std::uintptr_t last_bootstrap_token;
  std::uintptr_t observed_car_vptr;
  std::uintptr_t restored_car_vptr;
  std::uintptr_t observed_context_vptr;
  std::uintptr_t observed_add_method;
  std::uintptr_t observed_remove_method;
  std::uintptr_t last_dedicated_object;
  std::uintptr_t last_dedicated_token;
  std::uint64_t last_original_result;
  std::uintptr_t list_begin_before;
  std::uintptr_t list_end_before;
  std::uintptr_t list_active_before;
  std::uintptr_t list_begin_after_add;
  std::uintptr_t list_end_after_add;
  std::uintptr_t list_active_after_add;
  std::uintptr_t list_begin_after_remove;
  std::uintptr_t list_end_after_remove;
  std::uint8_t registration_dispatching_before;
  std::uint8_t registration_deferred_before;
  std::uint8_t registration_dispatching_after;
  std::uint8_t registration_deferred_after;
  std::uint8_t removal_dispatching_before;
  std::uint8_t removal_deferred_before;
  std::uint8_t removal_dispatching_after;
  std::uint8_t removal_deferred_after;
  std::int32_t last_status;
  std::uint32_t protocol_state;
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

static_assert(sizeof(Control) == 128, "FC-2 control ABI");
static_assert(sizeof(Evidence) == 256, "FC-2 evidence ABI");
static_assert(sizeof(CallbackListPrefix) == 32, "FC-2 list prefix ABI");
static_assert(sizeof(DedicatedObject) == sizeof(void*),
              "FC-2 dedicated object ABI");

__attribute__((noinline, visibility("hidden"))) std::int64_t
DedicatedUnexpectedSlot(void*, const std::uint64_t*) {
  return 0;
}

// Itanium address point is &g_dedicated_vtable[2].  The game-owned callback
// list invokes only address-point slot +0x10.  Slots +0/+8 are non-owning
// fail-safe stubs: the game never owns or destroys this static payload object.
alignas(64) std::uintptr_t g_dedicated_vtable[5] = {
    0,
    0,
    reinterpret_cast<std::uintptr_t>(&DedicatedUnexpectedSlot),
    reinterpret_cast<std::uintptr_t>(&DedicatedUnexpectedSlot),
    reinterpret_cast<std::uintptr_t>(&a9tas_fc2_dedicated_observer_v1),
};

alignas(64) DedicatedObject g_dedicated_object = {
    reinterpret_cast<std::uintptr_t>(&g_dedicated_vtable[2]),
};

alignas(64) Control g_control = {
    {'A', '9', 'F', 'C', '2', 'C', '1', 0},
    1,
    sizeof(Control),
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    {0, 0, 0, 0},
};

alignas(64) Evidence g_evidence = {
    .magic = {'A', '9', 'F', 'C', '2', 'E', '1', 0},
    .version = 1,
    .size = sizeof(Evidence),
    .bootstrap_entries = 0,
    .original_calls = 0,
    .original_returns = 0,
    .registration_attempts = 0,
    .registration_returns = 0,
    .dedicated_entries = 0,
    .removal_attempts = 0,
    .removal_returns = 0,
    .recursive_entries = 0,
    .failures = 0,
    .last_car = 0,
    .last_bootstrap_token = 0,
    .observed_car_vptr = 0,
    .restored_car_vptr = 0,
    .observed_context_vptr = 0,
    .observed_add_method = 0,
    .observed_remove_method = 0,
    .last_dedicated_object = 0,
    .last_dedicated_token = 0,
    .last_original_result = 0,
    .list_begin_before = 0,
    .list_end_before = 0,
    .list_active_before = 0,
    .list_begin_after_add = 0,
    .list_end_after_add = 0,
    .list_active_after_add = 0,
    .list_begin_after_remove = 0,
    .list_end_after_remove = 0,
    .registration_dispatching_before = 0,
    .registration_deferred_before = 0,
    .registration_dispatching_after = 0,
    .registration_deferred_after = 0,
    .removal_dispatching_before = 0,
    .removal_deferred_before = 0,
    .removal_dispatching_after = 0,
    .removal_deferred_after = 0,
    .last_status = kStatusPassive,
    .protocol_state = 0,
};

// As in FC-0/FC-1, a nonzero sentinel keeps the complete shadow file-backed.
alignas(64) std::uint8_t g_primary_shadow[kPrimaryShadowSize] = {0xA9};
std::atomic<std::uint32_t> g_bootstrap_depth{0};
std::atomic<std::uint32_t> g_registration_state{0};
std::atomic<std::uint32_t> g_removal_state{0};

template <typename T>
T Load(const T* address) {
  return __atomic_load_n(address, __ATOMIC_ACQUIRE);
}

template <typename T>
void Store(T* address, T value) {
  __atomic_store_n(address, value, __ATOMIC_RELEASE);
}

void AddCounter(std::uint64_t* address) {
  __atomic_add_fetch(address, 1u, __ATOMIC_RELAXED);
}

void RecordFailure(Status status) {
  AddCounter(&g_evidence.failures);
  Store(&g_evidence.last_status, static_cast<std::int32_t>(status));
}

bool CompleteControl(const Control& control) {
  return control.magic[0] == 'A' && control.magic[1] == '9' &&
         control.magic[2] == 'F' && control.magic[3] == 'C' &&
         control.magic[4] == '2' && control.magic[5] == 'C' &&
         control.magic[6] == '1' && control.magic[7] == 0 &&
         control.version == 1 && control.size == sizeof(Control) &&
         control.expected_car != 0 && control.original_car_vptr != 0 &&
         control.shadow_car_vptr != 0 &&
         control.original_car_callback != 0 && control.physics_context != 0 &&
         control.expected_context_vptr != 0 &&
         control.expected_add_method != 0 &&
         control.expected_remove_method != 0 &&
         control.dedicated_object ==
             reinterpret_cast<std::uintptr_t>(&g_dedicated_object) &&
         control.dedicated_vptr ==
             reinterpret_cast<std::uintptr_t>(&g_dedicated_vtable[2]);
}

bool ReadMutationMethods(const Control& control, ListMutation* add,
                         ListMutation* remove) {
  const auto* const context_vptr_address =
      reinterpret_cast<const std::uintptr_t*>(control.physics_context);
  const std::uintptr_t context_vptr = Load(context_vptr_address);
  g_evidence.observed_context_vptr = context_vptr;
  if (context_vptr != control.expected_context_vptr) {
    RecordFailure(kStatusUnexpectedContextVptr);
    return false;
  }
  const auto* const table =
      reinterpret_cast<const std::uintptr_t*>(context_vptr);
  const std::uintptr_t add_method =
      Load(&table[kAddVslotOffset / sizeof(std::uintptr_t)]);
  const std::uintptr_t remove_method =
      Load(&table[kRemoveVslotOffset / sizeof(std::uintptr_t)]);
  g_evidence.observed_add_method = add_method;
  g_evidence.observed_remove_method = remove_method;
  if (add_method != control.expected_add_method ||
      remove_method != control.expected_remove_method) {
    RecordFailure(kStatusUnexpectedMutationSlot);
    return false;
  }
  *add = reinterpret_cast<ListMutation>(add_method);
  *remove = reinterpret_cast<ListMutation>(remove_method);
  return true;
}

CallbackListPrefix ReadListPrefix(std::uintptr_t context) {
  const auto* const list = reinterpret_cast<const CallbackListPrefix*>(
      context + kCallbackListOffset);
  CallbackListPrefix snapshot{};
  snapshot.begin = Load(&list->begin);
  snapshot.end = Load(&list->end);
  snapshot.capacity_end = Load(&list->capacity_end);
  snapshot.active_end = Load(&list->active_end);
  return snapshot;
}

bool ValidListShape(const CallbackListPrefix& list) {
  return list.begin != 0 && list.begin <= list.active_end &&
         list.active_end <= list.end && list.end <= list.capacity_end &&
         ((list.end - list.begin) & 15u) == 0 &&
         ((list.active_end - list.begin) & 15u) == 0 &&
         list.end - list.begin <= 4096u * 16u;
}

CallbackListPrefix RequestDeferredRemoval(const Control& control,
                                          ListMutation remove) {
  void* dedicated = &g_dedicated_object;
  AddCounter(&g_evidence.removal_attempts);
  remove(reinterpret_cast<void*>(control.physics_context), &dedicated);
  AddCounter(&g_evidence.removal_returns);
  const CallbackListPrefix after = ReadListPrefix(control.physics_context);
  const auto* const dispatching = reinterpret_cast<const std::uint8_t*>(
      control.physics_context + kDispatchingOffset);
  const auto* const deferred = reinterpret_cast<const std::uint8_t*>(
      control.physics_context + kDeferredOffset);
  g_evidence.list_begin_after_remove = after.begin;
  g_evidence.list_end_after_remove = after.end;
  g_evidence.removal_dispatching_after = Load(dispatching);
  g_evidence.removal_deferred_after = Load(deferred);
  return after;
}

}  // namespace

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_fc2_frame_callback_bootstrap_v1(void* car,
                                      const std::uint64_t* frame_token) {
  const Control control = g_control;
  if (car == nullptr) {
    RecordFailure(kStatusUnexpectedCar);
    return 0;
  }

  auto* const car_vptr_address = reinterpret_cast<std::uintptr_t*>(car);
  const std::uintptr_t observed_vptr = Load(car_vptr_address);
  const bool control_has_restore =
      control.original_car_vptr != 0 && control.shadow_car_vptr != 0;
  if (control_has_restore && observed_vptr == control.shadow_car_vptr)
    Store(car_vptr_address, control.original_car_vptr);

  if (!CompleteControl(control)) {
    RecordFailure(kStatusInvalidControl);
    return 0;
  }
  if (reinterpret_cast<std::uintptr_t>(car) != control.expected_car) {
    RecordFailure(kStatusUnexpectedCar);
    return 0;
  }
  if (observed_vptr != control.shadow_car_vptr) {
    RecordFailure(kStatusUnexpectedCarVptr);
    return 0;
  }

  AddCounter(&g_evidence.bootstrap_entries);
  g_evidence.last_car = reinterpret_cast<std::uintptr_t>(car);
  g_evidence.last_bootstrap_token =
      reinterpret_cast<std::uintptr_t>(frame_token);
  g_evidence.observed_car_vptr = observed_vptr;
  g_evidence.restored_car_vptr = control.original_car_vptr;

  const std::uint32_t previous_depth =
      g_bootstrap_depth.fetch_add(1u, std::memory_order_acq_rel);
  if (previous_depth != 0) {
    AddCounter(&g_evidence.recursive_entries);
    RecordFailure(kStatusDuplicateBootstrap);
  }

  const auto original =
      reinterpret_cast<FrameCallback>(control.original_car_callback);
  AddCounter(&g_evidence.original_calls);
  const std::int64_t result = original(car, frame_token);
  g_evidence.last_original_result = static_cast<std::uint64_t>(result);
  AddCounter(&g_evidence.original_returns);

  std::uint32_t expected_state = 0;
  if (Load(car_vptr_address) != control.original_car_vptr) {
    RecordFailure(kStatusUnexpectedCarVptr);
    g_registration_state.store(3u, std::memory_order_release);
  } else if (!g_registration_state.compare_exchange_strong(
          expected_state, 1u, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    RecordFailure(kStatusDuplicateBootstrap);
  } else {
    ListMutation add = nullptr;
    ListMutation remove = nullptr;
    if (!ReadMutationMethods(control, &add, &remove)) {
      g_registration_state.store(3u, std::memory_order_release);
    } else {
      const auto* const dispatching = reinterpret_cast<const std::uint8_t*>(
          control.physics_context + kDispatchingOffset);
      const auto* const deferred = reinterpret_cast<const std::uint8_t*>(
          control.physics_context + kDeferredOffset);
      const CallbackListPrefix before = ReadListPrefix(control.physics_context);
      const std::uint8_t dispatch_before = Load(dispatching);
      const std::uint8_t deferred_before = Load(deferred);
      g_evidence.list_begin_before = before.begin;
      g_evidence.list_end_before = before.end;
      g_evidence.list_active_before = before.active_end;
      g_evidence.registration_dispatching_before = dispatch_before;
      g_evidence.registration_deferred_before = deferred_before;
      if (!ValidListShape(before) || dispatch_before != 1 ||
          deferred_before != 0) {
        RecordFailure(kStatusRegistrationOutsideDispatch);
        g_registration_state.store(3u, std::memory_order_release);
      } else {
        void* dedicated = &g_dedicated_object;
        AddCounter(&g_evidence.registration_attempts);
        add(reinterpret_cast<void*>(control.physics_context), &dedicated);
        AddCounter(&g_evidence.registration_returns);
        const CallbackListPrefix after =
            ReadListPrefix(control.physics_context);
        const std::uint8_t dispatch_after = Load(dispatching);
        const std::uint8_t deferred_after = Load(deferred);
        g_evidence.list_begin_after_add = after.begin;
        g_evidence.list_end_after_add = after.end;
        g_evidence.list_active_after_add = after.active_end;
        g_evidence.registration_dispatching_after = dispatch_after;
        g_evidence.registration_deferred_after = deferred_after;
        const std::uintptr_t before_size = before.end - before.begin;
        const std::uintptr_t after_size = after.end - after.begin;
        const std::uintptr_t before_active =
            before.active_end - before.begin;
        const std::uintptr_t after_active =
            after.active_end - after.begin;
        if (!ValidListShape(after) || dispatch_after != 1 ||
            deferred_after != 1 || after_size != before_size + 16u ||
            after_active != before_active) {
          RecordFailure(kStatusRegistrationShape);
          // add() has already returned.  Request removal immediately on this
          // same game-owned producer thread so a failed postcondition cannot
          // leave the payload object referenced until process shutdown.
          (void)RequestDeferredRemoval(control, remove);
          g_registration_state.store(3u, std::memory_order_release);
        } else {
          g_registration_state.store(2u, std::memory_order_release);
          Store(&g_evidence.protocol_state, 1u);
          Store(&g_evidence.last_status,
                static_cast<std::int32_t>(kStatusRegistered));
        }
      }
    }
  }

  g_bootstrap_depth.fetch_sub(1u, std::memory_order_release);
  return result;
}

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_fc2_dedicated_observer_v1(void* object,
                                const std::uint64_t* frame_token) {
  const Control control = g_control;
  if (!CompleteControl(control)) {
    RecordFailure(kStatusInvalidControl);
    return 0;
  }
  if (object != &g_dedicated_object ||
      reinterpret_cast<std::uintptr_t>(object) != control.dedicated_object) {
    RecordFailure(kStatusUnexpectedDedicatedObject);
    return 0;
  }
  const std::uintptr_t vptr =
      Load(reinterpret_cast<const std::uintptr_t*>(object));
  if (vptr != control.dedicated_vptr) {
    RecordFailure(kStatusUnexpectedDedicatedVptr);
    return 0;
  }
  if (g_registration_state.load(std::memory_order_acquire) != 2u) {
    RecordFailure(kStatusRegistrationShape);
    return 0;
  }

  AddCounter(&g_evidence.dedicated_entries);
  g_evidence.last_dedicated_object =
      reinterpret_cast<std::uintptr_t>(object);
  g_evidence.last_dedicated_token =
      reinterpret_cast<std::uintptr_t>(frame_token);

  std::uint32_t expected_state = 0;
  if (!g_removal_state.compare_exchange_strong(
          expected_state, 1u, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    RecordFailure(kStatusDuplicateDedicatedEntry);
    return 0;
  }

  ListMutation add = nullptr;
  ListMutation remove = nullptr;
  if (!ReadMutationMethods(control, &add, &remove)) {
    g_removal_state.store(3u, std::memory_order_release);
    return 0;
  }
  const auto* const dispatching = reinterpret_cast<const std::uint8_t*>(
      control.physics_context + kDispatchingOffset);
  const auto* const deferred = reinterpret_cast<const std::uint8_t*>(
      control.physics_context + kDeferredOffset);
  const std::uint8_t dispatch_before = Load(dispatching);
  const std::uint8_t deferred_before = Load(deferred);
  g_evidence.removal_dispatching_before = dispatch_before;
  g_evidence.removal_deferred_before = deferred_before;
  if (dispatch_before != 1 || deferred_before != 0) {
    RecordFailure(kStatusRemovalOutsideDispatch);
    g_removal_state.store(3u, std::memory_order_release);
    return 0;
  }

  const CallbackListPrefix after = RequestDeferredRemoval(control, remove);
  const std::uint8_t dispatch_after = Load(dispatching);
  const std::uint8_t deferred_after = Load(deferred);
  if (!ValidListShape(after) || dispatch_after != 1 || deferred_after != 1) {
    RecordFailure(kStatusRemovalDidNotDefer);
    g_removal_state.store(3u, std::memory_order_release);
    return 0;
  }

  g_removal_state.store(2u, std::memory_order_release);
  Store(&g_evidence.protocol_state, 2u);
  Store(&g_evidence.last_status,
        static_cast<std::int32_t>(kStatusRemovalRequested));
  return 0;
}

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_fc2_frame_callback_protocol_v1() {
  return 1;
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_fc2_frame_callback_arm_v1(void*, void*) {
  return kStatusRejectedBuildOnly;
}

// Function exports are audit conveniences only.  Host address resolution must
// use the data locators below and must not call a guest export.
extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_fc2_shadow_storage_v1() {
  return reinterpret_cast<std::uintptr_t>(g_primary_shadow);
}
extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_fc2_control_storage_v1() {
  return reinterpret_cast<std::uintptr_t>(&g_control);
}
extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_fc2_evidence_storage_v1() {
  return reinterpret_cast<std::uintptr_t>(&g_evidence);
}
extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_fc2_dedicated_object_v1() {
  return reinterpret_cast<std::uintptr_t>(&g_dedicated_object);
}
extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_fc2_dedicated_vtable_v1() {
  return reinterpret_cast<std::uintptr_t>(&g_dedicated_vtable[2]);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_fc2_shadow_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_primary_shadow);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_fc2_control_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_control);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_fc2_evidence_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_evidence);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_fc2_dedicated_object_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_dedicated_object);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_fc2_dedicated_vtable_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_dedicated_vtable[2]);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_fc2_control_size_data_v1 = sizeof(g_control);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_fc2_evidence_size_data_v1 = sizeof(g_evidence);
