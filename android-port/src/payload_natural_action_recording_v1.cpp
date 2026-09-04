// Passive, preload-only NitroService wrapper for source recording.
// It never submits an action and never writes Nitro state or colour.

#include "natural_action_recording_protocol_v1.h"

#include <cstdint>
#include <cstring>

using namespace a9tas::natural_action_recording_v1;

namespace {

using Activate = void (*)(void*);

alignas(64) Control g_control = {
    {kControlMagic[0], kControlMagic[1], kControlMagic[2], kControlMagic[3],
     kControlMagic[4], kControlMagic[5], kControlMagic[6], kControlMagic[7]},
    kVersion, sizeof(Control), 0, 0, 0, 0, 0, 0, 0, 0, 0, {}};

alignas(64) Evidence g_evidence = {
    {kEvidenceMagic[0], kEvidenceMagic[1], kEvidenceMagic[2],
     kEvidenceMagic[3], kEvidenceMagic[4], kEvidenceMagic[5],
     kEvidenceMagic[6], kEvidenceMagic[7]},
    kVersion, sizeof(Evidence), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    kPassive, 0, {}};

alignas(64) std::uint32_t g_counts[kMaximumFrames]{};
alignas(64) std::uint8_t g_shadow[kShadowVtableSize]{};

template <typename T>
T Load(const T* value) {
  return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

template <typename T>
void Store(T* value, T next) {
  __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

// The recorder is entered by the authoritative FrameThread.  Do not use an
// outlined atomic RMW helper here: under LDPlayer's NativeBridge that inserts
// extra guest-runtime calls inside the Nitro virtual dispatch.  A single-
// producer acquire/load + release/store keeps the payload leaf-like while the
// host still observes every committed value with the matching ordering.
template <typename T>
void IncrementSingleProducer(T* value) {
  Store(value, static_cast<T>(Load(value) + 1u));
}

}  // namespace

extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_natural_action_recording_wrapper_v1(void* service) {
  IncrementSingleProducer(&g_evidence.wrapper_entries);
  Store(&g_evidence.last_service,
        reinterpret_cast<std::uintptr_t>(service));

  const auto original_address = Load(&g_control.original_activate);
  const auto original = reinterpret_cast<Activate>(original_address);
  if (original_address == 0) {
    IncrementSingleProducer(&g_evidence.failures);
    Store(&g_evidence.last_status,
          static_cast<std::int32_t>(kUnexpectedService));
    return;
  }

  std::int32_t status = kCounted;
  const auto expected_service = Load(&g_control.expected_service);
  const auto shadow_vptr = Load(&g_control.shadow_vptr);
  const auto flags = Load(&g_control.flags);
  std::uintptr_t observed_vptr = 0;
  if (service != nullptr)
    observed_vptr = Load(reinterpret_cast<std::uintptr_t*>(service));
  Store(&g_evidence.observed_vptr, observed_vptr);

  if (flags != (kConfigured | kInstalled) || service == nullptr ||
      reinterpret_cast<std::uintptr_t>(service) != expected_service) {
    status = kUnexpectedService;
    IncrementSingleProducer(&g_evidence.failures);
  } else if (observed_vptr != shadow_vptr) {
    status = kUnexpectedVptr;
    IncrementSingleProducer(&g_evidence.failures);
  } else {
    const std::uint64_t sequence = Load(&g_control.active_sequence);
    const std::uint32_t frame_count = Load(&g_control.frame_count);
    Store(&g_evidence.last_sequence, sequence);
    if (sequence == 0 || sequence > frame_count ||
        sequence > kMaximumFrames) {
      status = kOutOfWindow;
      IncrementSingleProducer(&g_evidence.out_of_window_calls);
    } else {
      const std::uint32_t index = static_cast<std::uint32_t>(sequence - 1u);
      const std::uint32_t before = Load(&g_counts[index]);
      Store(&g_counts[index], before + 1u);
      IncrementSingleProducer(&g_evidence.counted_calls);
      if (before >= 2u) {
        status = kFrameOverflow;
        IncrementSingleProducer(&g_evidence.overflow_calls);
      }
    }
  }

  IncrementSingleProducer(&g_evidence.original_calls);
  original(service);
  IncrementSingleProducer(&g_evidence.clean_returns);
  Store(&g_evidence.last_status, status);
}

extern "C" __attribute__((visibility("default"))) void*
a9tas_natural_action_recording_control_storage_v1() { return &g_control; }
extern "C" __attribute__((visibility("default"))) void*
a9tas_natural_action_recording_evidence_storage_v1() { return &g_evidence; }
extern "C" __attribute__((visibility("default"))) void*
a9tas_natural_action_recording_counts_storage_v1() { return g_counts; }
extern "C" __attribute__((visibility("default"))) void*
a9tas_natural_action_recording_shadow_storage_v1() { return g_shadow; }

extern "C" __attribute__((used, visibility("default"))) void*
a9tas_natural_action_recording_control_storage_data_v1 = &g_control;
extern "C" __attribute__((used, visibility("default"))) void*
a9tas_natural_action_recording_evidence_storage_data_v1 = &g_evidence;
extern "C" __attribute__((used, visibility("default"))) void*
a9tas_natural_action_recording_counts_storage_data_v1 = g_counts;
extern "C" __attribute__((used, visibility("default"))) void*
a9tas_natural_action_recording_shadow_storage_data_v1 = g_shadow;

extern "C" __attribute__((used, visibility("default"))) std::uint32_t
a9tas_natural_action_recording_control_size_data_v1 = sizeof(g_control);
extern "C" __attribute__((used, visibility("default"))) std::uint32_t
a9tas_natural_action_recording_evidence_size_data_v1 = sizeof(g_evidence);
extern "C" __attribute__((used, visibility("default"))) std::uint32_t
a9tas_natural_action_recording_counts_size_data_v1 = sizeof(g_counts);
extern "C" __attribute__((used, visibility("default"))) std::uint32_t
a9tas_natural_action_recording_shadow_size_data_v1 = sizeof(g_shadow);
