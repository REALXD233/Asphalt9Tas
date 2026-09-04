#pragma once

#include "g3_boundary_adapter_v1.h"

#include <cstddef>
#include <cstdint>

namespace a9tas::g3_multi_hook_runtime_v1 {

inline constexpr std::uint32_t kVersion = 16;
inline constexpr std::uint32_t kHookCount = 3;
inline constexpr char kControlMagic[8] = {
    'A', '9', 'G', '3', 'C', '1', '\0', '\0',
};
inline constexpr char kEvidenceMagic[8] = {
    'A', '9', 'G', '3', 'E', '1', '\0', '\0',
};

enum class Command : std::uint32_t {
  kArmSession = 1,
  kRestore = 2,
  kInstallPassive = 3,
};

enum HookIndex : std::uint32_t {
  kTickHook = 0,
  kFinalWriterHook = 1,
  kFrameEventHook = 2,
};

enum Status : std::int32_t {
  kPassive = 0,
  kArmed = 1,
  kComplete = 2,
  kRestored = 3,
  kPassiveInstalled = 4,
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
  kErrorBeginVptrIdentity = 16,
  kErrorBeginTableRange = 17,
  kErrorBeginSlotIdentity = 18,
  kErrorBeginShadowPublish = 19,
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
  kNoGameplayWrites = 1ULL << 8,
  kNoExternalPerFrameStop = 1ULL << 9,
  kPayloadPreloaded = 1ULL << 10,
};

struct alignas(64) Control {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t enabled;
  std::uint32_t frame_limit;
  std::uint32_t generation;
  std::uint32_t completed;
  std::uint32_t active_helpers;
  std::uint32_t reserved0;
  std::uint64_t session_id;
  std::uint64_t expected_begin_owner;
  std::uint64_t expected_begin_owner_vptr;
  std::uint64_t expected_interval_owner;
  std::uint64_t expected_interval_owner_vptr;
  std::uint64_t expected_player;
  std::uint64_t expected_player_vptr;
  std::uint64_t lifecycle_state_address;
  std::uint64_t game_base;
  std::uint64_t target_entry[kHookCount];
  std::uint64_t target_tail[kHookCount];
  std::uint64_t wrapper[kHookCount];
  std::uint64_t begin_continuation;
  std::uint64_t begin_branch_destination;
  std::uint64_t reserved[6];
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
  std::uint64_t last_object[3];
  std::uint64_t last_vptr[3];
  std::uint32_t last_lifecycle_state;
  std::uint32_t reserved0;
  std::uint64_t reserved1[4];
};

static_assert(sizeof(Control) == 256, "G3 control ABI");
static_assert(sizeof(Evidence) == 320, "G3 evidence ABI");

}  // namespace a9tas::g3_multi_hook_runtime_v1
