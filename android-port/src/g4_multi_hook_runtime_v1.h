#pragma once

#include "g4_g3_adapter_v1.h"

#include <cstddef>
#include <cstdint>

namespace a9tas::g4_multi_hook_runtime_v1 {

inline constexpr std::uint32_t kVersion = 14;
inline constexpr std::uint32_t kHookCount = 8;
inline constexpr std::uint32_t kRandomHookCount = 2;
inline constexpr std::uint32_t kSetterCount = 3;
// Brake and steering are installed on the live-proven adjusted downstream
// vtable.  Accelerator remains represented in the recording ABI but is not
// installed until an Android producer with AluTasV2's after-original member
// semantics is proven.
inline constexpr std::uint32_t kInstalledSetterCount = 2;
inline constexpr std::uint32_t kMaximumFrames =
    g4_g3_adapter_v1::kMaximumReceipts;
inline constexpr std::uint32_t kMaximumIntervalSamples = kMaximumFrames * 4;
inline constexpr std::uint32_t kMinimumReplaySpeedFactor = 1;
inline constexpr std::uint32_t kMaximumReplaySpeedFactor = 8;
// Existing ABI-tail field values for an AluTasV2-style synchronous replay
// completion barrier.
inline constexpr std::uint32_t kReplayCompletionBarrierDisabled = 0;
inline constexpr std::uint32_t kReplayCompletionBarrierEnabled = 1;
// Branch replay is different from an inspection pause.  At the final
// FrameAfter boundary the payload converts the already-observed prefix into a
// recording and switches to Record before returning to game code.  Host-side
// pause latency can therefore add recorded suffix ticks, but can never create
// an unrecorded simulation gap.
inline constexpr std::uint32_t kReplayCompletionAtomicRecord = 2;
// Record checkpoints use the same ABI-tail word, but only while mode=Record.
// The host publishes Requested while the race is running.  The dispatcher
// changes it to Held only after one complete authoritative Tick has closed;
// the host can then queue the pause and seal without dropping a partial Tick.
inline constexpr std::uint32_t kRecordCheckpointBarrierRequested = 2;
inline constexpr std::uint32_t kRecordCheckpointBarrierHeld = 3;
inline constexpr std::uintptr_t kMainVtableRva = 0x7F3C1A8;
inline constexpr std::uintptr_t kFixedDeltaOffset = 0x150;
inline constexpr std::uintptr_t kControlPairOffset = 0xC98;
inline constexpr std::uintptr_t kBackendInterfaceOffset = 0x30;
inline constexpr std::uintptr_t kNativeBodySlotOffset = 0x90;
inline constexpr std::uintptr_t kNativePoseOffset = 0x10;
inline constexpr std::uintptr_t kNativeLinearOffset = 0x150;
inline constexpr std::uintptr_t kNativePhysicsBodyVtableRva = 0x9EDC6A0;
inline constexpr std::uintptr_t kNitroServiceOffset = 0xCB8;
inline constexpr std::uintptr_t kNitroServiceVtableRva = 0x7EE8A90;
inline constexpr std::uintptr_t kNitroDispatchSlot = 0x108;
inline constexpr std::uintptr_t kNitroDispatchThunkRva = 0x3674E50;
inline constexpr std::uintptr_t kNitroStateEntryRva = 0x36D8524;
inline constexpr std::uintptr_t kBarrelRollEntryRva = 0x369DE4C;
// AluTasV2-compatible BarrelYaw stabilization tail.  This is not a gameplay
// 360 detector: the exact Android caller reaches it only while the stunt state
// at owner+0x1D80 is active and the backend contact query returns zero.
inline constexpr std::uintptr_t kBarrelYawEntryRva = 0x369E30C;
inline constexpr std::uintptr_t kBarrelRbxOffset = 0x1968;
inline constexpr std::uintptr_t kBarrelBackendOffset = 0x18;
inline constexpr std::uintptr_t kBarrelBackendNativeBodyOffset = 0x90;
inline constexpr std::uintptr_t kVehicleSourceVtableRva = 0x7EF2F48;
inline constexpr std::uintptr_t kCarPhysicsBodySourceVtableRva = 0x7EECD88;
inline constexpr std::uintptr_t kPhysicsImplementationVtableRva = 0x7EECD88;
inline constexpr std::uintptr_t kAdjustedSetterVtableRva = 0x7EED9E0;
inline constexpr std::uintptr_t kAdjustedSetterObjectOffset = 0x2A80;
inline constexpr std::intptr_t kAdjustedSetterThisAdjustment = -0x2A80;
inline constexpr std::uintptr_t
    kSetterSlotOffsets[kInstalledSetterCount] = {
        0x2A8, 0x2B0,
};
inline constexpr std::uintptr_t
    kSetterOriginalRvas[kInstalledSetterCount] = {
        0x36934B8, 0x36934E0,
};

inline constexpr char kControlMagic[8] = {
    'A', '9', 'G', '4', 'C', '1', '0', '\0',
};
inline constexpr char kEvidenceMagic[8] = {
    'A', '9', 'G', '4', 'E', '1', '0', '\0',
};

enum class RunMode : std::uint32_t {
  kNeutral = 0,
  kRecord = 1,
  kReplay = 2,
};

enum class CompletionPolicy : std::uint32_t {
  kFixedFrameLimit = 0,
  kRaceLifecycle = 1,
};

enum class CompletionReason : std::uint32_t {
  kNone = 0,
  kFixedFrameLimit = 1,
  kRaceLifecycle = 2,
  kManualCheckpoint = 3,
};

inline constexpr bool ArmFrameLimitValid(
    std::uint32_t frame_limit, RunMode mode, CompletionPolicy completion,
    bool archived_rearm) noexcept {
  if (frame_limit == 0 || frame_limit > kMaximumFrames) return false;
  if (completion == CompletionPolicy::kRaceLifecycle)
    return mode == RunMode::kRecord;
  if (completion != CompletionPolicy::kFixedFrameLimit) return false;
  if (archived_rearm) return mode == RunMode::kReplay;
  // Product replay uses the exact sealed recording length.  The historical
  // 5/60/900 values are bounded development gates, not replay semantics.
  if (mode == RunMode::kReplay) return true;
  return frame_limit == 5 || frame_limit == 60 || frame_limit == 900;
}

enum class Command : std::uint32_t {
  kArmSession = 1,
  kRestore = 2,
  kInstallPassive = 3,
  kRearmArchivedReplay = 4,
  kRearmArchivedRecord = 5,
  kSealPausedRecord = 6,
  kRearmPausedReplayRecord = 7,
  kQueueArchivedReplay = 8,
  kQueueArchivedRecord = 9,
  // Pre-arms the next record generation while the current recording remains
  // active.  Direct Retry can therefore discard the current attempt and start
  // the next one without a host-side countdown scan.
  kQueueActiveRecordRetry = 10,
  // Cancels a queued next-race transition without restoring the resident
  // hooks.  This is used when the user changes their mind while waiting at
  // Retry (for example, switching from a blank recording to replaying a
  // selected prefix).
  kCancelPending = 11,
};

enum HookIndex : std::uint32_t {
  kTickHook = 0,
  kFinalWriterHook = 1,
  kFrameEventHook = 2,
  kNitroHook = 3,
  kSubmitHook = 4,
  kBarrelRollHook = 5,
  kBarrelYawHook = 6,
  // Complete game-side logic dispatcher.  Fast replay repeats this natural
  // call; it never enlarges the per-tick fixed physics interval.
  kLogicDispatcherHook = 7,
};

enum RandomHookIndex : std::uint32_t {
  kBarrelRandomBoolHook = 0,
  kBarrelRandomLerpHook = 1,
};

enum SetterIndex : std::uint32_t {
  kBrakeSetter = 0,
  kSteeringSetter = 1,
  kAcceleratorSetter = 2,
};

enum Status : std::int32_t {
  kPassive = 0,
  kArmed = 1,
  kComplete = 2,
  kRestored = 3,
  kPassiveInstalled = 4,
  // The current race is complete and the retained lifecycle hook owns a
  // request for the next real 2 -> 3 race transition.
  kQueued = 5,
  kFault = -1,
};

enum Error : std::uint32_t {
  kErrorNone = 0,
  kErrorCommand = 1,
  kErrorControl = 2,
  kErrorGameIdentity = 3,
  kErrorLifecycleIdentity = 4,
  kErrorObjectIdentity = 5,
  kErrorTargetRange = 6,
  kErrorPrologue = 7,
  kErrorProtection = 8,
  kErrorPatchReadback = 9,
  kErrorRestoreReadback = 10,
  kErrorAlreadyInstalled = 11,
  kErrorNotInstalled = 12,
  kErrorAdapter = 13,
  kErrorRecursive = 14,
  kErrorEarlyInstallTimeout = 15,
  kErrorMainObjectIdentity = 16,
  kErrorControlPair = 17,
  kErrorFixedDelta = 18,
  kErrorIntervalStream = 19,
  kErrorNitroIdentity = 20,
  kErrorNitroCall = 21,
  kErrorRecordingCapacity = 22,
  kErrorSetterIdentity = 23,
  kErrorSetterValue = 24,
  kErrorSetterPublish = 25,
  kErrorSetterRestore = 26,
  kErrorPhysicsIdentity = 27,
  kErrorPhysicsCapture = 28,
  kErrorPhysicsCorrection = 29,
  kErrorBarrelIdentity = 30,
  kErrorBarrelTail = 31,
  kErrorBarrelRandom = 32,
};

enum EvidenceFlag : std::uint64_t {
  kGameIdentity = 1ULL << 0,
  kLifecycleIdentity = 1ULL << 1,
  kObjectIdentity = 1ULL << 2,
  kAllPrologues = 1ULL << 3,
  kAllPatchesPublished = 1ULL << 4,
  kAllPatchesReadBack = 1ULL << 5,
  kAllLogicalRx = 1ULL << 6,
  kAllRestored = 1ULL << 7,
  kNoExternalPerFrameStop = 1ULL << 8,
  kPayloadPreloaded = 1ULL << 9,
  kG3CoordinatorReused = 1ULL << 10,
  kControlPairBound = 1ULL << 11,
  kFixedDeltaBound = 1ULL << 12,
  kNitroServiceBound = 1ULL << 13,
  kExactIntervalStream = 1ULL << 14,
  kSetterVtableBound = 1ULL << 15,
  kAllSetterSlotsPublished = 1ULL << 16,
  kAllSetterSlotsRestored = 1ULL << 17,
  kPhysicsBackendBound = 1ULL << 18,
  kBarrelTailBound = 1ULL << 19,
  kDeterministicBarrelPrngBound = 1ULL << 20,
  kReplayRecordHandoffComplete = 1ULL << 21,
};

inline constexpr char kRecordingBundleMagic[8] = {
    'A', '9', 'G', '4', 'R', '2', '\0', '\0',
};
inline constexpr std::uint32_t kLegacyRecordingBundleVersion = 2;
inline constexpr std::uint32_t kRecordingBundleVersion = 3;
// Explicit opt-in format: absent interval groups represent complete logical
// updates with no integration. Existing v2/v3 files remain dense.
inline constexpr std::uint32_t kSparseRecordingBundleVersion = 4;
inline constexpr std::uint32_t kPhaseRecordingBundleVersion = 5;
inline constexpr std::uint32_t kPhaseRecordingBundleFlags = 0x7f;
enum RecordingBundleFlag : std::uint32_t {
  kBundleAdjustedBrakeSteering = 1u << 0,
  kBundleNaturalNitro = 1u << 1,
  kBundleExactIntervalStream = 1u << 2,
  kBundlePhysicsValid = 1u << 3,
  kBundleBarrelStabilization = 1u << 4,
  kBundleZeroIntegrationUpdates = 1u << 5,
};
inline constexpr std::uint32_t kRecordingBundleFlags =
    kBundleAdjustedBrakeSteering | kBundleNaturalNitro |
    kBundleExactIntervalStream | kBundlePhysicsValid |
    kBundleBarrelStabilization;
inline constexpr std::uint32_t kLegacyRecordingBundleFlags =
    kBundleAdjustedBrakeSteering | kBundleNaturalNitro |
    kBundleExactIntervalStream | kBundlePhysicsValid;
inline constexpr std::uint32_t kSparseRecordingBundleFlags =
    kRecordingBundleFlags | kBundleZeroIntegrationUpdates;

// Replay diagnostics reuse the already allocated record-mode frame buffer.
// During replay each qualified Final Writer stores the natural after-original
// 64+12 bytes there before any conditional correction.  The host exports only
// those bytes and their exact source-bound correction class; this is evidence,
// not a second gameplay recording or a camera track.
inline constexpr char kReplayPhysicsDiagnosticMagic[8] = {
    'A', '9', 'G', '5', 'D', '1', '\0', '\0',
};
inline constexpr std::uint32_t kReplayPhysicsDiagnosticVersion = 1;
enum ReplayPhysicsDiagnosticFlag : std::uint32_t {
  kDiagnosticNaturalEqual = 1u << 0,
  kDiagnosticCorrected = 1u << 1,
};

#pragma pack(push, 1)
struct RecordingBundleHeaderV1 {
  char magic[8];
  std::uint32_t version;
  std::uint32_t header_size;
  std::uint32_t frame_size;
  std::uint32_t interval_size;
  std::uint32_t frame_count;
  std::uint32_t interval_count;
  std::uint32_t fixed_delta_us;
  std::uint32_t flags;
  std::uint64_t session_id;
  std::uint32_t generation;
  std::uint32_t reserved0;
  std::uint8_t reserved[8];
};

struct ReplayPhysicsDiagnosticHeaderV1 {
  char magic[8];
  std::uint32_t version;
  std::uint32_t header_size;
  std::uint32_t record_size;
  std::uint32_t frame_count;
  std::uint32_t fixed_delta_us;
  std::uint32_t flags;
  std::uint64_t session_id;
  std::uint32_t generation;
  std::uint32_t reserved0;
  std::uint8_t recording_sha256[32];
  std::uint8_t reserved[16];
};

struct ReplayPhysicsDiagnosticRecordV1 {
  std::uint64_t tick;
  std::uint32_t flags;
  std::uint32_t reserved0;
  std::uint8_t natural_transform[unified_tick_v1::kTransformSize];
  std::uint8_t natural_linear[unified_tick_v1::kLinearVelocitySize];
  std::uint8_t reserved[4];
};
#pragma pack(pop)
static_assert(sizeof(RecordingBundleHeaderV1) == 64,
              "G4 recording bundle header ABI");
static_assert(sizeof(ReplayPhysicsDiagnosticHeaderV1) == 96,
              "G5 replay diagnostic header ABI");
static_assert(sizeof(ReplayPhysicsDiagnosticRecordV1) == 96,
              "G5 replay diagnostic record ABI");

struct alignas(64) Control {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t mode;
  std::uint32_t enabled;
  std::uint32_t frame_limit;
  std::uint32_t generation;
  std::uint32_t completed;
  std::uint32_t active_helpers;
  std::uint32_t fixed_delta_us;
  std::uint32_t replay_frame_count;
  std::uint32_t replay_interval_count;
  std::uint32_t completion_policy;
  std::uint64_t session_id;
  std::uint64_t expected_begin_owner;
  std::uint64_t expected_begin_owner_vptr;
  std::uint64_t expected_interval_owner;
  std::uint64_t expected_interval_owner_vptr;
  std::uint64_t expected_player;
  std::uint64_t expected_player_vptr;
  std::uint64_t lifecycle_state_address;
  std::uint64_t expected_main_object;
  std::uint64_t expected_main_object_vptr;
  std::uint64_t game_base;
  std::uint64_t target_entry[kHookCount];
  std::uint64_t target_tail[kHookCount];
  std::uint64_t wrapper[kHookCount];
  std::uint8_t recording_sha256[32];
  std::uint64_t expected_nitro_state;
  std::uint64_t setter_slot[kSetterCount];
  std::uint64_t setter_original[kSetterCount];
  std::uint64_t setter_wrapper[kSetterCount];
  std::uint64_t expected_setter_object;
  std::uint64_t expected_setter_vptr;
  std::uint64_t expected_backend_interface;
  std::uint64_t expected_physics_velocity_interface;
  std::uint64_t expected_native_body;
  std::uint64_t expected_barrel_owner;
  std::uint64_t expected_barrel_owner_vptr;
  // Nonzero only while rearming an archived lifecycle recording as a replay.
  // The payload checks these against the retained completed race before it
  // clears any receipt. They remain in the same 24-byte ABI tail.
  std::uint64_t archived_generation;
  std::uint64_t archived_frame_count;
  std::uint64_t archived_interval_count;
  std::uint32_t replay_speed_factor;
  std::uint32_t replay_speed_reserved;
  // The eight established replay hooks remain unchanged.  This ninth,
  // lifecycle-only entry is kept outside their indexed tables so it cannot
  // perturb the proven per-tick ABI.  It turns Retry into a game-owned event
  // instead of a host-side multi-gigabyte heap scan.
  std::uint64_t lifecycle_target_entry;
  std::uint64_t lifecycle_target_tail;
  std::uint64_t lifecycle_wrapper;
  std::uint32_t pending_mode;
  std::uint32_t pending_state;
  std::uint32_t pending_generation;
  std::uint32_t pending_reserved;
  // v14: v5 archive's exact pre-first-Submit phase; 0 absent, 1 available.
  std::uint32_t initial_phase_bits[2];
  std::uint32_t initial_phase_present;
  std::uint32_t initial_phase_reserved;
  // Optional extension in existing alignment padding. Zero is legacy 1x.
  // Retained during replay for the atomic replay->record transition, but only
  // applied in Record mode; never multiplies the replay speed.
  std::uint32_t record_slowmo_divisor;
};

struct alignas(64) Evidence {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::int32_t status;
  std::uint32_t first_error;
  std::uint64_t flags;
  std::uint64_t install_calls;
  std::uint64_t restore_calls;
  std::uint64_t wrapper_entries[kHookCount];
  std::uint64_t qualified_events[kHookCount];
  std::uint64_t ignored_events;
  std::uint64_t core_faults;
  std::uint64_t recursive_entries;
  std::uint64_t published_ticks;
  std::uint64_t first_tid[kHookCount];
  std::uint64_t last_tid[kHookCount];
  std::uint64_t last_tick;
  std::uint64_t last_caller_return;
  std::uint64_t last_object[kHookCount];
  std::uint64_t last_vptr[kHookCount];
  std::uint32_t last_lifecycle_state;
  std::uint32_t completion_reason;
  std::uint64_t fixed_delta_writes;
  std::uint64_t control_pair_reads;
  std::uint64_t control_pair_writes;
  std::uint64_t interval_calls;
  std::uint64_t interval_overrides;
  std::uint64_t interval_records;
  std::uint64_t natural_nitro_calls;
  std::uint64_t suppressed_nitro_calls;
  std::uint64_t injected_nitro_calls;
  std::uint64_t recorded_frames;
  std::int32_t last_g3_result;
  std::int32_t last_g4_result;
  std::uint64_t setter_entries[kSetterCount];
  std::uint64_t setter_qualified_events[kSetterCount];
  std::uint64_t setter_overrides[kSetterCount];
  std::uint64_t setter_last_object[kSetterCount];
  std::uint32_t setter_last_bits[kSetterCount];
  std::uint32_t setter_cache_bits[kSetterCount];
  std::uint64_t physics_equal_frames;
  std::uint64_t physics_corrected_frames;
  std::uint64_t physics_correction_writes;
  std::uint64_t physics_skipped_frames;
  std::uint64_t barrel_rbx_overrides;
  std::uint64_t barrel_angular_overrides;
  std::uint64_t barrel_rbx_records;
  std::uint64_t barrel_angular_records;
  std::uint64_t reserved[1];
  std::uint64_t barrel_random_bool_calls;
  std::uint64_t barrel_random_lerp_calls;
  std::uint64_t barrel_random_resets;
  std::uint64_t lifecycle_entries;
  std::uint64_t lifecycle_qualified;
  std::uint64_t pending_activations;
};

static_assert(sizeof(Control) == 640, "G4 phase-aware control ABI");
inline constexpr bool ValidRecordSlowmo(std::uint32_t divisor) {
  return divisor == 0 || divisor == 1 || divisor == 2 || divisor == 4 || divisor == 8 || divisor == 75 || divisor == 90;
}
inline constexpr std::uint32_t RecordSlowmoDivisor(std::uint32_t mode,
                                                 std::uint32_t divisor) {
  return mode == static_cast<std::uint32_t>(RunMode::kRecord) &&
      ValidRecordSlowmo(divisor) && divisor != 0 ? (divisor == 90 ? 10u : divisor == 75 ? 4u : divisor) : 1u;
}
// Legacy values are divisors; 75/90 encode 3/4 and 9/10 speeds.
inline constexpr std::uint32_t RecordSlowmoNumerator(std::uint32_t mode,
                                                   std::uint32_t code) {
  return mode == static_cast<std::uint32_t>(RunMode::kRecord) ? (code == 90 ? 9u : code == 75 ? 3u : 1u) : 1u;
}
static_assert(offsetof(Control, record_slowmo_divisor) == 592,
              "Slowmo extends control padding without moving existing fields");
static_assert(sizeof(Evidence) == 832,
              "G4 deterministic barrel PRNG evidence ABI");
static_assert(offsetof(Control, target_entry) == 144,
              "G4 target table ABI");
static_assert(offsetof(Control, recording_sha256) == 336,
              "G4 recording identity ABI");
static_assert(offsetof(Control, expected_nitro_state) == 368,
              "G4 Nitro state identity ABI");
static_assert(offsetof(Control, setter_slot) == 376,
              "G4 setter slot table ABI");
static_assert(offsetof(Control, setter_original) == 400,
              "G4 setter original table ABI");
static_assert(offsetof(Control, setter_wrapper) == 424,
              "G4 setter wrapper table ABI");
static_assert(offsetof(Control, expected_setter_object) == 448,
              "G4 adjusted setter object ABI");
static_assert(offsetof(Control, expected_setter_vptr) == 456,
              "G4 adjusted setter vptr ABI");
static_assert(offsetof(Control, expected_backend_interface) == 464,
              "G5 backend interface ABI");
static_assert(offsetof(Control, expected_physics_velocity_interface) == 472,
              "G5 velocity interface ABI");
static_assert(offsetof(Control, expected_native_body) == 480,
              "G5 native body ABI");
static_assert(offsetof(Control, expected_barrel_owner) == 488,
              "G6 barrel owner ABI");
static_assert(offsetof(Control, expected_barrel_owner_vptr) == 496,
              "G6 barrel owner vptr ABI");
static_assert(offsetof(Control, replay_speed_factor) == 528,
              "G4 fast replay factor ABI");
static_assert(offsetof(Control, lifecycle_target_entry) == 536,
              "G4 resident lifecycle entry ABI");
static_assert(offsetof(Control, pending_mode) == 560,
              "G4 pending race mode ABI");

}  // namespace a9tas::g4_multi_hook_runtime_v1
