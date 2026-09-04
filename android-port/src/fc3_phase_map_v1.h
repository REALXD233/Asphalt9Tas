#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::fc3_phase_map_v1 {

enum class Phase : std::uint32_t {
  kPreflight = 0,
  kWaitingBootstrapClose = 1,
  kWaitingDelta = 2,
  kWaitingC98 = 3,
  kWaitingCallbackOpen = 4,
  kWaitingC9C = 5,
  kWaitingF64 = 6,
  kWaitingDedicated = 7,
  kWaitingCallbackClose = 8,
  kWaitingWorldCommit = 9,
  kWaitingNextDelta = 10,
  kWaitingNextC98 = 11,
  kProved = 12,
  kDetached = 13,
  kRejected = 14,
};

enum class EventKind : std::uint32_t {
  kCallbackFlags = 0,
  kDedicated = 1,
  kDelta = 2,
  kC98 = 3,
  kC9C = 4,
  kF64 = 5,
  kWorldCommit = 6,
};

enum class Decision : std::uint32_t {
  kContinue = 0,
  kRearmC9C = 1,
  kRearmF64 = 2,
  kRearmWorldCommit = 3,
  kRearmNextCycle = 4,
  kProved = 5,
  kReject = 6,
};

enum ReportFlags : std::uint32_t {
  kTargetVerified = 1u << 0,
  kIdentityPinned = 1u << 1,
  kWatchPlanArmed = 1u << 2,
  kBootstrapCloseSeen = 1u << 3,
  kDeltaSeen = 1u << 4,
  kC98Seen = 1u << 5,
  kCallbackOpenSeen = 1u << 6,
  kC9CSeen = 1u << 7,
  kF64Seen = 1u << 8,
  kDedicatedSeen = 1u << 9,
  kDedicatedAfterCar = 1u << 10,
  kCallbackCloseSeen = 1u << 11,
  kWorldCommitSeen = 1u << 12,
  kNextDeltaSeen = 1u << 13,
  kNextC98Seen = 1u << 14,
  kPhaseOrderProved = 1u << 15,
  kCleanDetach = 1u << 16,
  kProcessAlive = 1u << 17,
  kTracerClear = 1u << 18,
  kDeferredClearSeen = 1u << 19,
};

constexpr std::uint32_t kRequiredObservationFlags =
    kTargetVerified | kIdentityPinned | kWatchPlanArmed |
    kBootstrapCloseSeen | kDeltaSeen | kC98Seen | kCallbackOpenSeen |
    kC9CSeen | kF64Seen | kDedicatedSeen | kDedicatedAfterCar |
    kCallbackCloseSeen | kDeferredClearSeen | kWorldCommitSeen | kNextDeltaSeen |
    kNextC98Seen | kPhaseOrderProved;

constexpr std::uint32_t kRequiredFinalFlags =
    kRequiredObservationFlags | kCleanDetach | kProcessAlive | kTracerClear;

enum RejectReason : std::uint32_t {
  kRejectNullReport = 1u << 0,
  kRejectWrongOrder = 1u << 1,
  kRejectWrongCallbackValue = 1u << 2,
  kRejectWrongDedicatedThread = 1u << 3,
  kRejectCallbackListOrder = 1u << 4,
  kRejectDuplicateOrLateEvent = 1u << 5,
  kRejectWrongPhaseThread = 1u << 6,
  kRejectInvalidObservedValue = 1u << 7,
  kRejectMissingDeferredClear = 1u << 8,
};

