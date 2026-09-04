#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::fc3_replay_observer_v1 {

// FC-3 is a neutral observation gate.  It may resolve and sample the recovered
// replay identities, but it must never submit an action or mutate Nitro,
// fixed-delta, input, or vehicle physics state.
//
// The empty action queue and idle Nitro requirements below are deliberately
// test-only preconditions.  They are not the final replay semantics: the final
// TAS must submit recorded activations through the real game scheduler.  The
// inner-world +0x188 accumulator is only a phase witness; it must never be
// confused with the replay fixed-delta input (the Android MainTimeSource
// producer value corresponding to the PC p_in_delta_micros write).
enum Flag : std::uint32_t {
  kTargetVerified = 1u << 0,
  kIdentityPinned = 1u << 1,
  kPayloadPrepared = 1u << 2,
  kDedicatedHit = 1u << 3,
  kCallbackOpenAtDedicated = 1u << 4,
  kDirectModeZero = 1u << 5,
  kActionQueueEmpty = 1u << 6,
  kNitroIdle = 1u << 7,
  kCallbackCloseSeen = 1u << 8,
  kNextAccumulatorSeen = 1u << 9,
  kSameOwner = 1u << 10,
  kActionQueueUnchanged = 1u << 11,
  kNitroUnchanged = 1u << 12,
  kNextCallbackOpenSeen = 1u << 13,
  kDedicatedMembershipAbsent = 1u << 14,
  kCleanDetach = 1u << 15,
  kProcessAlive = 1u << 16,
  kTracerClear = 1u << 17,
};

constexpr std::uint32_t kRequiredFlags = 0x3FFFFu;

enum class Phase : std::uint32_t {
  kPreflight = 0,
  kWaitingDedicatedHit = 1,
  kWaitingCallbackClose = 2,
  kWaitingNextAccumulator = 3,
  kWaitingNextCallbackOpen = 4,
  kPhaseProved = 5,
  kDetached = 6,
  kRejected = 7,
};

enum RejectReason : std::uint32_t {
  kRejectWrongOwner = 1u << 0,
  kRejectWrongOrder = 1u << 1,
  kRejectCallbackState = 1u << 2,
  kRejectDirectMode = 1u << 3,
  kRejectActionQueueNotEmpty = 1u << 4,
  kRejectNitroNotIdle = 1u << 5,
  kRejectActionQueueWrite = 1u << 6,
  kRejectNitroMutation = 1u << 7,
  kRejectAccumulator = 1u << 8,
  kRejectPayloadEvidence = 1u << 9,
  kRejectDedicatedMembership = 1u << 10,
  kRejectIdentity = 1u << 11,
  kRejectThreadLifecycle = 1u << 12,
  kRejectTimeout = 1u << 13,
  kRejectRead = 1u << 14,
  kRejectPtrace = 1u << 15,
};

enum class EventKind : std::uint32_t {
  kDedicatedHit = 0,
  kCallbackClose = 1,
  kActionQueueWrite = 2,
  kNitroActiveWrite = 3,
  kNitroModeWrite = 4,
  kNextAccumulator = 5,
  kNextCallbackOpen = 6,
};

#pragma pack(push, 1)

// A fixed 64-byte read-only snapshot taken at one certified event.
struct Snapshot {
  std::uint64_t monotonic_ns;
  std::int32_t tid;
  std::uint16_t callback_flags;
  std::uint8_t direct_mode;
  std::uint8_t nitro_active;
  std::uint32_t nitro_mode;
  std::uint32_t accumulator_bits;
  std::uint64_t action_queue_begin;
  std::uint64_t action_queue_end;
  std::uint64_t action_queue_capacity;
  std::uint64_t action_queue_count;
  std::uint64_t dedicated_entries;
};

struct Event {
  EventKind kind;
  Snapshot snapshot;
  std::uint32_t dedicated_members_full;
  std::uint32_t payload_failures;
};

