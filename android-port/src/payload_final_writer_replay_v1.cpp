// Build-only proof for AluTasV2-style final transform/linear correction at
// the natural CarPhysicsState callback boundary.
//
// The wrapper deliberately calls the exact original game callback first.  It
// then performs the upstream component-wise 19-float comparison and, only on
// mismatch, copies the complete 64-byte transform and 12-byte linear velocity
// target together.  The shadow vptr is retained only for a finite preloaded
// frame window and is restored on completion or any detected fault.
//
// This artifact has no installer.  Its arm export always returns -100.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "final_writer_replay_protocol_v1.h"

using namespace a9tas::final_writer_replay_v1;

namespace {
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free,
              "final-writer requires lock-free pointer atomics");

using Callback = std::int64_t (*)(void*, const std::uint64_t*);

alignas(64) Control g_control = {
    {kControlMagic[0], kControlMagic[1], kControlMagic[2], kControlMagic[3],
     kControlMagic[4], kControlMagic[5], kControlMagic[6], kControlMagic[7]},
    kProtocolVersion,
    sizeof(Control),
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    {},
    {0, 0},
};

alignas(64) Evidence g_evidence = {
    {kEvidenceMagic[0], kEvidenceMagic[1], kEvidenceMagic[2],
     kEvidenceMagic[3], kEvidenceMagic[4], kEvidenceMagic[5],
     kEvidenceMagic[6], kEvidenceMagic[7]},
    kProtocolVersion,
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
    kStatusPassive,
    0,
    0,
    0,
    0,
    0,
    {0, 0, 0},
};

alignas(64) std::uint8_t g_primary_shadow[kPrimaryShadowSize] = {0xA9};
alignas(64) FrameTarget g_targets[kMaximumFrames]{};
alignas(64) FrameAudit g_audits[kMaximumFrames]{};
std::atomic<std::uint32_t> g_wrapper_depth{0};

void CopyBytes(std::uint8_t* destination, const std::uint8_t* source,
               std::size_t size) {
  for (std::size_t index = 0; index < size; ++index)
    destination[index] = source[index];
}

bool BytesEqual(const std::uint8_t* left, const std::uint8_t* right,
                std::size_t size) {
  for (std::size_t index = 0; index < size; ++index)
    if (left[index] != right[index]) return false;
  return true;
}

bool ComponentsEqual(const std::uint8_t* pose, const std::uint8_t* linear,
                      const FrameTarget& target) {
  float current[kComponentCount]{};
  float recorded[kComponentCount]{};
  CopyBytes(reinterpret_cast<std::uint8_t*>(current), pose, kTransformSize);
  CopyBytes(reinterpret_cast<std::uint8_t*>(current) + kTransformSize,
            linear, kLinearSize);
  CopyBytes(reinterpret_cast<std::uint8_t*>(recorded), target.transform,
            kTransformSize);
  CopyBytes(reinterpret_cast<std::uint8_t*>(recorded) + kTransformSize,
            target.linear, kLinearSize);
  for (std::size_t index = 0; index < kComponentCount; ++index)
    if (current[index] != recorded[index]) return false;
  return true;
}

bool RestoreOriginalVptr(void* object, std::uintptr_t shadow_vptr,
                         std::uintptr_t original_vptr) {
  if (object == nullptr || shadow_vptr == 0 || original_vptr == 0) return false;
  auto* const slot = reinterpret_cast<std::uintptr_t*>(object);
  const std::uintptr_t observed = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (observed == original_vptr) return true;
  if (observed != shadow_vptr) return false;
  __atomic_store_n(slot, original_vptr, __ATOMIC_RELEASE);
  return __atomic_load_n(slot, __ATOMIC_ACQUIRE) == original_vptr;
}

void FailAndRestore(void* object, std::int32_t status,
                    std::uintptr_t shadow_vptr,
                    std::uintptr_t original_vptr) {
  __atomic_add_fetch(&g_evidence.failures, 1u, __ATOMIC_RELAXED);
  const bool restored =
      RestoreOriginalVptr(object, shadow_vptr, original_vptr);
  __atomic_store_n(&g_evidence.final_vptr,
                   restored ? original_vptr : 0, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_status, status, __ATOMIC_RELEASE);
}

}  // namespace

