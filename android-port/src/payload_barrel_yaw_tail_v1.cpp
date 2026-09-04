// BUILD-ONLY ARM64 payload for the late, source-equivalent BarrelYaw tail.
//
// A reviewed host transaction installs a transient shadow vptr inside the
// certified post-C9C physics window.  The exact BarrelYaw path invokes body
// vslot +0x68 at the two pinned return addresses below.  Other users of the
// same vslot remain complete natural pass-through calls and do not consume the
// replay token.  The two qualified calls run naturally first; only after the
// second return is the AluTasV2 angular tail applied.  Each completed or failed
// transaction restores the original vptr immediately; the host never leaves
// this exact-build table replacement installed across a full frame.

#include "barrel_stabilization_replay_core_v1.h"
#include "barrel_yaw_tail_payload_protocol_v1.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>

namespace protocol = a9tas::barrel_yaw_tail_payload_v1;
namespace semantic = a9tas::barrel_stabilization_replay_v1;

namespace {

using BoundaryCallback = void (*)(void*, const void*, std::uint32_t);

alignas(64) protocol::Control g_control = {
    {protocol::kControlMagic[0], protocol::kControlMagic[1],
     protocol::kControlMagic[2], protocol::kControlMagic[3],
     protocol::kControlMagic[4], protocol::kControlMagic[5],
     protocol::kControlMagic[6], protocol::kControlMagic[7]},
    protocol::kProtocolVersion,
    sizeof(protocol::Control),
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
    0,
    {},
    protocol::kDisarmedToken,
    0,
    0,
};

alignas(64) protocol::Evidence g_evidence = {
    {protocol::kEvidenceMagic[0], protocol::kEvidenceMagic[1],
     protocol::kEvidenceMagic[2], protocol::kEvidenceMagic[3],
     protocol::kEvidenceMagic[4], protocol::kEvidenceMagic[5],
     protocol::kEvidenceMagic[6], protocol::kEvidenceMagic[7]},
    protocol::kProtocolVersion,
    sizeof(protocol::Evidence),
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    protocol::kStatusPassive,
    0,
    0,
    0,
    0,
    {0, 0, 0, 0, 0},
};

alignas(64) std::uint8_t g_shadow[protocol::kShadowSize] = {0xA9};
alignas(64) protocol::FrameTarget g_targets[protocol::kMaximumFrames]{};
alignas(64) protocol::TransactionAudit
    g_audits[protocol::kMaximumTransactions]{};

std::uint32_t CurrentTid() {
  return static_cast<std::uint32_t>(syscall(SYS_gettid));
}

bool BytesEqual(const std::uint32_t* left, const std::uint32_t* right,
                std::size_t count) {
  return std::memcmp(left, right, count * sizeof(*left)) == 0;
}

bool RestoreOriginalVptr() {
  const std::uintptr_t object =
      __atomic_load_n(&g_control.expected_object, __ATOMIC_ACQUIRE);
  const std::uintptr_t original =
      __atomic_load_n(&g_control.original_vptr, __ATOMIC_ACQUIRE);
  const std::uintptr_t shadow =
      __atomic_load_n(&g_control.shadow_vptr, __ATOMIC_ACQUIRE);
  if (object == 0 || original == 0 || shadow == 0) return false;
  auto* const slot = reinterpret_cast<std::uintptr_t*>(object);
  std::uintptr_t observed = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (observed == original) return true;
  if (observed != shadow) return false;
  if (!__atomic_compare_exchange_n(slot, &observed, original, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return false;
  return __atomic_load_n(slot, __ATOMIC_ACQUIRE) == original;
}

bool RestoreOriginalVptrFromShadow() {
  const std::uintptr_t object =
      __atomic_load_n(&g_control.expected_object, __ATOMIC_ACQUIRE);
  const std::uintptr_t original =
      __atomic_load_n(&g_control.original_vptr, __ATOMIC_ACQUIRE);
  const std::uintptr_t shadow =
      __atomic_load_n(&g_control.shadow_vptr, __ATOMIC_ACQUIRE);
  if (object == 0 || original == 0 || shadow == 0) return false;
  auto* const slot = reinterpret_cast<std::uintptr_t*>(object);
  std::uintptr_t expected = shadow;
  if (!__atomic_compare_exchange_n(slot, &expected, original, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return false;
  return __atomic_load_n(slot, __ATOMIC_ACQUIRE) == original;
}

[[noreturn]] void Fail(std::int32_t status) {
  // Preserve the same reverse-publication order as the success path.  If the
  // exact shadow cannot be conditionally restored, leave token/call state
  // armed so the host cannot mistake this mutation-uncertain process for a
  // resumable rollback.
  const bool restored = RestoreOriginalVptr();
  if (restored) {
    __atomic_store_n(&g_control.active_token, protocol::kDisarmedToken,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_evidence.active_call_count, 0u, __ATOMIC_RELEASE);
  }
  __atomic_add_fetch(&g_evidence.failures, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.final_vptr,
                   restored ? g_control.original_vptr : 0,
                   __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_status, status, __ATOMIC_RELEASE);
  // A failed source-bound transaction is never allowed to execute the
  // remainder of the physics frame.  Host-side F64 checks would be too late
  // after a partial angular write or failed vptr restoration.
  __builtin_trap();
  __builtin_unreachable();
}

bool StaticControlValid(std::uint64_t token, std::uint32_t* frame_index,
                        std::uint32_t* audit_index) {
  const std::uint32_t flags =
      __atomic_load_n(&g_control.flags, __ATOMIC_ACQUIRE);
  const std::uint32_t count =
      __atomic_load_n(&g_control.frame_count, __ATOMIC_ACQUIRE);
  const std::uint32_t generation =
      __atomic_load_n(&g_control.session_generation, __ATOMIC_ACQUIRE);
  const std::uint32_t arm_sequence = static_cast<std::uint32_t>(token);
  const std::uint32_t active_frame_index =
      __atomic_load_n(&g_control.active_frame_index, __ATOMIC_ACQUIRE);
  if (flags != (protocol::kControlConfigured |
                protocol::kControlTargetsLoaded) ||
      count == 0 || count > protocol::kMaximumFrames || generation == 0 ||
      arm_sequence == 0 || active_frame_index >= count ||
      static_cast<std::uint32_t>(token >> 32) != generation ||
      g_control.expected_object == 0 || g_control.original_vptr == 0 ||
      g_control.shadow_vptr == 0 ||
      g_control.original_boundary_callback == 0 ||
      g_control.native_angular == 0 || g_control.expected_tid == 0 ||
      g_control.expected_first_caller_return == 0 ||
      g_control.expected_second_caller_return == 0 ||
      g_control.expected_first_caller_return ==
          g_control.expected_second_caller_return) {
    return false;
  }
  *frame_index = active_frame_index;
  *audit_index =
      __atomic_load_n(&g_control.active_audit_index, __ATOMIC_ACQUIRE);
  return *audit_index < protocol::kMaximumTransactions;
}

}  // namespace

extern "C" __attribute__((noinline, visibility("default"))) void
a9tas_barrel_yaw_tail_boundary_v1(void* object, const void* value,
                                  std::uint32_t mode) {
  const std::uintptr_t caller_return = reinterpret_cast<std::uintptr_t>(
      __builtin_extract_return_addr(__builtin_return_address(0)));
  const std::uintptr_t original_address =
      __atomic_load_n(&g_control.original_boundary_callback,
                      __ATOMIC_ACQUIRE);
  if (original_address == 0) {
    Fail(protocol::kStatusInvalidControl);
    return;
  }

  // AluTasV2 ordering is non-negotiable: the complete natural method runs
  // before any replay validation or correction in this wrapper.
  const auto original = reinterpret_cast<BoundaryCallback>(original_address);
  original(object, value, mode);

  const std::uint64_t token =
      __atomic_load_n(&g_control.active_token, __ATOMIC_ACQUIRE);
  if (token == protocol::kDisarmedToken) return;

  const std::uint32_t tid = CurrentTid();

  std::uint32_t frame_index = 0;
  std::uint32_t audit_index = 0;
  if (!StaticControlValid(token, &frame_index, &audit_index)) {
    Fail(audit_index >= protocol::kMaximumTransactions
             ? protocol::kStatusAuditOverflow
             : protocol::kStatusInvalidControl);
    return;
  }
  if (reinterpret_cast<std::uintptr_t>(object) != g_control.expected_object) {
    Fail(protocol::kStatusUnexpectedObject);
    return;
  }
  const std::uintptr_t observed_vptr =
      __atomic_load_n(reinterpret_cast<std::uintptr_t*>(object),
                      __ATOMIC_ACQUIRE);
  if (observed_vptr != g_control.shadow_vptr) {
    Fail(protocol::kStatusUnexpectedVptr);
    return;
  }
  if (__atomic_load_n(&g_control.active_token, __ATOMIC_ACQUIRE) != token) {
    Fail(protocol::kStatusStaleToken);
    return;
  }

  // The shadow covers the complete exact-build vtable, but only the two
  // BarrelYaw call sites are replay semantics.  A broad auxiliary-word
  // watchpoint can arm slightly before the real setter, so unrelated +0x68
  // calls (including calls on another thread) must remain transparent.  Known
  // BarrelYaw calls still have to occur on the authoritative owner thread.
  const bool first_caller =
      caller_return == g_control.expected_first_caller_return;
  const bool second_caller =
      caller_return == g_control.expected_second_caller_return;
  if (!first_caller && !second_caller) return;
  if (tid != g_control.expected_tid) {
    Fail(protocol::kStatusUnexpectedThread);
    return;
  }

  std::uint32_t previous_call_count =
      __atomic_load_n(&g_evidence.active_call_count, __ATOMIC_ACQUIRE);
  const std::uintptr_t expected_return =
      previous_call_count == 0
          ? g_control.expected_first_caller_return
          : (previous_call_count == 1
                 ? g_control.expected_second_caller_return
                 : 0);
  if (expected_return == 0 || caller_return != expected_return) {
    Fail(protocol::kStatusUnexpectedCallerReturn);
    return;
  }

  // Evidence counts qualified natural calls only.  Unrelated calls already
  // completed above but leave the replay transaction byte-for-byte unchanged.
  __atomic_add_fetch(&g_evidence.wrapper_entries, 1u, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_evidence.original_calls, 1u, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_evidence.original_returns, 1u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_tid, tid, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_token, token, __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_object,
                   reinterpret_cast<std::uintptr_t>(object),
                   __ATOMIC_RELAXED);
  if (!__atomic_compare_exchange_n(&g_evidence.active_call_count,
                                   &previous_call_count,
                                   previous_call_count + 1u, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    Fail(protocol::kStatusUnexpectedCallCount);
    return;
  }

  protocol::TransactionAudit& audit = g_audits[audit_index];
  audit.token = token;
  audit.frame_index = frame_index;
  audit.tid = tid;
  const std::uint32_t call_count = previous_call_count + 1u;
  audit.natural_calls = call_count;
  if (call_count == 1) {
    audit.flags = protocol::kAuditFirstNaturalCallReturned;
    __atomic_store_n(&g_evidence.last_status, protocol::kStatusPassive,
                     __ATOMIC_RELEASE);
    return;
  }
  if (call_count != 2) {
    Fail(protocol::kStatusUnexpectedCallCount);
    return;
  }

  audit.flags |= protocol::kAuditSecondNaturalCallReturned;
  auto* const live =
      reinterpret_cast<std::uint32_t*>(g_control.native_angular);
  std::memcpy(audit.before_bits, live, sizeof(audit.before_bits));

  semantic::ActiveFrame active{};
  active.has_value = true;
  active.permit = {g_control.session_generation, frame_index, 0};
  active.skip_override_flags = g_targets[frame_index].skip_override_flags;
  std::memcpy(active.angular_bits, g_targets[frame_index].angular_bits,
              sizeof(active.angular_bits));
  semantic::PostOriginalContext context{
      {g_control.session_generation, frame_index, 0}, true, {}};
  semantic::Capture capture{};
  const semantic::Result result = semantic::OnBarrelYawPostOriginal(
      active, context, live, &capture);
  std::memcpy(audit.immediate_bits, live, sizeof(audit.immediate_bits));
  std::memcpy(audit.captured_bits, capture.angular_bits,
              sizeof(audit.captured_bits));

  if (result == semantic::Result::kOverridden) {
    audit.flags |= protocol::kAuditOverridden;
    __atomic_add_fetch(&g_evidence.override_writes, 1u, __ATOMIC_RELAXED);
    if (!BytesEqual(audit.immediate_bits,
                    g_targets[frame_index].angular_bits, 3)) {
      Fail(protocol::kStatusImmediateAuditMismatch);
      return;
    }
    audit.flags |= protocol::kAuditImmediateExact;
  } else if (result == semantic::Result::kNaturalSkipped) {
    audit.flags |= protocol::kAuditSkipped | protocol::kAuditImmediateExact;
    __atomic_add_fetch(&g_evidence.skipped_transactions, 1u,
                       __ATOMIC_RELAXED);
  } else {
    Fail(protocol::kStatusStaleToken);
    return;
  }

  // The shadow was the final host publish write, so successful completion must
  // reverse that order: restore the exact shadow to the exact original first,
  // then disarm.  A successful CAS proves that no alien vptr was overwritten.
  if (!RestoreOriginalVptrFromShadow()) {
    Fail(protocol::kStatusVptrRestoreFailed);
  }
  audit.flags |= protocol::kAuditVptrRestored;
  __atomic_store_n(&g_control.active_token, protocol::kDisarmedToken,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.active_call_count, 0u, __ATOMIC_RELEASE);
  audit.flags |= protocol::kAuditTokenDisarmed;
  __atomic_store_n(&g_evidence.final_vptr, g_control.original_vptr,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&g_evidence.audit_count, audit_index + 1u,
                   __ATOMIC_RELEASE);
  __atomic_add_fetch(&g_evidence.completed_transactions, 1u,
                     __ATOMIC_RELAXED);
  __atomic_store_n(&g_evidence.last_status, protocol::kStatusComplete,
                   __ATOMIC_RELEASE);
}

extern "C" __attribute__((visibility("default"))) std::int32_t
a9tas_barrel_yaw_tail_arm_v1(void*, void*) {
  return protocol::kStatusRejectedBuildOnly;
}

extern "C" __attribute__((visibility("default"))) std::uint32_t
a9tas_barrel_yaw_tail_protocol_v1() {
  return protocol::kProtocolVersion;
}

extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_barrel_yaw_tail_shadow_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_shadow);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_barrel_yaw_tail_control_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_control);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_barrel_yaw_tail_target_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_targets);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_barrel_yaw_tail_audit_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(g_audits);
extern "C" __attribute__((visibility("default"))) std::uintptr_t
    a9tas_barrel_yaw_tail_evidence_storage_data_v1 =
        reinterpret_cast<std::uintptr_t>(&g_evidence);
