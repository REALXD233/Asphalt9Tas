// FC-0 build-only proof for the natural PhysicsContext frame callback route.
//
// This artifact intentionally has no runtime configuration or installation
// path.  The exported arm entry always returns -100.  The pass-through wrapper
// is present only so its AArch64 ABI and ordering can be audited before a
// separately reviewed live candidate is ever created.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace {

constexpr std::uintptr_t kPrimaryPrefixRva = 0x7EE8CC0;
constexpr std::uintptr_t kPrimaryAddressPointRva = 0x7EE8D18;
constexpr std::uintptr_t kNextAddressPointRva = 0x7EE95C8;
constexpr std::uintptr_t kOriginalCallbackRva = 0x367D66C;
constexpr std::size_t kPrimaryPrefixSize = 0x58;
constexpr std::size_t kPrimaryShadowSize = 0x908;
constexpr std::size_t kCallbackSlotOffset = 0x10;

static_assert(kPrimaryAddressPointRva - kPrimaryPrefixRva ==
                  kPrimaryPrefixSize,
              "FC-0 primary prefix boundary");
static_assert(kNextAddressPointRva - kPrimaryPrefixRva ==
                  kPrimaryShadowSize,
              "FC-0 primary vtable boundary");
static_assert(kPrimaryPrefixSize + kCallbackSlotOffset + sizeof(void*) <=
                  kPrimaryShadowSize,
              "FC-0 callback slot lies inside primary shadow");
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free,
              "FC-0 requires lock-free pointer atomics");

using Callback = std::int64_t (*)(void*, const std::uint64_t*);

enum Status : std::int32_t {
  kStatusPassive = 0,
  kStatusRejectedBuildOnly = -100,
  kStatusUnexpectedObject = -201,
  kStatusInvalidControl = -202,
  kStatusUnexpectedVptr = -203,
};

struct alignas(64) Control {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uintptr_t expected_object;
  std::uintptr_t original_vptr;
  std::uintptr_t shadow_vptr;
  std::uintptr_t original_callback;
  std::uint64_t reserved[2];
};

struct alignas(64) Evidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t wrapper_entries;
  std::uint64_t original_calls;
  std::uint64_t clean_returns;
  std::uint64_t recursive_entries;
  std::uint64_t failures;
  std::uintptr_t last_object;
  std::uintptr_t last_token;
  std::uintptr_t observed_vptr;
  std::uintptr_t restored_vptr;
  std::int64_t last_result;
  std::int32_t last_status;
  std::uint32_t reserved;
};

static_assert(sizeof(Control) == 64, "FC-0 control ABI");
static_assert(sizeof(Evidence) == 128, "FC-0 evidence ABI");

alignas(64) Control g_control = {
    {'A', '9', 'F', 'C', '0', 'C', '1', 0},
    1,
    sizeof(Control),
    0,
    0,
    0,
    0,
    {0, 0},
};

alignas(64) Evidence g_evidence = {
    {'A', '9', 'F', 'C', '0', 'E', '1', 0},
    1,
    sizeof(Evidence),
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    kStatusPassive,
    0,
};

// The future live candidate will copy exactly [kPrimaryPrefixRva,
// kNextAddressPointRva) here and point the object at +kPrimaryPrefixSize.
// FC-0 exposes no function capable of populating or installing this storage.
// A nonzero sentinel keeps the whole shadow in the file-backed .data segment
// instead of an anonymous BSS extension.  FC-1 overwrites all 0x908 bytes
// before arming, so the sentinel is never visible to the game.
alignas(64) std::uint8_t g_primary_shadow[kPrimaryShadowSize] = {0xA9};
std::atomic<std::uint32_t> g_wrapper_depth{0};