extern "C" __attribute__((noinline, visibility("default"))) std::int64_t
a9tas_final_writer_replay_callback_v1(void* object,
                                      const std::uint64_t* frame_token) {
  const std::uint32_t control_flags =
      __atomic_load_n(&g_control.flags, __ATOMIC_ACQUIRE);
  const std::uint32_t frame_count =
      __atomic_load_n(&g_control.frame_count, __ATOMIC_ACQUIRE);
  const std::uintptr_t expected_object =
      __atomic_load_n(&g_control.expected_object, __ATOMIC_ACQUIRE);
  const std::uintptr_t original_vptr =
      __atomic_load_n(&g_control.original_vptr, __ATOMIC_ACQUIRE);
  const std::uintptr_t shadow_vptr =
      __atomic_load_n(&g_control.shadow_vptr, __ATOMIC_ACQUIRE);
  const std::uintptr_t original_callback =
      __atomic_load_n(&g_control.original_callback, __ATOMIC_ACQUIRE);
  const std::uintptr_t native_pose =
      __atomic_load_n(&g_control.native_pose, __ATOMIC_ACQUIRE);
  const std::uintptr_t native_linear =
      __atomic_load_n(&g_control.native_linear, __ATOMIC_ACQUIRE);

  const bool control_complete =
      control_flags == (kControlConfigured | kControlTargetsLoaded) &&
      frame_count > 0 && frame_count <= kMaximumFrames &&
      expected_object != 0 && original_vptr != 0 && shadow_vptr != 0 &&
      original_callback != 0 && native_pose != 0 && native_linear != 0;
  if (!control_complete || object == nullptr) {
    FailAndRestore(object, kStatusInvalidControl, shadow_vptr, original_vptr);
    return 0;
  }
  if (reinterpret_cast<std::uintptr_t>(object) != expected_object) {
    FailAndRestore(object, kStatusUnexpectedObject, shadow_vptr, original_vptr);
    return 0;
  }

  auto* const object_vptr = reinterpret_cast<std::uintptr_t*>(object);
  const std::uintptr_t observed_vptr =
      __atomic_load_n(object_vptr, __ATOMIC_ACQUIRE);
  if (observed_vptr != shadow_vptr) {
    FailAndRestore(object, kStatusUnexpectedVptr, shadow_vptr, original_vptr);
    return 0;
  }

  const std::uint32_t frame_index =
      __atomic_load_n(&g_evidence.processed_frames, __ATOMIC_ACQUIRE);
  if (frame_index >= frame_count || frame_index >= kMaximumFrames) {
    FailAndRestore(object, kStatusFrameOverflow, shadow_vptr, original_vptr);
    return 0;
  }

  // Match upstream's replay_current_frame_inputs.has_value() contract.  The
  // hook may remain installed while no replay packet is active; in that case
  // it must call the real writer and leave the replay cursor untouched.  A
  // single atomic claim also prevents two concurrent callbacks from consuming
  // one external tick permit.
  const auto original = reinterpret_cast<Callback>(original_callback);
  std::uint64_t expected_permit = FramePermit(frame_index);
  if (!__atomic_compare_exchange_n(
          &g_control.reserved[0], &expected_permit, kFramePermitDisarmed,
          false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return original(object, frame_token);

  __atomic_add_fetch(&g_evidence.wrapper_entries, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_object,
                   reinterpret_cast<std::uintptr_t>(object),
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_token,
                   reinterpret_cast<std::uintptr_t>(frame_token),
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.observed_vptr, observed_vptr,
                   __ATOMIC_RELAXED);

  const std::uint32_t previous_depth =
      g_wrapper_depth.fetch_add(1u, std::memory_order_acq_rel);
  if (previous_depth != 0) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    __atomic_add_fetch(&g_evidence.recursive_entries, 1u, __ATOMIC_RELAXED);
    FailAndRestore(object, kStatusRecursiveEntry, shadow_vptr, original_vptr);
    return original(object, frame_token);
  }

  // AluTasV2 ordering: the complete real writer/callback runs first.
  __atomic_add_fetch(&g_evidence.original_calls, 1u, __ATOMIC_RELAXED);
  const std::int64_t result = original(object, frame_token);

  FrameAudit& audit = g_audits[frame_index];
  const FrameTarget& target = g_targets[frame_index];
  auto* const pose = reinterpret_cast<std::uint8_t*>(native_pose);
  auto* const linear = reinterpret_cast<std::uint8_t*>(native_linear);
  audit.frame_index = frame_index;
  audit.flags = kAuditOriginalReturned;
  CopyBytes(audit.before_transform, pose, kTransformSize);
  CopyBytes(audit.before_linear, linear, kLinearSize);

  const bool equal = ComponentsEqual(pose, linear, target);
  if (equal) {
    audit.flags |= kAuditEqual;
    __atomic_add_fetch(&g_evidence.equal_frames, 1u, __ATOMIC_RELAXED);
  } else {
    // The two original payload ranges are one all-or-nothing semantic unit.
    CopyBytes(pose, target.transform, kTransformSize);
    CopyBytes(linear, target.linear, kLinearSize);
    audit.flags |= kAuditCorrected;
    __atomic_add_fetch(&g_evidence.corrected_frames, 1u,
                       __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_evidence.correction_writes, 2u,
                       __ATOMIC_RELAXED);
  }

  CopyBytes(audit.immediate_transform, pose, kTransformSize);
  CopyBytes(audit.immediate_linear, linear, kLinearSize);
  const bool immediate_exact =
      BytesEqual(audit.immediate_transform, target.transform,
                 kTransformSize) &&
      BytesEqual(audit.immediate_linear, target.linear, kLinearSize);
  if (!immediate_exact) {
    g_wrapper_depth.fetch_sub(1u, std::memory_order_release);
    FailAndRestore(object, kStatusImmediateAuditMismatch, shadow_vptr,
                   original_vptr);
    return result;
  }
  audit.flags |= kAuditImmediateExact;

  const std::uint32_t processed = frame_index + 1;
  __atomic_store_n(&g_evidence.processed_frames, processed,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.last_result, result, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_evidence.clean_returns, 1u, __ATOMIC_RELAXED);
  g_wrapper_depth.fetch_sub(1u, std::memory_order_release);

  if (processed == frame_count) {
    audit.flags |= kAuditFinalFrame;
    const bool restored =
        RestoreOriginalVptr(object, shadow_vptr, original_vptr);
    if (!restored) {
      FailAndRestore(object, kStatusUnexpectedVptr, shadow_vptr,
                     original_vptr);
      return result;
    }
    audit.flags |= kAuditVptrRestored;
    __atomic_store_n(&g_evidence.final_vptr, original_vptr,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&g_evidence.last_status, kStatusComplete,
                     __ATOMIC_RELEASE);
  } else {
    __atomic_store_n(&g_evidence.last_status, kStatusPassive,
                     __ATOMIC_RELEASE);
  }
  return result;
}

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_final_writer_replay_protocol_v1() {
  return kProtocolVersion;
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_final_writer_replay_arm_v1(void*, void*) {
  return kStatusRejectedBuildOnly;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_final_writer_replay_shadow_storage_v1() {
  return reinterpret_cast<std::uintptr_t>(g_primary_shadow);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_final_writer_replay_control_storage_v1() {
  return reinterpret_cast<std::uintptr_t>(&g_control);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_final_writer_replay_target_storage_v1() {
  return reinterpret_cast<std::uintptr_t>(g_targets);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_final_writer_replay_audit_storage_v1() {
  return reinterpret_cast<std::uintptr_t>(g_audits);
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
a9tas_final_writer_replay_evidence_storage_v1() {
  return reinterpret_cast<std::uintptr_t>(&g_evidence);
}

extern "C" __attribute__((visibility("default"))) std::uint64_t
a9tas_final_writer_replay_maximum_frames_v1() {
  return kMaximumFrames;
}

extern "C" __attribute__((visibility("default"))) std::uint64_t
a9tas_final_writer_replay_original_callback_rva_v1() {
  return kOriginalCallbackRva;
}

// Relocation-backed data locators let a future offline-reviewed host
// controller resolve payload-owned storage without calling guest exports.
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_final_writer_replay_shadow_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_primary_shadow);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_final_writer_replay_control_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_control);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_final_writer_replay_target_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_targets);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_final_writer_replay_audit_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_audits);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_final_writer_replay_evidence_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_evidence);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_final_writer_replay_control_size_data_v1 = sizeof(g_control);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_final_writer_replay_target_size_data_v1 = sizeof(FrameTarget);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_final_writer_replay_audit_size_data_v1 = sizeof(FrameAudit);
extern "C" __attribute__((visibility("default"))) std::uint64_t
    a9tas_final_writer_replay_evidence_size_data_v1 = sizeof(g_evidence);
