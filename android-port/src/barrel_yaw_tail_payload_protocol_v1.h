#pragma once

// Fixed-width ABI for the build-only BarrelYaw post-natural tail payload.
// The host preloads every frame target, then publishes one finite transaction
// only after the exact angular-setter auxiliary store has been observed.

#include <cstddef>
#include <cstdint>

namespace a9tas::barrel_yaw_tail_payload_v1 {

inline constexpr std::uint32_t kProtocolVersion = 2;
inline constexpr char kControlMagic[8] = {'A', '9', 'B', 'Y', 'T', 'C', '2', 0};
inline constexpr char kEvidenceMagic[8] = {'A', '9', 'B', 'Y', 'T', 'E', '2', 0};
inline constexpr std::size_t kVptrPrefixSize = 0x10;
// Exact-build PhysicsBackendBody address point and the two tail-call returns
// reached after the BarrelYaw setter.  These are RVAs, never process addresses.
inline constexpr std::uintptr_t kPhysicsBackendVptrRva = 0x9d5ed40;
inline constexpr std::uintptr_t kOriginalBoundaryCallbackRva = 0x4cc5aac;
inline constexpr std::uintptr_t kFirstBoundaryCallerReturnRva = 0x369e4a0;
inline constexpr std::uintptr_t kSecondBoundaryCallerReturnRva = 0x369e57c;
inline constexpr std::uintptr_t kNativeBodyPointerOffset = 0x90;
inline constexpr std::uintptr_t kNativeAngularOffset = 0x160;

// IDA proves that the exact PhysicsBackendBody table has callable entries from
// +0x0 through +0x1A8 and terminates before the next table at +0x1C0.  Copy the
// complete table, including its 0x10-byte Itanium address-point prefix.  The
// shadow is nevertheless transient: it is published only at the certified
// setter stop and restored after the second certified +0x68 return.
inline constexpr std::size_t kShadowSize = 0x1c0;
inline constexpr std::size_t kBoundarySlotOffset = 0x68;
inline constexpr std::size_t kLargestProvenVtableSlotOffset = 0x1a8;
inline constexpr std::uint32_t kMaximumFrames = 3600;
inline constexpr std::uint32_t kMaximumTransactions = 7200;
inline constexpr std::uint32_t kMaximumTransactionsPerFrame = 2;
inline constexpr std::uint64_t kDisarmedToken = 0;

inline constexpr std::uint64_t ArmToken(std::uint32_t generation,
                                        std::uint32_t arm_sequence) {
  return (static_cast<std::uint64_t>(generation) << 32) |
         static_cast<std::uint64_t>(arm_sequence);
}

enum ControlFlag : std::uint32_t {
  kControlConfigured = 1u << 0,
  kControlTargetsLoaded = 1u << 1,
};

enum Status : std::int32_t {
  kStatusPassive = 0,
  kStatusComplete = 1,
  kStatusRejectedBuildOnly = -100,
  kStatusInvalidControl = -201,
  kStatusUnexpectedObject = -202,
  kStatusUnexpectedVptr = -203,
  kStatusUnexpectedThread = -204,
  kStatusUnexpectedCallCount = -205,
  kStatusStaleToken = -206,
  kStatusImmediateAuditMismatch = -207,
  kStatusAuditOverflow = -208,
  kStatusUnexpectedCallerReturn = -209,
  kStatusVptrRestoreFailed = -210,
};

enum AuditFlag : std::uint32_t {
  kAuditFirstNaturalCallReturned = 1u << 0,
  kAuditSecondNaturalCallReturned = 1u << 1,
  kAuditOverridden = 1u << 2,
  kAuditSkipped = 1u << 3,
  kAuditImmediateExact = 1u << 4,
  kAuditTokenDisarmed = 1u << 5,
  kAuditVptrRestored = 1u << 6,
};

struct alignas(16) FrameTarget {
  std::uint32_t angular_bits[3];
  std::uint32_t skip_override_flags;
};

struct alignas(16) TransactionAudit {
  std::uint64_t token;
  std::uint32_t frame_index;
  std::uint32_t flags;
  std::uint32_t tid;
  std::uint32_t natural_calls;
  std::uint32_t before_bits[3];
  std::uint32_t immediate_bits[3];
  std::uint32_t captured_bits[3];
  std::uint32_t reserved;
};

struct alignas(64) Control {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t frame_count;
  std::uintptr_t expected_object;
  std::uintptr_t original_vptr;
  std::uintptr_t shadow_vptr;
  std::uintptr_t original_boundary_callback;
  std::uintptr_t native_angular;
  std::uint32_t expected_tid;
  std::uint32_t session_generation;
  std::uint32_t active_audit_index;
  std::uint32_t active_frame_index;
  std::uint8_t recording_sha256[32];
  std::uint64_t active_token;
  std::uintptr_t expected_first_caller_return;
  std::uintptr_t expected_second_caller_return;
};

struct alignas(64) Evidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint64_t wrapper_entries;
  std::uint64_t original_calls;
  std::uint64_t original_returns;
  std::uint64_t completed_transactions;
  std::uint64_t override_writes;
  std::uint64_t skipped_transactions;
  std::uint64_t failures;
  std::uint32_t active_call_count;
  std::uint32_t audit_count;
  std::int32_t last_status;
  std::uint32_t last_tid;
  std::uint64_t last_token;
  std::uintptr_t last_object;
  std::uintptr_t final_vptr;
  std::uint64_t reserved[5];
};

static_assert(kVptrPrefixSize + kBoundarySlotOffset + sizeof(void*) <=
                  kShadowSize,
               "BarrelYaw boundary slot lies inside shadow copy");
static_assert(kVptrPrefixSize + kLargestProvenVtableSlotOffset +
                      sizeof(void*) <=
                  kShadowSize,
              "BarrelYaw complete exact-build vtable lies inside shadow copy");
static_assert(kMaximumFrames * kMaximumTransactionsPerFrame ==
                  kMaximumTransactions,
              "BarrelYaw audit capacity matches the per-frame hard limit");
static_assert(sizeof(FrameTarget) == 16, "BarrelYaw target ABI");
static_assert(sizeof(TransactionAudit) == 64, "BarrelYaw audit ABI");
static_assert(sizeof(Control) == 192, "BarrelYaw control ABI");
static_assert(sizeof(Evidence) == 192, "BarrelYaw evidence ABI");
static_assert(offsetof(Control, active_token) == 112,
              "BarrelYaw publish-last token offset");

}  // namespace a9tas::barrel_yaw_tail_payload_v1