void RecordFailure(std::int32_t status) {
  __atomic_add_fetch(&g_evidence.failures, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_status, status, __ATOMIC_RELEASE);
}

}  // namespace

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_fc0_frame_callback_passthrough_v1(void* object,
                                        const std::uint64_t* frame_token) {
  const std::uintptr_t expected_object = __atomic_load_n(
      &g_control.expected_object, __ATOMIC_ACQUIRE);
  const std::uintptr_t original_vptr = __atomic_load_n(
      &g_control.original_vptr, __ATOMIC_ACQUIRE);
  const std::uintptr_t shadow_vptr = __atomic_load_n(
      &g_control.shadow_vptr, __ATOMIC_ACQUIRE);
  const std::uintptr_t original_callback = __atomic_load_n(
      &g_control.original_callback, __ATOMIC_ACQUIRE);

  if (object == nullptr) {
    RecordFailure(kStatusUnexpectedObject);
    return 0;
  }

  auto* const object_vptr = reinterpret_cast<std::uintptr_t*>(object);
  const std::uintptr_t observed_vptr =
      __atomic_load_n(object_vptr, __ATOMIC_ACQUIRE);

  // Restore only the exact shadow installed by FC-1.  This is the first
  // possible mutation on every path with a complete control block, including
  // an unexpected-object failure.  Never overwrite an unrelated third-party
  // vptr merely because the wrapper was entered.
  const bool control_complete =
      original_vptr != 0 && shadow_vptr != 0 && original_callback != 0;
  if (control_complete && observed_vptr == shadow_vptr) {
    __atomic_store_n(object_vptr, original_vptr, __ATOMIC_RELEASE);
  }

  if (!control_complete) {
    RecordFailure(kStatusInvalidControl);
    return 0;
  }
  if (reinterpret_cast<std::uintptr_t>(object) != expected_object) {
    RecordFailure(kStatusUnexpectedObject);
    return 0;
  }
  if (observed_vptr != shadow_vptr && observed_vptr != original_vptr) {
    RecordFailure(kStatusUnexpectedVptr);
    return 0;
  }

  __atomic_add_fetch(&g_evidence.wrapper_entries, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_object,
                   reinterpret_cast<std::uintptr_t>(object),
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_token,
                   reinterpret_cast<std::uintptr_t>(frame_token),
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.observed_vptr, observed_vptr,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.restored_vptr, original_vptr,
                   __ATOMIC_RELAXED);

  if (observed_vptr != shadow_vptr) {
    RecordFailure(kStatusUnexpectedVptr);
  }

  const std::uint32_t previous_depth =
      g_wrapper_depth.fetch_add(1u, std::memory_order_acq_rel);
  if (previous_depth != 0) {
    __atomic_add_fetch(&g_evidence.recursive_entries, 1u, __ATOMIC_RELAXED);
  }

  // Every valid wrapper entry calls the exact original callback exactly once
  // with x0/x1 unchanged and propagates x0 back to the broadcaster.
  const auto original = reinterpret_cast<Callback>(original_callback);
  __atomic_add_fetch(&g_evidence.original_calls, 1u, __ATOMIC_RELAXED);
  const std::int64_t result = original(object, frame_token);

  g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
  __atomic_store_n(&g_evidence.last_result, result, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_evidence.clean_returns, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_status,
                   observed_vptr == shadow_vptr ? kStatusPassive
                                                : kStatusUnexpectedVptr,
                   __ATOMIC_RELEASE);
  return result;
}

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_fc0_frame_callback_protocol_v1() {
  return 1;
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_fc0_frame_callback_arm_v1(void*, void*) {
  return kStatusRejectedBuildOnly;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_fc0_frame_callback_shadow_storage_v1() {
  return reinterpret_cast<std::uintptr_t>(g_primary_shadow);
}

extern "C" __attribute__((visibility("default"))) std::uint64_t
a9tas_fc0_frame_callback_shadow_size_v1() {
  return sizeof(g_primary_shadow);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_fc0_frame_callback_control_storage_v1() {
  return reinterpret_cast<std::uintptr_t>(&g_control);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_fc0_frame_callback_evidence_storage_v1() {
  return reinterpret_cast<std::uintptr_t>(&g_evidence);
}

extern "C" __attribute__((visibility("default"))) std::uint64_t
a9tas_fc0_frame_callback_primary_prefix_rva_v1() {
  return kPrimaryPrefixRva;
}

extern "C" __attribute__((visibility("default"))) std::uint64_t
a9tas_fc0_frame_callback_primary_address_point_rva_v1() {
  return kPrimaryAddressPointRva;
}

extern "C" __attribute__((visibility("default"))) std::uint64_t
a9tas_fc0_frame_callback_next_address_point_rva_v1() {
  return kNextAddressPointRva;
}

extern "C" __attribute__((visibility("default"))) std::uint64_t
a9tas_fc0_frame_callback_original_callback_rva_v1() {
  return kOriginalCallbackRva;
}

// Data exports are addresses of payload-owned storage, not game state.  They
// let a future host-only FC-1 controller locate and validate the passive
// buffers without making a guest call.  FC-0 still has no installer and its
// arm function remains a constant -100.
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_fc0_frame_callback_shadow_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_primary_shadow);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_fc0_frame_callback_control_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_control);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_fc0_frame_callback_evidence_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_evidence);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_fc0_frame_callback_control_size_data_v1 = sizeof(g_control);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_fc0_frame_callback_evidence_size_data_v1 = sizeof(g_evidence);