// The 512-byte report intentionally contains no replay input values.  FC-3
// proves only that the natural dedicated callback occupies the recovered
// post-result/pre-next-submit phase and that the observe-only window is clean.
struct Report {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t final_phase;
  std::uint64_t pid;
  std::uint64_t library_base;
  std::uint64_t physics_context;
  std::uint64_t callback_flags;
  std::uint64_t callback_list;
  std::uint64_t car_physics_state;
  std::uint64_t payload_evidence;
  std::uint64_t dedicated_object;
  std::uint64_t action_owner;
  std::uint64_t action_queue_end;
  std::uint64_t nitro_state;
  std::uint64_t nitro_active_address;
  std::uint64_t nitro_mode_address;
  std::uint64_t physics_backend_world;
  std::uint64_t phase_witness_accumulator;
  std::int32_t owner_tid;
  std::uint32_t initial_threads;
  std::uint32_t final_threads;
  std::uint32_t reject_reasons;
  Snapshot dedicated;
  Snapshot callback_close;
  Snapshot next_accumulator;
  Snapshot next_callback_open;
  std::uint64_t dedicated_hits;
  std::uint64_t callback_close_hits;
  std::uint64_t action_queue_writes;
  std::uint64_t nitro_writes;
  std::uint64_t accumulator_hits;
  std::uint64_t game_write_attempts;
  std::uint64_t read_errors;
  std::uint64_t ptrace_errors;
  std::uint64_t semantic_errors;
  std::uint32_t dedicated_members_final;
  std::uint32_t cleanup_disposition;
  std::uint64_t reserved[2];
};

#pragma pack(pop)

static_assert(sizeof(Snapshot) == 64, "FC-3 snapshot ABI");
static_assert(sizeof(Event) == 76, "FC-3 event ABI");
static_assert(sizeof(Report) == 512, "FC-3 report ABI");
static_assert(offsetof(Snapshot, monotonic_ns) == 0,
              "FC-3 snapshot timestamp offset");
static_assert(offsetof(Snapshot, tid) == 8,
              "FC-3 snapshot TID offset");
static_assert(offsetof(Snapshot, callback_flags) == 12,
              "FC-3 snapshot callback offset");
static_assert(offsetof(Snapshot, direct_mode) == 14,
              "FC-3 snapshot direct offset");
static_assert(offsetof(Snapshot, nitro_active) == 15,
              "FC-3 snapshot Nitro active offset");
static_assert(offsetof(Snapshot, nitro_mode) == 16,
              "FC-3 snapshot Nitro mode offset");
static_assert(offsetof(Snapshot, accumulator_bits) == 20,
              "FC-3 snapshot accumulator offset");
static_assert(offsetof(Snapshot, action_queue_begin) == 24,
              "FC-3 snapshot queue-begin offset");
static_assert(offsetof(Snapshot, action_queue_end) == 32,
              "FC-3 snapshot queue-end offset");
static_assert(offsetof(Snapshot, action_queue_capacity) == 40,
              "FC-3 snapshot queue-capacity offset");
static_assert(offsetof(Snapshot, action_queue_count) == 48,
              "FC-3 snapshot queue-count offset");
static_assert(offsetof(Snapshot, dedicated_entries) == 56,
              "FC-3 snapshot dedicated offset");
static_assert(offsetof(Report, pid) == 24,
              "FC-3 identity block offset");
static_assert(offsetof(Report, phase_witness_accumulator) == 136,
              "FC-3 identity block end");
static_assert(offsetof(Report, owner_tid) == 144,
              "FC-3 thread block offset");
static_assert(offsetof(Report, dedicated) == 160,
              "FC-3 snapshot block offset");
static_assert(offsetof(Report, callback_close) == 224,
              "FC-3 callback-close offset");
static_assert(offsetof(Report, next_accumulator) == 288,
              "FC-3 accumulator offset");
static_assert(offsetof(Report, next_callback_open) == 352,
              "FC-3 callback-open offset");
static_assert(offsetof(Report, dedicated_hits) == 416,
              "FC-3 counter block offset");
static_assert(offsetof(Report, dedicated_members_final) == 488,
              "FC-3 trailer offset");
static_assert(offsetof(Report, cleanup_disposition) == 492,
              "FC-3 cleanup offset");
static_assert(offsetof(Report, reserved) == 496,
              "FC-3 reserved trailer offset");

class StateMachine {
 public:
  explicit StateMachine(std::int32_t owner_tid);

  bool Consume(const Event& event, Report* report);
  bool rejected() const { return phase_ == Phase::kRejected; }
  bool phase_proved() const { return phase_ == Phase::kPhaseProved; }
  Phase phase() const { return phase_; }

 private:
  bool Reject(std::uint32_t reason, Report* report);
  bool QueueMatchesAnchor(const Snapshot& snapshot) const;
  bool NitroMatchesAnchor(const Snapshot& snapshot) const;

  Phase phase_ = Phase::kWaitingDedicatedHit;
  std::int32_t owner_tid_ = 0;
  Snapshot anchor_{};
};

}  // namespace a9tas::fc3_replay_observer_v1