#pragma pack(push, 1)
struct Report {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t reject_reasons;
  std::uint32_t final_phase;
  std::int32_t callback_owner_tid;
  std::uint64_t pid;
  std::uint64_t library_base;
  std::uint64_t main_time_source_owner;
  std::uint64_t final_control_owner;
  std::uint64_t fixed_delta_address;
  std::uint64_t c98_address;
  std::uint64_t c9c_address;
  std::uint64_t callback_flags_address;
  std::uint64_t callback_list_address;
  std::uint64_t car_physics_state;
  std::uint64_t dedicated_object;
  std::uint64_t f64_address;
  std::uint64_t world_commit_address;
  std::uint64_t bootstrap_close_ns;
  std::uint64_t delta_ns;
  std::uint64_t c98_ns;
  std::uint64_t callback_open_ns;
  std::uint64_t c9c_ns;
  std::uint64_t f64_ns;
  std::uint64_t dedicated_ns;
  std::uint64_t callback_close_ns;
  std::uint64_t world_commit_ns;
  std::uint64_t next_delta_ns;
  std::uint64_t next_c98_ns;
  std::int32_t delta_tid;
  std::int32_t c98_tid;
  std::int32_t c9c_tid;
  std::int32_t f64_tid;
  std::int32_t world_commit_tid;
  std::int32_t next_delta_tid;
  std::int32_t next_c98_tid;
  std::uint32_t car_callback_index;
  std::uint32_t dedicated_callback_index;
  std::uint64_t callback_flag_hits;
  std::uint64_t delta_hits;
  std::uint64_t c98_hits;
  std::uint64_t c9c_hits;
  std::uint64_t f64_hits;
  std::uint64_t dedicated_hits;
  std::uint64_t world_commit_hits;
  std::uint64_t read_errors;
  std::uint64_t ptrace_errors;
  std::uint64_t semantic_errors;
  std::uint64_t game_write_attempts;
  std::uint64_t action_calls;
  std::uint64_t nitro_calls;
  std::uint64_t physics_writes;
  std::uint32_t initial_threads;
  std::uint32_t final_threads;
  std::uint32_t cleanup_disposition;
  std::uint32_t f64_bits;
  std::uint32_t world_commit_bits;
  std::uint64_t pre_anchor_delta_hits;
  std::uint64_t pre_anchor_c98_hits;
};
#pragma pack(pop)

static_assert(sizeof(Report) == 408, "FC-3 phase-map report ABI");
static_assert(offsetof(Report, pid) == 32, "FC-3 phase-map pid offset");
static_assert(offsetof(Report, fixed_delta_address) == 64,
              "FC-3 phase-map fixed-delta offset");
static_assert(offsetof(Report, bootstrap_close_ns) == 136,
              "FC-3 phase-map timeline offset");
static_assert(offsetof(Report, delta_tid) == 224,
              "FC-3 phase-map event-tid offset");
static_assert(offsetof(Report, car_callback_index) == 252,
              "FC-3 phase-map callback-index offset");
static_assert(offsetof(Report, callback_flag_hits) == 260,
              "FC-3 phase-map counter offset");
static_assert(offsetof(Report, read_errors) == 316,
              "FC-3 phase-map error offset");
static_assert(offsetof(Report, initial_threads) == 372,
              "FC-3 phase-map thread offset");
static_assert(offsetof(Report, cleanup_disposition) == 380,
              "FC-3 phase-map cleanup offset");
static_assert(offsetof(Report, f64_bits) == 384,
              "FC-3 phase-map F64 value offset");
static_assert(offsetof(Report, world_commit_bits) == 388,
              "FC-3 phase-map world-commit value offset");
static_assert(offsetof(Report, pre_anchor_delta_hits) == 392,
              "FC-3 phase-map pre-anchor counter offset");

struct Event {
  EventKind kind;
  std::int32_t tid;
  std::uint64_t monotonic_ns;
  std::uint16_t callback_flags;
  std::uint16_t reserved16;
  std::uint32_t car_callback_index;
  std::uint32_t dedicated_callback_index;
  std::uint32_t observed_bits;
};

class Core {
 public:
  explicit Core(std::int32_t callback_owner_tid);

  Decision Consume(const Event& event, Report* report);
  bool proved() const { return phase_ == Phase::kProved; }
  bool rejected() const { return phase_ == Phase::kRejected; }
  Phase phase() const { return phase_; }

 private:
  Decision Reject(Report* report, std::uint32_t reason);

  std::int32_t callback_owner_tid_;
  bool post_close_deferred_clear_seen_ = false;
  Phase phase_ = Phase::kWaitingBootstrapClose;
};

}  // namespace a9tas::fc3_phase_map_v1
