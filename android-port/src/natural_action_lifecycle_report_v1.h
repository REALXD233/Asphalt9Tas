#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::natural_action_lifecycle_report_v1 {

enum Flag : std::uint32_t {
    kGameIdentityPinned = 1u << 0,
    kPayloadIdentityPinned = 1u << 1,
    kReadyNoAttach = 1u << 2,
    kSingleResumeObserved = 1u << 3,
    kOwnerThreadPinned = 1u << 4,
    kSingleSwapWritten = 1u << 5,
    kRegistrationAcknowledged = 1u << 6,
    kOriginalVptrAfterRegistration = 1u << 7,
    kRegisteredDetachClean = 1u << 8,
    kMailboxArmed = 1u << 9,
    kZeroCallReceipt = 1u << 10,
    kRemovalAcknowledged = 1u << 11,
    kDedicatedObjectAbsent = 1u << 12,
    kCallbackCountStable = 1u << 13,
    kOriginalVptrFinal = 1u << 14,
    kFinalDetachClean = 1u << 15,
    kProcessAliveTracerClear = 1u << 16,
};

constexpr std::uint32_t kRequiredFlags = 0x1FFFFu;

enum class CleanupDisposition : std::uint32_t {
    kNone = 0,
    kNaturalRemovalProved = 1,
    kForceStopRequired = 2,
    kExactProcessForceStopped = 3,
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
    std::uint32_t session_id;
    std::uint32_t expected_producer_tid;
    std::uint64_t vehicle_owner;
    std::uint32_t remove_requested;
    std::uint32_t reserved0;
    std::uint64_t reserved[1];
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
    std::uint64_t reserved[5];
};

#pragma pack(push, 1)
struct Report {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t flags;
    std::uint32_t final_phase;
    std::uint64_t pid;
    std::uint64_t process_generation;
    std::uint64_t game_library_base;
    std::uint64_t physics_context;
    std::uint64_t callback_list;
    std::uint64_t callback_flags;
    std::uint64_t car_physics_state;
    std::uint64_t original_vptr;
    std::uint64_t shadow_vptr;
    std::uint64_t payload_load_bias;
    std::uint64_t payload_shadow;
    std::uint64_t payload_control;
    std::uint64_t payload_evidence;
    std::uint64_t payload_mailbox;
    std::uint64_t dedicated_object;
    std::uint64_t dedicated_vptr;
    std::uint64_t bootstrap;
    std::uint64_t persistent_consumer;
    std::uint64_t fail_safe_callback;
    std::uint32_t session_id;
    std::int32_t owner_tid;
    std::uint32_t initial_threads;
    std::uint32_t final_threads;
    std::uint32_t cleanup_disposition;
    std::uint32_t reject_reasons;
    std::uint64_t ready_no_attach_ns;
    std::uint64_t resume_observed_ns;
    std::uint64_t registration_proved_ns;
    std::uint64_t mailbox_armed_ns;
    std::uint64_t zero_receipt_ns;
    std::uint64_t removal_requested_ns;
    std::uint64_t final_proof_ns;
    std::uint64_t game_write_attempts;
    std::uint64_t game_write_failures;
    std::uint64_t payload_write_attempts;
    std::uint64_t payload_write_failures;
    std::uint64_t ptrace_errors;
    std::uint64_t read_errors;
    std::uint64_t semantic_errors;
    std::uint64_t zero_call_frames;
    std::uint64_t rollback_attempts;
    std::uint64_t rollback_failures;
    std::uint64_t mailbox_claimed_sequence;
    std::uint64_t mailbox_completed_sequence;
    PayloadEvidence evidence;
    std::uint8_t payload_sha256[32];
    std::uint8_t payload_build_id[20];
    std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(PayloadControl) == 128, "lifecycle control ABI");
static_assert(sizeof(PayloadEvidence) == 256, "lifecycle evidence ABI");
static_assert(sizeof(Report) == 664, "lifecycle report ABI");
static_assert(offsetof(Report, pid) == 24, "report identity offset");
static_assert(offsetof(Report, payload_load_bias) == 96,
              "report payload offset");
static_assert(offsetof(Report, session_id) == 176,
              "report session offset");
static_assert(offsetof(Report, ready_no_attach_ns) == 200,
              "report timestamps offset");
static_assert(offsetof(Report, game_write_attempts) == 256,
              "report counters offset");
static_assert(offsetof(Report, evidence) == 352,
              "report evidence offset");
static_assert(offsetof(Report, payload_sha256) == 608,
              "report payload hash offset");
static_assert(offsetof(Report, reserved) == 660,
              "report reserved offset");

}  // namespace a9tas::natural_action_lifecycle_report_v1
