#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::fc2_report_v1 {

enum Flag : std::uint32_t {
  kTargetVerified = 1u << 0,
  kIdentityPinned = 1u << 1,
  kPayloadPrepared = 1u << 2,
  kBootstrapOpenSeen = 1u << 3,
  kSingleSwapWritten = 1u << 4,
  kRegistrationAcknowledged = 1u << 5,
  kRegistrationNextOpenSeen = 1u << 6,
  kDedicatedMembershipPresent = 1u << 7,
  kDedicatedOwnerHit = 1u << 8,
  kRemovalAcknowledged = 1u << 9,
  kCompactionNextOpenSeen = 1u << 10,
  kDedicatedMembershipAbsent = 1u << 11,
  kOriginalVptrFinal = 1u << 12,
  kCleanDetach = 1u << 13,
  kProcessAlive = 1u << 14,
  kTracerClear = 1u << 15,
};

constexpr std::uint32_t kRequiredFlags = 0xFFFFu;

enum class Phase : std::uint32_t {
  kPreflight = 0,
  kWaitingBootstrapOpen = 1,
  kOthersFrozen = 2,
  kShadowInstalled = 3,
  kWaitingRegistrationProof = 4,
  kRegistrationProved = 5,
  kWaitingRemovalProof = 6,
  kRemovalProved = 7,
  kWaitingCompactionProof = 8,
  kCompactionProved = 9,
  kRollback = 10,
  kDetached = 11,
};

struct alignas(64) PayloadControl {
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

struct alignas(64) PayloadEvidence {
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

#pragma pack(push, 1)
struct Report {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t final_phase;
  std::uint64_t pid;
  std::uint64_t library_base;
  std::uint64_t physics_context;
  std::uint64_t callback_list;
  std::uint64_t callback_flags;
  std::uint64_t car_physics_state;
  std::uint64_t original_vptr;
  std::uint64_t shadow_vptr;
  std::uint64_t payload_shadow;
  std::uint64_t payload_control;
  std::uint64_t payload_evidence;
  std::uint64_t wrapper;
  std::uint64_t dedicated_observer;
  std::uint64_t dedicated_object;
  std::uint64_t dedicated_vptr;
  std::int32_t owner_tid;
  std::uint32_t initial_threads;
  std::uint32_t final_threads;
  std::uint32_t reject_reasons;
  std::uint64_t bootstrap_open_ns;
  std::uint64_t registration_proved_ns;
  std::uint64_t removal_requested_ns;
  std::uint64_t compaction_proved_ns;
  std::uint64_t game_write_attempts;
  std::uint64_t game_write_failures;
  std::uint64_t rollback_attempts;
  std::uint64_t rollback_failures;
  std::uint64_t read_errors;
  std::uint64_t ptrace_errors;
  std::uint64_t semantic_errors;
  std::uint32_t dedicated_members_before;
  std::uint32_t dedicated_members_after_registration;
  std::uint32_t dedicated_members_after_removal;
  std::uint32_t cleanup_disposition;
  PayloadEvidence evidence;
  std::uint64_t reserved1;
};
#pragma pack(pop)

static_assert(sizeof(PayloadControl) == 128, "FC-2 control ABI");
static_assert(sizeof(PayloadEvidence) == 256, "FC-2 evidence ABI");
static_assert(sizeof(Report) == 528, "FC-2 report ABI");
static_assert(offsetof(PayloadEvidence, bootstrap_entries) == 16,
              "FC-2 evidence counters offset");
static_assert(offsetof(PayloadEvidence, last_car) == 96,
              "FC-2 evidence identity offset");
static_assert(offsetof(PayloadEvidence, last_original_result) == 168,
              "FC-2 evidence result offset");
static_assert(offsetof(PayloadEvidence, list_begin_before) == 176,
              "FC-2 evidence list offset");
static_assert(offsetof(PayloadEvidence, registration_dispatching_before) == 240,
              "FC-2 evidence byte-state offset");
static_assert(offsetof(PayloadEvidence, last_status) == 248,
              "FC-2 evidence trailer offset");
static_assert(offsetof(Report, pid) == 24, "FC-2 identity block offset");
static_assert(offsetof(Report, dedicated_vptr) == 136,
              "FC-2 identity block end");
static_assert(offsetof(Report, owner_tid) == 144,
              "FC-2 thread block offset");
static_assert(offsetof(Report, bootstrap_open_ns) == 160,
              "FC-2 timestamp block offset");
static_assert(offsetof(Report, game_write_attempts) == 192,
              "FC-2 counter block offset");
static_assert(offsetof(Report, dedicated_members_before) == 248,
              "FC-2 membership block offset");
static_assert(offsetof(Report, cleanup_disposition) == 260,
              "FC-2 cleanup offset");
static_assert(offsetof(Report, evidence) == 264,
              "FC-2 report evidence offset");
static_assert(offsetof(Report, reserved1) == 520,
              "FC-2 reserved trailer offset");

}  // namespace a9tas::fc2_report_v1
