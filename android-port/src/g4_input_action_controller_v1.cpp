// Host-side transaction controller for the Android five-hook G4 Gate.
// It reuses the live-proven G2 full-process freeze and NativeBridge guest-call
// machinery. Physics submit owns BeginTick and the source-faithful fixed-delta
// write; Physics Interval observes natural substeps. Final Writer and Tick End
// close the same logical tick. All five hooks are session-scoped.

#define main a9tas_g2_controller_embedded_main_v1
#include "g2_physics_interval_controller_v1.cpp"
#undef main

#include "g4_multi_hook_runtime_v1.h"
#include "g8_runtime_build_profile_v1.h"
#include "race_lifecycle_object_resolver_v1.h"
#include "status_observation_policy_v1.h"
#include "vehicle_state_resolver_v1.h"
#if defined(A9TAS_G4_NATIVE_ARM64_CONTROLLER)
#include "native_arm64_g4_payload_resolver_v1.h"
#include "native_arm64_immutable_trap_resolver_v1.h"
#endif

#include <cinttypes>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <limits>

namespace g4_controller_v1 {

namespace g2 = g2_controller_v1;
namespace protocol = a9tas::g4_multi_hook_runtime_v1;
namespace bridge = a9tas::g4_g3_adapter_v1;
namespace g4 = a9tas::g4_input_action_v1;
namespace adapter = a9tas::g3_boundary_adapter_v1;
namespace coordinator = a9tas::g3_tick_coordinator_v1;
namespace recording = a9tas::unified_tick_v1;
namespace lifecycle = a9tas::race_lifecycle_v1;
namespace vehicle = a9tas::vehicle_state_v1;
namespace build_profile = a9tas::g8_runtime_build_profile_v1;

constexpr char kAcknowledgement[] = "I_ACCEPT_G4_TICK_COORDINATOR_V1";

bool SafeCancellationSignalPath(const char* path) {
  if (path == nullptr)
    return false;
  const std::size_t length = std::strlen(path);
  if (length == 0 || length > 511 || std::strstr(path, "..") != nullptr ||
      std::strstr(path, "//") != nullptr)
    return false;
  for (std::size_t index = 0; index < length; ++index) {
    const char value = path[index];
    const bool safe = (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9') || value == '/' || value == '.' ||
        value == '_' || value == '-';
    if (!safe)
      return false;
  }

  constexpr char kUserPrefix[] = "/data/user/";
  constexpr char kDataPrefix[] = "/data/data/";
  constexpr char kPackageNamespace[] = "dev.a9tas.android";
  constexpr char kCacheSuffix[] = "/cache/a9tas-cancel-";
  const char* package = nullptr;
  const char* token = nullptr;
  if (std::strncmp(path, kUserPrefix, sizeof(kUserPrefix) - 1) == 0) {
    const char* cursor = path + sizeof(kUserPrefix) - 1;
    if (*cursor < '0' || *cursor > '9')
      return false;
    while (*cursor >= '0' && *cursor <= '9')
      ++cursor;
    if (*cursor++ != '/')
      return false;
    package = cursor;
  } else if (std::strncmp(path, kDataPrefix, sizeof(kDataPrefix) - 1) == 0) {
    package = path + sizeof(kDataPrefix) - 1;
  } else {
    return false;
  }
  const char* suffix = std::strstr(package, kCacheSuffix);
  if (suffix == nullptr ||
      std::strncmp(package, kPackageNamespace,
                   sizeof(kPackageNamespace) - 1) != 0)
    return false;
  const char* package_tail = package + sizeof(kPackageNamespace) - 1;
  // Accept the production application id and its build-variant suffixes, but
  // do not let an arbitrary package use this root-side cancellation channel.
  if (package_tail != suffix && *package_tail != '.')
    return false;
  for (const char* cursor = package_tail; cursor < suffix; ++cursor) {
    const char value = *cursor;
    if (!((value >= 'a' && value <= 'z') ||
          (value >= 'A' && value <= 'Z') ||
          (value >= '0' && value <= '9') || value == '.' || value == '_'))
      return false;
  }
  token = suffix + sizeof(kCacheSuffix) - 1;
  if (token == nullptr || *token == '\0')
    return false;
  constexpr char kExtension[] = ".signal";
  const std::size_t token_length = std::strlen(token);
  if (token_length <= sizeof(kExtension) - 1 ||
      std::strcmp(token + token_length - (sizeof(kExtension) - 1),
                  kExtension) != 0)
    return false;
  for (std::size_t index = 0;
       index < token_length - (sizeof(kExtension) - 1); ++index) {
    const char value = token[index];
    if (!((value >= '0' && value <= '9') ||
          (value >= 'a' && value <= 'f') || value == '-'))
      return false;
  }
  return true;
}
#if !defined(A9TAS_G4_NATIVE_ARM64_CONTROLLER)
constexpr char kBootstrapLocatorSymbol[] =
    "a9tas_bootstrap_g4_tick_coordinator_locator_v1";
constexpr char kBootstrapTrapSymbol[] =
    "a9tas_bootstrap_g4_tick_coordinator_return_trap_v1";
constexpr char kBootstrapCalibrateSymbol[] =
    "a9tas_bootstrap_g4_tick_coordinator_calibrate_tid_v1";
constexpr char kCommandSymbol[] = "a9tas_g4_command_v1";
constexpr char kControlLocatorSymbol[] = "a9tas_g4_control_data_v1";
constexpr char kEvidenceLocatorSymbol[] = "a9tas_g4_evidence_data_v1";
constexpr char kRuntimeLocatorSymbol[] = "a9tas_g4_runtime_data_v1";
constexpr char kRecordedFramesLocatorSymbol[] =
    "a9tas_g4_recorded_frames_data_v1";
constexpr char kRecordedIntervalsLocatorSymbol[] =
    "a9tas_g4_recorded_intervals_data_v1";
constexpr char kReplayFramesLocatorSymbol[] =
    "a9tas_g4_replay_frames_data_v1";
constexpr char kReplayIntervalsLocatorSymbol[] =
    "a9tas_g4_replay_intervals_data_v1";
constexpr char kBuildProfileLocatorSymbol[] =
    "a9tas_g4_build_profile_data_v1";
constexpr std::uint64_t kLocatorMagic = 0x47345443314c4f43ULL;
#endif
constexpr char kBuildProfilePath[] =
    "/data/local/tmp/a9tas_g8_runtime_build_profile_v1.bin";
constexpr std::int64_t kStepOptionsThisAdjustment = -0x2A78;
constexpr std::uint32_t kArmTag = 0x47344901;
constexpr std::uint32_t kRestoreTag = 0x47344902;
constexpr std::uint32_t kInstallTag = 0x47344903;
constexpr std::uint32_t kRearmTag = 0x47344904;
constexpr std::uintptr_t kEmbeddedTimeSourceOffset = 0xB0;
constexpr std::uintptr_t kTimeScaleManagerOffset = 0xF8;
constexpr std::uintptr_t kEnabledOffset = 0x158;
constexpr std::uintptr_t kPhaseSchedulerOffset = 0x178;
constexpr std::uintptr_t kPhysicsContextOffset = 0x1A98;
constexpr std::uintptr_t kTimeScaleValueOffset = 0x2D8;
constexpr std::size_t kValidatedMainObjectSize = 0x1C50;
constexpr std::size_t kValidatedIntervalOwnerSize =
    protocol::kControlPairOffset + sizeof(std::uint64_t);

constexpr std::uint8_t kExpectedPrologues[protocol::kHookCount][16] = {
    {0xff,0xc3,0x00,0xd1,0xf5,0x53,0x01,0xa9,
     0xf3,0x7b,0x02,0xa9,0x35,0x11,0x91,0x52},
    {0xec,0x0f,0x17,0xfc,0xeb,0x2b,0x01,0x6d,
     0xe9,0x23,0x02,0x6d,0xfc,0x6f,0x03,0xa9},
    {0xff,0xc3,0x00,0xd1,0xf4,0x0b,0x00,0xf9,
     0xf3,0x7b,0x02,0xa9,0x28,0x00,0x40,0xf9},
    {0xf5,0x53,0xbe,0xa9,0xf3,0x7b,0x01,0xa9,
     0x08,0xe4,0x46,0x39,0xc8,0x00,0x00,0x34},
    {0xff,0xc3,0x00,0xd1,0xf4,0x0b,0x00,0xf9,
     0xf3,0x7b,0x02,0xa9,0x08,0x20,0x47,0x39},
    {0xff,0xc3,0x02,0xd1,0xee,0x2b,0x00,0xfd,
     0xed,0xb3,0x05,0x6d,0xeb,0xab,0x06,0x6d},
    {0xff,0x03,0x05,0xd1,0xef,0x3b,0x0c,0x6d,
     0xed,0x33,0x0d,0x6d,0xeb,0x2b,0x0e,0x6d},
    {0xff,0x43,0x01,0xd1,0xf7,0x5b,0x02,0xa9,
     0xf5,0x53,0x03,0xa9,0xf3,0x7b,0x04,0xa9},
};

constexpr std::uint8_t kExpectedLifecyclePrologue[16] = {
    0xff,0x83,0x03,0xd1,0xf7,0x5b,0x0b,0xa9,
    0xf5,0x53,0x0c,0xa9,0xf3,0x7b,0x0d,0xa9,
};

constexpr std::uint8_t kExpectedRandomOriginalPrefix[protocol::kRandomHookCount][12] = {
    {0xfe,0x0f,0x1f,0xf8,0x08,0xfc,0xe7,0xd2,0xc6,0x19,0x00,0x94},
    {0xfe,0x0f,0x1f,0xf8,0x28,0x00,0x40,0xf9,0xb7,0x19,0x00,0x94},
};

constexpr std::uint8_t
    kExpectedSetterBodies[protocol::kInstalledSetterCount][28] = {
        {0x08,0x00,0x40,0xf9,0x29,0x00,0x40,0xb9,
         0x08,0x61,0x0b,0xd1,0x08,0x01,0x40,0xf9,
         0x08,0x00,0x08,0x8b,0x09,0x99,0x0c,0xb9,
         0xc0,0x03,0x5f,0xd6},
        {0x08,0x00,0x40,0xf9,0x29,0x00,0x40,0xb9,
         0x08,0x81,0x0b,0xd1,0x08,0x01,0x40,0xf9,
         0x08,0x00,0x08,0x8b,0x09,0x9d,0x0c,0xb9,
         0xc0,0x03,0x5f,0xd6},
};

enum class Action : std::uint32_t {
  kArm = 1,
  kStatus = 2,
  kRestore = 3,
  kDiagnose = 4,
  kPassiveStatus = 5,
  kInstallPassive = 6,
  kArmRecord = 7,
  kDumpRecord = 8,
  kArmReplay = 9,
  kDumpReplayDiagnostic = 10,
  kArmLifecycleRecord = 11,
  kWaitForCompletion = 12,
  kRearmArchivedReplay = 13,
  kPauseProbe = 14,
  kWaitForReplayProgress = 15,
  kWaitForCompletionAndPause = 16,
  kRearmArchivedRecord = 17,
  kSealPausedRecord = 18,
  kRearmPausedReplayRecord = 19,
  kWaitForCompletionAndStop = 20,
  kResumeStoppedAtPauseMenu = 21,
  kWaitForRetryCountdownAndPause = 22,
  // Waits on the payload-owned exact replay completion barrier, queues one
  // pause key, then rearms the retained runtime for suffix recording before
  // returning to Java.  This removes the timing-sensitive second controller
  // launch from the normal replay -> record branch handoff.
  kWaitForCompletionPauseAndRearm = 23,
  // Arms only payload-owned pending state.  No lifecycle/object scan and no
  // countdown pause are performed; the resident race-enter hook binds the
  // next race at the game's real 2 -> 3 transition.
  kQueueArchivedRecord = 24,
  kQueueArchivedReplay = 25,
  // Waits only on the resident lifecycle hook's pending receipt.  Unlike the
  // legacy Retry waiter this never scans process mappings and never injects
  // ESC; the real race-enter callback owns the activation boundary.
  kWaitForPendingActivation = 26,
  kQueueActiveRecordRetry = 27,
  kCancelPending = 28,
  // Requests an exact record boundary, waits until the game-side dispatcher
  // holds it, queues one ESC, then seals the already-closed Tick.
  kCheckpointAtNextClosedTick = 29,
};

constexpr bool ReplayProgressCursorValid(
    std::uint32_t receipts, std::uint64_t tick, std::size_t replay_head,
    coordinator::TickPhase phase, bool active_packet,
    bool input_tick_open) noexcept {
  if (tick != receipts) return false;
  if (phase == coordinator::TickPhase::kClosed)
    return replay_head == receipts && !active_packet && !input_tick_open;
  return receipts != std::numeric_limits<std::uint32_t>::max() &&
         replay_head == static_cast<std::size_t>(receipts) + 1u &&
         active_packet && input_tick_open;
}

// A host pause can freeze either a closed authoritative tick or the next tick
// after Begin/Interval but before FinalWriter/FrameEnd.  In the latter shape
// receipt_count/tick still identify the last complete packet while ticks_begun
// is exactly one ahead.  The payload resolves that open tail while every game
// thread remains stopped.
constexpr bool PausedRecordCursorResolvable(
    std::uint32_t receipts, std::uint64_t tick, std::uint64_t ticks_begun,
    coordinator::TickPhase phase, bool input_tick_open) noexcept {
  if (tick != receipts) return false;
  if (input_tick_open)
    return phase != coordinator::TickPhase::kClosed &&
           ticks_begun == static_cast<std::uint64_t>(receipts) + 1u;
  return phase == coordinator::TickPhase::kClosed && ticks_begun == receipts;
}

static_assert(ReplayProgressCursorValid(
    120, 120, 120, coordinator::TickPhase::kClosed, false, false));
static_assert(ReplayProgressCursorValid(
    120, 120, 121, coordinator::TickPhase::kBegun, true, true));
static_assert(!ReplayProgressCursorValid(
    120, 120, 122, coordinator::TickPhase::kBegun, true, true));
static_assert(!ReplayProgressCursorValid(
    120, 120, 121, coordinator::TickPhase::kClosed, true, true));
static_assert(PausedRecordCursorResolvable(
    528, 528, 528, coordinator::TickPhase::kClosed, false));
static_assert(PausedRecordCursorResolvable(
    528, 528, 529, coordinator::TickPhase::kBegun, true));
static_assert(!PausedRecordCursorResolvable(
    528, 528, 529, coordinator::TickPhase::kClosed, true));
static_assert(!PausedRecordCursorResolvable(
    528, 528, 530, coordinator::TickPhase::kBegun, true));

constexpr bool IsArmAction(Action action) noexcept {
  return action == Action::kArm || action == Action::kArmRecord ||
         action == Action::kArmReplay ||
         action == Action::kArmLifecycleRecord;
}

constexpr bool IsRearmAction(Action action) noexcept {
  return action == Action::kRearmArchivedReplay ||
          action == Action::kRearmArchivedRecord ||
         action == Action::kRearmPausedReplayRecord ||
         action == Action::kWaitForCompletionPauseAndRearm ||
         action == Action::kQueueArchivedRecord ||
         action == Action::kQueueArchivedReplay;
}

constexpr bool IsSessionConfigurationAction(Action action) noexcept {
  return IsArmAction(action) || IsRearmAction(action);
}

struct Runtime {
  g2::Runtime call{};
  std::uintptr_t runtime{};
  std::uintptr_t recorded_frames{};
  std::uintptr_t recorded_intervals{};
  std::uintptr_t replay_frames{};
  std::uintptr_t replay_intervals{};
  std::uintptr_t build_profile{};
  std::uintptr_t begin_owner{};
  std::uintptr_t begin_owner_vptr{};
  std::uintptr_t interval_owner{};
  std::uintptr_t interval_owner_vptr{};
  std::uintptr_t player{};
  std::uintptr_t player_vptr{};
  std::uintptr_t lifecycle_object{};
  std::uintptr_t lifecycle_state{};
  std::uintptr_t main_object{};
  std::uintptr_t main_object_vptr{};
  std::uintptr_t nitro_state{};
  std::uintptr_t setter_object{};
  std::uintptr_t setter_object_vptr{};
  std::uintptr_t backend_interface{};
  std::uintptr_t physics_velocity_interface{};
  std::uintptr_t native_body{};
  std::uintptr_t barrel_owner{};
  std::uintptr_t barrel_owner_vptr{};
};

#pragma pack(push, 1)
struct BuildProfileAnnexHeaderV1 {
  char magic[8];
  std::uint32_t version;
  std::uint32_t header_size;
  std::uint32_t vehicle_rva_count;
  std::uint32_t lifecycle_vtable_count;
  std::uint32_t total_size;
  std::uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(BuildProfileAnnexHeaderV1) == 32);
static_assert(sizeof(vehicle::Profile) == 42 * sizeof(std::uintptr_t));

struct BuildProfileBundleV1 {
  build_profile::Profile core{};
  vehicle::Profile vehicle{};
  std::vector<std::uintptr_t> lifecycle_vtables;
};

BuildProfileBundleV1 g_build_profile{};

bool BytesZero(const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint8_t combined = 0;
  for (std::size_t index = 0; index < size; ++index) combined |= bytes[index];
  return combined == 0;
}

std::string HexBytes(const std::uint8_t* data, std::size_t size) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string output(size * 2, '0');
  for (std::size_t index = 0; index < size; ++index) {
    output[index * 2] = kHex[data[index] >> 4];
    output[index * 2 + 1] = kHex[data[index] & 0xf];
  }
  return output;
}

bool ReadBuildProfileBundle(const char* path, BuildProfileBundleV1* output) {
  if (path == nullptr || output == nullptr) return false;
  struct stat before{};
  if (lstat(path, &before) != 0 || !S_ISREG(before.st_mode) ||
      S_ISLNK(before.st_mode) || before.st_size <= 0 ||
      before.st_size > 1024 * 1024)
    return false;
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat opened{};
  bool ok = fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode) &&
            opened.st_dev == before.st_dev && opened.st_ino == before.st_ino &&
            opened.st_size == before.st_size;
  std::vector<std::uint8_t> bytes;
  if (ok) {
    bytes.resize(static_cast<std::size_t>(opened.st_size));
    std::size_t done = 0;
    while (done < bytes.size()) {
      const ssize_t count = read(fd, bytes.data() + done, bytes.size() - done);
      if (count <= 0) { ok = false; break; }
      done += static_cast<std::size_t>(count);
    }
  }
  struct stat after{};
  ok = ok && fstat(fd, &after) == 0 && after.st_dev == opened.st_dev &&
       after.st_ino == opened.st_ino && after.st_size == opened.st_size;
  ok = close(fd) == 0 && ok;
  if (!ok || bytes.size() < sizeof(build_profile::Profile) +
                              sizeof(BuildProfileAnnexHeaderV1))
    return false;

  BuildProfileBundleV1 result{};
  std::memcpy(&result.core, bytes.data(), sizeof(result.core));
  if (!build_profile::Valid(result.core)) return false;
  BuildProfileAnnexHeaderV1 annex{};
  std::memcpy(&annex, bytes.data() + sizeof(result.core), sizeof(annex));
  constexpr char kAnnexMagic[8] = {'A','9','B','P','A','X','1','\0'};
  if (std::memcmp(annex.magic, kAnnexMagic, sizeof(kAnnexMagic)) != 0 ||
      annex.version != 1 || annex.header_size != sizeof(annex) ||
      annex.vehicle_rva_count != 42 ||
      annex.lifecycle_vtable_count == 0 ||
      annex.lifecycle_vtable_count > 4096 || annex.reserved != 0 ||
      annex.total_size != bytes.size())
    return false;
  const std::size_t expected_size = sizeof(result.core) + sizeof(annex) +
      sizeof(std::uintptr_t) *
          (annex.vehicle_rva_count + annex.lifecycle_vtable_count);
  if (expected_size != bytes.size()) return false;
  const std::uint8_t* cursor =
      bytes.data() + sizeof(result.core) + sizeof(annex);
  std::memcpy(&result.vehicle, cursor, sizeof(result.vehicle));
  cursor += sizeof(result.vehicle);
  result.lifecycle_vtables.resize(annex.lifecycle_vtable_count);
  std::memcpy(result.lifecycle_vtables.data(), cursor,
              result.lifecycle_vtables.size() * sizeof(std::uintptr_t));
  const auto rva_valid = [&](std::uintptr_t rva) {
    return rva >= 0x1000 && rva < result.core.image_size && (rva & 3u) == 0;
  };
  const auto* vehicle_rvas =
      reinterpret_cast<const std::uintptr_t*>(&result.vehicle);
  for (std::size_t index = 0; index < 42; ++index)
    if (!rva_valid(vehicle_rvas[index])) return false;
  for (std::size_t index = 0; index < result.lifecycle_vtables.size(); ++index)
    if (!rva_valid(result.lifecycle_vtables[index]) ||
        (index != 0 && result.lifecycle_vtables[index] <=
                           result.lifecycle_vtables[index - 1]))
      return false;
  *output = std::move(result);
  return true;
}

lifecycle::Profile LifecycleProfile() {
  return {
      g_build_profile.core.image_size,
      g_build_profile.core.lifecycle_phase_gate_rva,
      g_build_profile.core.lifecycle_shared_enter_rva,
      g_build_profile.core.lifecycle_derived_enter_rva,
      g_build_profile.core.lifecycle_racing_store_rva,
      g_build_profile.lifecycle_vtables.data(),
      g_build_profile.lifecycle_vtables.size(),
      0,
      0,
  };
}

bool ParseAction(const char* text, Action* action) {
  if (text == nullptr || action == nullptr) return false;
  if (std::strcmp(text, "arm") == 0) *action = Action::kArm;
  else if (std::strcmp(text, "record") == 0)
    *action = Action::kArmRecord;
  else if (std::strcmp(text, "record-life") == 0)
    *action = Action::kArmLifecycleRecord;
  else if (std::strcmp(text, "wait") == 0)
    *action = Action::kWaitForCompletion;
  else if (std::strcmp(text, "rearm-replay") == 0)
    *action = Action::kRearmArchivedReplay;
  else if (std::strcmp(text, "rearm-record") == 0)
    *action = Action::kRearmArchivedRecord;
  else if (std::strcmp(text, "queue-record") == 0)
    *action = Action::kQueueArchivedRecord;
  else if (std::strcmp(text, "queue-replay") == 0)
    *action = Action::kQueueArchivedReplay;
  else if (std::strcmp(text, "wait-activation") == 0)
    *action = Action::kWaitForPendingActivation;
  else if (std::strcmp(text, "queue-active-record") == 0)
    *action = Action::kQueueActiveRecordRetry;
  else if (std::strcmp(text, "cancel-pending") == 0)
    *action = Action::kCancelPending;
  else if (std::strcmp(text, "checkpoint-pause") == 0)
    *action = Action::kCheckpointAtNextClosedTick;
  else if (std::strcmp(text, "seal-record") == 0)
    *action = Action::kSealPausedRecord;
  else if (std::strcmp(text, "rearm-branch") == 0)
    *action = Action::kRearmPausedReplayRecord;
  else if (std::strcmp(text, "pause-probe") == 0)
    *action = Action::kPauseProbe;
  else if (std::strcmp(text, "wait-progress") == 0)
    *action = Action::kWaitForReplayProgress;
  else if (std::strcmp(text, "wait-pause") == 0)
    *action = Action::kWaitForCompletionAndPause;
  else if (std::strcmp(text, "wait-pause-rearm") == 0)
    *action = Action::kWaitForCompletionPauseAndRearm;
  else if (std::strcmp(text, "wait-stop") == 0)
    *action = Action::kWaitForCompletionAndStop;
  else if (std::strcmp(text, "resume-stop") == 0)
    *action = Action::kResumeStoppedAtPauseMenu;
  else if (std::strcmp(text, "wait-retry-pause") == 0)
    *action = Action::kWaitForRetryCountdownAndPause;
  else if (std::strcmp(text, "dump") == 0)
    *action = Action::kDumpRecord;
  else if (std::strcmp(text, "replay") == 0)
    *action = Action::kArmReplay;
  else if (std::strcmp(text, "diff") == 0)
    *action = Action::kDumpReplayDiagnostic;
  else if (std::strcmp(text, "status") == 0) *action = Action::kStatus;
  else if (std::strcmp(text, "restore") == 0) *action = Action::kRestore;
  else if (std::strcmp(text, "diagnose") == 0) *action = Action::kDiagnose;
  else if (std::strcmp(text, "passive") == 0)
    *action = Action::kPassiveStatus;
  else if (std::strcmp(text, "install") == 0)
    *action = Action::kInstallPassive;
  else return false;
  return true;
}

struct ReplayBundleV1 {
  protocol::RecordingBundleHeaderV1 header{};
  std::vector<recording::RecordingFrameV1> frames;
  std::vector<g4::IntervalSampleV1> intervals;
  std::array<std::uint8_t, 32> sha256{};
};

bool ReadReplayBundle(const char* path, std::uint32_t expected_frames,
                      ReplayBundleV1* bundle) {
  if (path == nullptr || bundle == nullptr || expected_frames == 0)
    return false;
  struct stat before{};
  if (lstat(path, &before) != 0 || !S_ISREG(before.st_mode) ||
      S_ISLNK(before.st_mode) || before.st_size <= 0)
    return false;
  const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat opened{};
  bool ok = fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode) &&
            opened.st_dev == before.st_dev && opened.st_ino == before.st_ino &&
            opened.st_size == before.st_size;
  const std::size_t maximum_size =
      sizeof(protocol::RecordingBundleHeaderV1) +
      sizeof(recording::RecordingFrameV1) * protocol::kMaximumFrames +
      sizeof(g4::IntervalSampleV1) * protocol::kMaximumIntervalSamples;
  ok = ok && opened.st_size > 0 &&
       static_cast<std::uint64_t>(opened.st_size) <= maximum_size;
  std::vector<std::uint8_t> bytes;
  if (ok) {
    bytes.resize(static_cast<std::size_t>(opened.st_size));
    std::size_t done = 0;
    while (done < bytes.size()) {
      const ssize_t count = read(fd, bytes.data() + done, bytes.size() - done);
      if (count <= 0) {
        ok = false;
        break;
      }
      done += static_cast<std::size_t>(count);
    }
  }
  struct stat after{};
  ok = ok && fstat(fd, &after) == 0 && after.st_dev == opened.st_dev &&
       after.st_ino == opened.st_ino && after.st_size == opened.st_size;
  ok = close(fd) == 0 && ok;
  if (!ok || bytes.size() < sizeof(protocol::RecordingBundleHeaderV1))
    return false;

  ReplayBundleV1 output{};
  std::memcpy(&output.header, bytes.data(), sizeof(output.header));
  const auto& header = output.header;
  const protocol::RecordingBundleHeaderV1 zero{};
  const bool legacy_bundle =
      header.version == protocol::kLegacyRecordingBundleVersion &&
      header.flags == protocol::kLegacyRecordingBundleFlags;
  const bool current_bundle =
      header.version == protocol::kRecordingBundleVersion &&
      header.flags == protocol::kRecordingBundleFlags;
  const bool sparse_bundle =
      header.version == protocol::kSparseRecordingBundleVersion &&
      header.flags == protocol::kSparseRecordingBundleFlags &&
      (header.fixed_delta_us == 8333 || header.fixed_delta_us == 6944);
  if (std::memcmp(header.magic, protocol::kRecordingBundleMagic,
                  sizeof(header.magic)) != 0 ||
      (!legacy_bundle && !current_bundle && !sparse_bundle) ||
      header.header_size != sizeof(header) ||
      header.frame_size != sizeof(recording::RecordingFrameV1) ||
      header.interval_size != sizeof(g4::IntervalSampleV1) ||
      header.frame_count != expected_frames ||
      header.frame_count == 0 || header.frame_count > protocol::kMaximumFrames ||
      (header.interval_count == 0 && !sparse_bundle) ||
      header.interval_count > protocol::kMaximumIntervalSamples ||
      header.fixed_delta_us < recording::kMinimumFixedIntervalUs ||
      header.fixed_delta_us > recording::kMaximumFixedIntervalUs ||
      header.session_id == 0 || header.generation == 0 ||
      header.reserved0 != 0 ||
      std::memcmp(header.reserved, zero.reserved, sizeof(header.reserved)) != 0)
    return false;
  const std::size_t expected_size =
      sizeof(header) + sizeof(recording::RecordingFrameV1) * header.frame_count +
      sizeof(g4::IntervalSampleV1) * header.interval_count;
  if (bytes.size() != expected_size) return false;

  output.frames.resize(header.frame_count);
  output.intervals.resize(header.interval_count);
  std::size_t offset = sizeof(header);
  std::memcpy(output.frames.data(), bytes.data() + offset,
              output.frames.size() * sizeof(output.frames[0]));
  offset += output.frames.size() * sizeof(output.frames[0]);
  if (!output.intervals.empty())
    std::memcpy(output.intervals.data(), bytes.data() + offset,
                output.intervals.size() * sizeof(output.intervals[0]));
  for (std::uint32_t index = 0; index < header.frame_count; ++index) {
    const auto& frame = output.frames[index];
    if (!g4::PhysicsRecordingFrameValid(frame) || frame.tick != index ||
        frame.skip_override_flags !=
            (legacy_bundle ? g4::kLegacyPhysicsRecordSkipFlags
                           : g4::kPhysicsRecordSkipFlags) ||
        frame.monotonic_ns !=
            static_cast<std::uint64_t>(index) * header.fixed_delta_us * 1000ULL)
      return false;
  }
  if (!g4::IntervalSequenceValid(
          {output.intervals.data(), output.intervals.size()},
          header.frame_count, sparse_bundle)) return false;
  Sha256 digest;
  digest.Update(bytes.data(), bytes.size());
  output.sha256 = digest.Finish();
  bool nonzero = false;
  for (std::uint8_t value : output.sha256) nonzero = nonzero || value != 0;
  if (!nonzero) return false;
  *bundle = std::move(output);
  return true;
}

bool RecordingHashIsZero(const protocol::Control& control) {
  for (std::uint8_t value : control.recording_sha256)
    if (value != 0) return false;
  return true;
}

bool RecordingHashIsNonzero(const protocol::Control& control) {
  return !RecordingHashIsZero(control);
}

bool ArchivedFieldsZero(const protocol::Control& control) {
  return control.archived_generation == 0 &&
         control.archived_frame_count == 0 &&
         control.archived_interval_count == 0;
}

bool ArchivedReplayFieldsValid(const protocol::Control& control,
                               std::uint32_t limit) {
  return control.mode ==
             static_cast<std::uint32_t>(protocol::RunMode::kReplay) &&
         limit != 0 &&
         control.archived_generation != 0 &&
         control.archived_generation < UINT32_MAX &&
         control.generation == control.archived_generation + 1u &&
         control.archived_frame_count != 0 &&
         control.archived_frame_count <= protocol::kMaximumFrames &&
         (control.archived_interval_count != 0 ||
          control.fixed_delta_us == 8333 || control.fixed_delta_us == 6944) &&
         control.archived_interval_count <= protocol::kMaximumIntervalSamples;
}

bool ArchivedRecordFieldsValid(const protocol::Control& control,
                               std::uint32_t limit) {
  return control.mode ==
             static_cast<std::uint32_t>(protocol::RunMode::kRecord) &&
         control.completion_policy == static_cast<std::uint32_t>(
             protocol::CompletionPolicy::kRaceLifecycle) &&
         control.archived_generation != 0 &&
         control.archived_generation < UINT32_MAX &&
         control.generation == control.archived_generation + 1u &&
         control.archived_frame_count != 0 &&
         control.archived_frame_count <= limit &&
         (control.archived_interval_count != 0 ||
          control.fixed_delta_us == 8333 || control.fixed_delta_us == 6944) &&
         control.archived_interval_count <= protocol::kMaximumIntervalSamples;
}

#if !defined(A9TAS_G4_NATIVE_ARM64_CONTROLLER)
bool ResolvePayloadLocator(const ElfImage& elf, pid_t pid,
                           std::uintptr_t base, const char* symbol,
                           std::uintptr_t* output) {
  return g2::ResolveLocatorAddress(elf, pid, base, symbol, output);
}
#endif

bool ResolveArtifacts(pid_t pid, std::uint64_t start_ticks,
                      std::uintptr_t requested_game_base, Runtime* runtime,
                      bool status_only) {
  if (runtime == nullptr || !IsAlive(pid, start_ticks) ||
      TracerPid(pid) != 0 || requested_game_base == 0)
    return false;
#if defined(A9TAS_G4_NATIVE_ARM64_CONTROLLER)
  runtime->call.maps = ReadMaps(pid);
  if (runtime->call.maps.empty()) return false;
  char mem_path[64]{};
  std::snprintf(mem_path,sizeof(mem_path),"/proc/%d/mem",pid);
  const int payload_mem=open(mem_path,O_RDONLY|O_CLOEXEC);
  if (payload_mem<0) return false;
  a9tas::native_arm64_g4_payload_resolver_v1::Layout layout{};
  const char* resolver_failure="none";
  const bool layout_ok=a9tas::native_arm64_g4_payload_resolver_v1::Resolve(
      pid,payload_mem,&layout,&resolver_failure);
  const bool mem_closed=close(payload_mem)==0;
  a9tas::native_arm64_immutable_trap_resolver_v1::Report trap{};
  if (!layout_ok || !mem_closed) {
    std::fprintf(stderr,
                 "G4_NATIVE_PAYLOAD_RESOLVER passed=0 reason=%s "
                 "mem_closed=%u\n",
                 resolver_failure,
                 mem_closed ? 1u : 0u);
    return false;
  }
  // Some Android 7 vendor images expose no reusable BRK instruction in an
  // immutable executable mapping.  The shared ARM64 remote-call layer also
  // supports LR=0 and accepts only the exact SIGSEGV/SEGV_MAPERR return at
  // PC=0, so absence of a BRK is not an artifact-resolution failure.
  if (!status_only &&
      !a9tas::native_arm64_immutable_trap_resolver_v1::Resolve(pid,&trap))
    trap={};
  runtime->call.payload_base=layout.load_bias;
  runtime->call.control=layout.control;
  runtime->call.evidence=layout.evidence;
  runtime->call.native_command=layout.command;
  runtime->call.native_trap=trap.address;
  runtime->runtime=layout.runtime;
  runtime->recorded_frames=layout.recorded_frames;
  runtime->recorded_intervals=layout.recorded_intervals;
  runtime->replay_frames=layout.replay_frames;
  runtime->replay_intervals=layout.replay_intervals;
  runtime->build_profile=layout.build_profile;
#else
  FileImage payload_file{}, bootstrap_file{};
  if (!ReadPinnedRegularFile(kPayloadPath, kPayloadSha256, &payload_file) ||
      !ReadPinnedRegularFile(kBootstrapPath, kBootstrapSha256,
                             &bootstrap_file))
    return false;
  ElfImage payload_elf{}, bootstrap_elf{};
  if (!payload_elf.Parse(payload_file, EM_AARCH64) ||
      payload_elf.build_id() != kPayloadBuildId ||
      !bootstrap_elf.Parse(bootstrap_file, EM_X86_64) ||
      bootstrap_elf.build_id() != kBootstrapBuildId)
    return false;

  runtime->call.maps = ReadMaps(pid);
  if (runtime->call.maps.empty() ||
      !UniqueOffsetZeroBase(runtime->call.maps, kPayloadPath,
                            payload_file.status,
                            &runtime->call.payload_base) ||
      !UniqueOffsetZeroBase(runtime->call.maps, kBootstrapPath,
                            bootstrap_file.status,
                            &runtime->call.bootstrap_base))
    return false;

  Elf64_Sym locator_symbol{}, trap_symbol{}, calibrate_symbol{};
  Elf64_Sym stage_symbol{}, probe_symbol{}, trampoline_symbol{};
  if (!bootstrap_elf.ResolveUnique(kBootstrapLocatorSymbol, true,
                                   &locator_symbol) ||
      !bootstrap_elf.ResolveUnique(kBootstrapTrapSymbol, true,
                                   &trap_symbol) ||
      !bootstrap_elf.ResolveUnique(kBootstrapCalibrateSymbol, true,
                                   &calibrate_symbol) ||
      !bootstrap_elf.ResolveUnique(kStageSymbol, false, &stage_symbol) ||
      !bootstrap_elf.ResolveUnique(kProbeStatusSymbol, false, &probe_symbol) ||
      !bootstrap_elf.ResolveUnique(kTrampolineSymbol, false,
                                   &trampoline_symbol) ||
      !ReadStableLocator(pid, runtime->call.bootstrap_base +
                                  locator_symbol.st_value,
                         &runtime->call.bootstrap) ||
      runtime->call.bootstrap.magic != kLocatorMagic ||
      runtime->call.bootstrap.version != 1 ||
      runtime->call.bootstrap.size != sizeof(Locator) ||
      !ExactString(runtime->call.bootstrap.payload_path,
                   sizeof(runtime->call.bootstrap.payload_path),
                   kPayloadPath) ||
      !ExactString(runtime->call.bootstrap.payload_sha256,
                   sizeof(runtime->call.bootstrap.payload_sha256),
                   kPayloadSha256) ||
      !ExactString(runtime->call.bootstrap.payload_build_id,
                   sizeof(runtime->call.bootstrap.payload_build_id),
                   kPayloadBuildId) ||
      !ExactString(runtime->call.bootstrap.payload_source_sha256,
                   sizeof(runtime->call.bootstrap.payload_source_sha256),
                   kPayloadSourceSha256) ||
      !ExactString(runtime->call.bootstrap.run_symbol,
                   sizeof(runtime->call.bootstrap.run_symbol),
                   kCommandSymbol) ||
      !ExactString(runtime->call.bootstrap.shorty,
                   sizeof(runtime->call.bootstrap.shorty), "JJ") ||
      runtime->call.bootstrap.stage_address !=
          runtime->call.bootstrap_base + stage_symbol.st_value ||
      runtime->call.bootstrap.probe_status_address !=
          runtime->call.bootstrap_base + probe_symbol.st_value ||
      runtime->call.bootstrap.trampoline_address !=
          runtime->call.bootstrap_base + trampoline_symbol.st_value ||
      runtime->call.bootstrap.stage_size != 4 ||
      runtime->call.bootstrap.probe_status_size != 4 ||
      runtime->call.bootstrap.trampoline_size != 8 ||
      runtime->call.bootstrap.reserved != 0 ||
      runtime->call.bootstrap.return_trap_address !=
          runtime->call.bootstrap_base + trap_symbol.st_value ||
      runtime->call.bootstrap.calibrate_tid_address !=
          runtime->call.bootstrap_base + calibrate_symbol.st_value)
    return false;

  int stage = 0, probe_status = 0;
  if (!ReadProcessValue(pid, runtime->call.bootstrap.stage_address, &stage) ||
      stage != 5 ||
      !ReadProcessValue(pid, runtime->call.bootstrap.probe_status_address,
                        &probe_status) ||
      probe_status != 1 ||
      !ReadProcessValue(pid, runtime->call.bootstrap.trampoline_address,
                        &runtime->call.guest_trampoline) ||
      runtime->call.guest_trampoline == 0)
    return false;
  const Mapping* guest_map = FindMapping(runtime->call.maps,
                                         runtime->call.guest_trampoline, 1);
  const Mapping* trap_map = FindMapping(
      runtime->call.maps, runtime->call.bootstrap.return_trap_address, 2);
  if (guest_map == nullptr || !guest_map->readable || !guest_map->executable ||
      guest_map->writable || !guest_map->private_mapping || trap_map == nullptr ||
      !trap_map->readable || !trap_map->executable || trap_map->writable)
    return false;

  if (!ResolvePayloadLocator(payload_elf, pid, runtime->call.payload_base,
                             kControlLocatorSymbol, &runtime->call.control) ||
      !ResolvePayloadLocator(payload_elf, pid, runtime->call.payload_base,
                             kEvidenceLocatorSymbol, &runtime->call.evidence) ||
      !ResolvePayloadLocator(payload_elf, pid, runtime->call.payload_base,
                             kRuntimeLocatorSymbol, &runtime->runtime) ||
      !ResolvePayloadLocator(payload_elf, pid, runtime->call.payload_base,
                             kRecordedFramesLocatorSymbol,
                             &runtime->recorded_frames) ||
      !ResolvePayloadLocator(payload_elf, pid, runtime->call.payload_base,
                             kRecordedIntervalsLocatorSymbol,
                             &runtime->recorded_intervals) ||
      !ResolvePayloadLocator(payload_elf, pid, runtime->call.payload_base,
                             kReplayFramesLocatorSymbol,
                             &runtime->replay_frames) ||
      !ResolvePayloadLocator(payload_elf, pid, runtime->call.payload_base,
                             kReplayIntervalsLocatorSymbol,
                             &runtime->replay_intervals) ||
      !ResolvePayloadLocator(payload_elf, pid, runtime->call.payload_base,
                             kBuildProfileLocatorSymbol,
                             &runtime->build_profile) ||
      !g2::DataAddressValid(runtime->call.maps, runtime->call.control,
                            sizeof(protocol::Control)) ||
      !g2::DataAddressValid(runtime->call.maps, runtime->call.evidence,
                            sizeof(protocol::Evidence)) ||
      !g2::DataAddressValid(runtime->call.maps, runtime->runtime,
                             sizeof(bridge::StateV1)) ||
      !g2::DataAddressValid(
          runtime->call.maps, runtime->recorded_frames,
          sizeof(recording::RecordingFrameV1) * protocol::kMaximumFrames) ||
      !g2::DataAddressValid(
          runtime->call.maps, runtime->recorded_intervals,
          sizeof(g4::IntervalSampleV1) * protocol::kMaximumIntervalSamples) ||
      !g2::DataAddressValid(
          runtime->call.maps, runtime->replay_frames,
          sizeof(recording::RecordingFrameV1) * protocol::kMaximumFrames) ||
      !g2::DataAddressValid(
          runtime->call.maps, runtime->replay_intervals,
          sizeof(g4::IntervalSampleV1) * protocol::kMaximumIntervalSamples) ||
      !g2::DataAddressValid(runtime->call.maps, runtime->build_profile,
                            sizeof(build_profile::Profile)))
    return false;
#endif

  const Mapping* game = FindMapping(runtime->call.maps, requested_game_base, 1);
  if (game == nullptr || game->start != requested_game_base ||
      game->offset != 0 || !game->readable || game->writable ||
      game->path.find("libAsphalt9.so") == std::string::npos)
    return false;
  // Only the read-only status command may reuse the installed build binding.
  // PID/start-time, payload identity and the live game mapping are checked
  // above; RemoteBuildProfileMatches and TargetsMatch are still checked below.
  // Rehashing the entire game ELF on every progress poll is not a TAS clock.
  if (status_only) {
    runtime->call.game_base = requested_game_base;
    return true;
  }
  FileImage game_file{};
  const std::string expected_native_sha = HexBytes(
      g_build_profile.core.native_sha256,
      sizeof(g_build_profile.core.native_sha256));
  const std::string expected_build_id = HexBytes(
      g_build_profile.core.build_id, sizeof(g_build_profile.core.build_id));
  if (!ReadPinnedRegularFile(game->path.c_str(), expected_native_sha.c_str(),
                             &game_file))
    return false;
  ElfImage game_elf{};
  if (!game_elf.Parse(game_file, EM_AARCH64) ||
      game_elf.build_id() != expected_build_id)
    return false;
  runtime->call.game_base = requested_game_base;
  return true;
}

bool ValidateMainObject(int mem, const std::vector<Mapping>& maps,
                        std::uintptr_t base, std::uintptr_t object) {
  if (object == 0 || (object & 7u) != 0 ||
      object > UINTPTR_MAX - kValidatedMainObjectSize)
    return false;
  const Mapping* object_map =
      FindMapping(maps, object, kValidatedMainObjectSize);
  if (object_map == nullptr || !object_map->readable ||
      !object_map->writable || object_map->executable ||
      !object_map->private_mapping)
    return false;

  std::uintptr_t primary_vtable = 0, embedded_vtable = 0;
  std::uintptr_t manager = 0, scheduler = 0;
  std::uint8_t enabled = 0;
  std::int64_t accumulator = 0;
  if (!g2::ReadAt(mem, object, &primary_vtable) ||
      !g2::ReadAt(mem, object + kEmbeddedTimeSourceOffset,
                  &embedded_vtable) ||
      !g2::ReadAt(mem, object + kTimeScaleManagerOffset, &manager) ||
      !g2::ReadAt(mem, object + kPhaseSchedulerOffset, &scheduler) ||
      !g2::ReadAt(mem, object + kEnabledOffset, &enabled) ||
      !g2::ReadAt(mem, object + protocol::kFixedDeltaOffset, &accumulator))
    return false;
  if (primary_vtable !=
          base + g_build_profile.core.main_time_source_vtable_rva ||
      embedded_vtable !=
          base + g_build_profile.core.embedded_time_source_vtable_rva ||
       enabled > 1 || manager == 0 || scheduler == 0 ||
      FindMapping(maps, scheduler, sizeof(std::uintptr_t)) == nullptr ||
      FindMapping(maps, manager + kTimeScaleValueOffset,
                  sizeof(std::uint32_t)) == nullptr)
    return false;

  std::uint32_t scale_bits = 0;
  if (!g2::ReadAt(mem, manager + kTimeScaleValueOffset, &scale_bits))
    return false;
  float scale = 0.0f;
  std::memcpy(&scale, &scale_bits, sizeof(scale));
  return std::isfinite(scale) && scale >= 0.0f && scale <= 64.0f &&
         accumulator > INT64_MIN;
}

bool ResolveMainObject(int mem, const std::vector<Mapping>& maps,
                       std::uintptr_t base, std::uintptr_t* output) {
  if (output == nullptr || base == 0) return false;
  const std::uintptr_t expected_vtable =
      base + g_build_profile.core.main_time_source_vtable_rva;
  std::vector<std::uintptr_t> candidates;
  std::vector<std::uint8_t> buffer(1u << 20);
  for (const Mapping& mapping : maps) {
    if (!mapping.readable || !mapping.writable || mapping.executable ||
        !mapping.private_mapping || mapping.path == "[vvar]" ||
        mapping.path == "[vdso]")
      continue;
    for (std::uintptr_t cursor = mapping.start; cursor < mapping.end;) {
      const std::size_t wanted = static_cast<std::size_t>(
          std::min<std::uintptr_t>(buffer.size(), mapping.end - cursor));
      const ssize_t got =
          pread(mem, buffer.data(), wanted, static_cast<off_t>(cursor));
      if (got <= 0) {
        cursor += wanted;
        continue;
      }
      for (std::size_t offset = 0;
           offset + sizeof(std::uintptr_t) <= static_cast<std::size_t>(got);
           offset += alignof(std::uintptr_t)) {
        std::uintptr_t value = 0;
        std::memcpy(&value, buffer.data() + offset, sizeof(value));
        if (value != expected_vtable) continue;
        const std::uintptr_t candidate = cursor + offset;
        if (ValidateMainObject(mem, maps, base, candidate))
          candidates.push_back(candidate);
      }
      cursor += static_cast<std::uintptr_t>(got);
    }
  }
  std::sort(candidates.begin(), candidates.end());
  candidates.erase(std::unique(candidates.begin(), candidates.end()),
                   candidates.end());
  if (candidates.size() != 1) {
    std::fprintf(stderr, "G4_OBJECT_DIAG main_candidates=%zu\n",
                 candidates.size());
    return false;
  }
  *output = candidates.front();
  return true;
}

bool ResolveInstallObjects(pid_t pid, std::uintptr_t outer_owner,
                           Runtime* runtime) {
  if (runtime == nullptr || outer_owner == 0 || (outer_owner & 7u) != 0)
    return false;
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
  int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
  if (mem < 0) return false;
  lifecycle::Resolution race{};
  vehicle::Layout player{};
  vehicle::BackendLayout backend{};
  const lifecycle::Profile lifecycle_profile = LifecycleProfile();
  if (!lifecycle::ResolveCountdownObject(pid, mem, runtime->call.game_base,
                                         lifecycle_profile, &race)) {
    std::fprintf(stderr,
        "G4_OBJECT_DIAG lifecycle=0 structural=%u countdown=%u scanned=%" PRIu64
        "\n", race.structurally_valid, race.countdown_candidates,
        race.scanned_bytes);
    close(mem);
    return false;
  }
  if (!vehicle::Resolve(pid, mem, runtime->call.game_base,
                        g_build_profile.vehicle, &player)) {
    std::fprintf(stderr, "G4_OBJECT_DIAG vehicle=0\n");
    close(mem);
    return false;
  }
  if (!vehicle::ResolveBackendLayout(mem, runtime->call.game_base, player,
                                     g_build_profile.vehicle, &backend)) {
    std::fprintf(stderr, "G4_OBJECT_DIAG backend=0 stage=%u\n",
                 backend.failure_stage);
    close(mem);
    return false;
  }

  std::uintptr_t outer_vptr = 0, implementation_vptr = 0, player_vptr = 0;
  std::uintptr_t barrel_backend = 0;
  std::uintptr_t setter_vptr = 0;
  std::uintptr_t main_object = 0;
  std::int64_t adjustment = 0;
  std::int64_t brake_adjustment = 0, steering_adjustment = 0;
  const bool interval =
      g2::ReadAt(mem, outer_owner, &outer_vptr) &&
      outer_vptr == runtime->call.game_base +
                        g_build_profile.core.step_options_vtable_rva &&
      outer_vptr >= sizeof(std::uintptr_t) * 4 &&
      g2::ReadAt(mem, outer_vptr - sizeof(std::uintptr_t) * 4, &adjustment) &&
      adjustment == kStepOptionsThisAdjustment &&
      outer_owner >= static_cast<std::uintptr_t>(-adjustment);
  if (!interval) {
    std::fprintf(stderr, "G4_OBJECT_DIAG stale_interval_owner=0x%" PRIxPTR "\n", outer_owner);
    close(mem);
    return false;
  }
  const std::uintptr_t implementation =
      outer_owner - static_cast<std::uintptr_t>(-adjustment);
  if (implementation >
      UINTPTR_MAX - protocol::kAdjustedSetterObjectOffset) {
    close(mem);
    return false;
  }
  const std::uintptr_t setter_object =
      implementation + protocol::kAdjustedSetterObjectOffset;
  const bool identities =
      g2::ReadAt(mem, implementation, &implementation_vptr) &&
       implementation_vptr ==
           runtime->call.game_base +
               g_build_profile.core.physics_implementation_vtable_rva &&
      g2::ReadAt(mem,
                 backend.angular_source_base + protocol::kBarrelBackendOffset,
                 &barrel_backend) &&
      barrel_backend == backend.physics_velocity_interface &&
      g2::ReadAt(mem, player.physics_base, &player_vptr) &&
      g2::ReadAt(mem, setter_object, &setter_vptr) &&
      setter_vptr ==
          runtime->call.game_base +
              g_build_profile.core.adjusted_setter_vtable_rva &&
      g2::ReadAt(mem, setter_vptr - 0x2D8, &brake_adjustment) &&
      g2::ReadAt(mem, setter_vptr - 0x2E0, &steering_adjustment) &&
      brake_adjustment == protocol::kAdjustedSetterThisAdjustment &&
      steering_adjustment == protocol::kAdjustedSetterThisAdjustment &&
      ResolveMainObject(mem, runtime->call.maps, runtime->call.game_base,
                        &main_object);
  std::uintptr_t physics_context = 0, physics_context_vptr = 0;
  const bool context = identities &&
      g2::ReadAt(mem, main_object + kPhysicsContextOffset, &physics_context) &&
      physics_context != 0 &&
      g2::ReadAt(mem, physics_context, &physics_context_vptr) &&
      physics_context_vptr ==
          runtime->call.game_base +
              g_build_profile.core.physics_context_vtable_rva;
  std::uintptr_t nitro_service = 0, nitro_vtable = 0, nitro_dispatch = 0;
  const bool nitro = identities &&
      g2::ReadAt(mem, implementation + protocol::kNitroServiceOffset,
                  &nitro_service) &&
      nitro_service != 0 && nitro_service <= UINTPTR_MAX - 8 &&
      g2::ReadAt(mem, nitro_service, &nitro_vtable) &&
      nitro_vtable ==
          runtime->call.game_base +
              g_build_profile.core.nitro_service_vtable_rva &&
      g2::ReadAt(mem, nitro_vtable + protocol::kNitroDispatchSlot,
                 &nitro_dispatch) &&
      nitro_dispatch ==
          runtime->call.game_base + g_build_profile.core.nitro_dispatch_rva;
  if (identities && (!context || !nitro)) {
    std::fprintf(stderr,
                 "G4_OBJECT_DIAG main=0x%" PRIxPTR
                 " interval_owner=0x%" PRIxPTR
                  " context=0x%" PRIxPTR " context_vptr=0x%" PRIxPTR
                  " nitro_service=0x%" PRIxPTR
                 " nitro_vtable=0x%" PRIxPTR
                 " nitro_dispatch=0x%" PRIxPTR " ready=0\n",
                  main_object, implementation, physics_context,
                  physics_context_vptr, nitro_service, nitro_vtable,
                 nitro_dispatch);
  }
  close(mem);
  if (!identities || !context || !nitro || player_vptr == 0) return false;

  runtime->call.outer_owner = outer_owner;
  runtime->call.outer_owner_vptr = outer_vptr;
  runtime->begin_owner = physics_context;
  runtime->begin_owner_vptr = physics_context_vptr;
  runtime->interval_owner = implementation;
  runtime->interval_owner_vptr = implementation_vptr;
  runtime->player = player.physics_base;
  runtime->player_vptr = player_vptr;
  runtime->lifecycle_object = race.selected.object;
  runtime->lifecycle_state = race.selected.state_address;
  runtime->main_object = main_object;
  runtime->main_object_vptr =
      runtime->call.game_base +
          g_build_profile.core.main_time_source_vtable_rva;
  runtime->nitro_state = nitro_service + 8;
  runtime->setter_object = setter_object;
  runtime->setter_object_vptr = setter_vptr;
  runtime->backend_interface = backend.interface;
  runtime->physics_velocity_interface = backend.physics_velocity_interface;
  runtime->native_body = backend.native_body;
  runtime->barrel_owner = backend.angular_source_base;
  runtime->barrel_owner_vptr = backend.source_vtable;
  return g2::DataAddressValid(runtime->call.maps, runtime->begin_owner,
                              sizeof(std::uintptr_t)) &&
         g2::DataAddressValid(runtime->call.maps, runtime->interval_owner,
                               kValidatedIntervalOwnerSize) &&
         g2::DataAddressValid(
             runtime->call.maps, runtime->barrel_owner,
             protocol::kBarrelRbxOffset + sizeof(std::uint64_t)) &&
         g2::DataAddressValid(runtime->call.maps, runtime->setter_object,
                              sizeof(std::uintptr_t)) &&
         g2::DataAddressValid(runtime->call.maps, runtime->player,
                               sizeof(std::uintptr_t)) &&
         g2::DataAddressValid(runtime->call.maps,
                              runtime->player +
                                  protocol::kBackendInterfaceOffset,
                              sizeof(std::uintptr_t)) &&
         g2::DataAddressValid(runtime->call.maps,
                              runtime->physics_velocity_interface +
                                  protocol::kNativeBodySlotOffset,
                              sizeof(std::uintptr_t)) &&
          g2::DataAddressValid(
              runtime->call.maps, runtime->native_body,
              recording::kAngularVelocityOffset +
                  recording::kAngularVelocitySize) &&
         g2::DataAddressValid(runtime->call.maps, runtime->lifecycle_state,
                              sizeof(std::uint32_t)) &&
         g2::DataAddressValid(runtime->call.maps, runtime->main_object,
                              kValidatedMainObjectSize);
}

// A replay-to-record handoff happens inside lifecycle 3 after the replay
// prefix has already consumed its final authoritative tick.  Re-running the
// countdown-only object scanner here is both unnecessary and impossible: the
// exact live objects are retained in the completed replay Control.  Bind those
// addresses into Runtime, then let the stopped-identity checks below prove
// every vptr and native relationship before issuing the guest rearm command.
bool BindPausedReplayRuntime(const protocol::Control& control,
                             Runtime* runtime) {
  if (runtime == nullptr || control.expected_begin_owner == 0 ||
      control.expected_interval_owner == 0 || control.expected_player == 0 ||
      control.lifecycle_state_address == 0 ||
      control.expected_main_object == 0 || control.expected_nitro_state == 0 ||
      control.expected_setter_object == 0 ||
      control.expected_backend_interface == 0 ||
      control.expected_physics_velocity_interface == 0 ||
      control.expected_native_body == 0 ||
      control.expected_barrel_owner == 0 ||
      control.expected_interval_owner >
          UINTPTR_MAX - kValidatedIntervalOwnerSize ||
      control.expected_barrel_owner >
          UINTPTR_MAX - protocol::kBarrelRbxOffset - sizeof(std::uint64_t) ||
      control.expected_player >
          UINTPTR_MAX - protocol::kBackendInterfaceOffset ||
      control.expected_physics_velocity_interface >
          UINTPTR_MAX - protocol::kNativeBodySlotOffset ||
      control.expected_native_body >
          UINTPTR_MAX - recording::kAngularVelocityOffset -
              recording::kAngularVelocitySize ||
      control.expected_main_object >
          UINTPTR_MAX - kValidatedMainObjectSize)
    return false;

  runtime->begin_owner = control.expected_begin_owner;
  runtime->begin_owner_vptr = control.expected_begin_owner_vptr;
  runtime->interval_owner = control.expected_interval_owner;
  runtime->interval_owner_vptr = control.expected_interval_owner_vptr;
  runtime->player = control.expected_player;
  runtime->player_vptr = control.expected_player_vptr;
  runtime->lifecycle_object = 0;
  runtime->lifecycle_state = control.lifecycle_state_address;
  runtime->main_object = control.expected_main_object;
  runtime->main_object_vptr = control.expected_main_object_vptr;
  runtime->nitro_state = control.expected_nitro_state;
  runtime->setter_object = control.expected_setter_object;
  runtime->setter_object_vptr = control.expected_setter_vptr;
  runtime->backend_interface = control.expected_backend_interface;
  runtime->physics_velocity_interface =
      control.expected_physics_velocity_interface;
  runtime->native_body = control.expected_native_body;
  runtime->barrel_owner = control.expected_barrel_owner;
  runtime->barrel_owner_vptr = control.expected_barrel_owner_vptr;

  return g2::DataAddressValid(runtime->call.maps, runtime->begin_owner,
                              sizeof(std::uintptr_t)) &&
         g2::DataAddressValid(runtime->call.maps, runtime->interval_owner,
                              kValidatedIntervalOwnerSize) &&
         g2::DataAddressValid(
             runtime->call.maps, runtime->barrel_owner,
             protocol::kBarrelRbxOffset + sizeof(std::uint64_t)) &&
         g2::DataAddressValid(runtime->call.maps, runtime->setter_object,
                              sizeof(std::uintptr_t)) &&
         g2::DataAddressValid(runtime->call.maps, runtime->player,
                              sizeof(std::uintptr_t)) &&
         g2::DataAddressValid(
             runtime->call.maps,
             runtime->player + protocol::kBackendInterfaceOffset,
             sizeof(std::uintptr_t)) &&
         g2::DataAddressValid(
             runtime->call.maps,
             runtime->physics_velocity_interface +
                 protocol::kNativeBodySlotOffset,
             sizeof(std::uintptr_t)) &&
         g2::DataAddressValid(
             runtime->call.maps, runtime->native_body,
             recording::kAngularVelocityOffset +
                 recording::kAngularVelocitySize) &&
         g2::DataAddressValid(runtime->call.maps, runtime->lifecycle_state,
                              sizeof(std::uint32_t)) &&
         g2::DataAddressValid(runtime->call.maps, runtime->main_object,
                              kValidatedMainObjectSize) &&
         g2::DataAddressValid(runtime->call.maps, runtime->nitro_state,
                              sizeof(std::uint32_t));
}

std::size_t HookPatchSize(std::uint32_t index);

bool NativePhysicsIdentityAlive(int mem, const Runtime& runtime) {
  std::uintptr_t interface_pointer = 0;
  std::uintptr_t native_pointer = 0;
  std::uintptr_t barrel_native_pointer = 0;
  std::uintptr_t native_vptr = 0;
  return g2::ReadAt(mem,
                    runtime.player + protocol::kBackendInterfaceOffset,
                    &interface_pointer) &&
         interface_pointer == runtime.backend_interface &&
         g2::ReadAt(mem,
                    runtime.physics_velocity_interface +
                        protocol::kNativeBodySlotOffset,
                    &native_pointer) &&
         native_pointer == runtime.native_body &&
         g2::ReadAt(mem,
                    runtime.barrel_owner + protocol::kBarrelBackendOffset,
                    &barrel_native_pointer) &&
         barrel_native_pointer == runtime.physics_velocity_interface &&
         g2::ReadAt(mem, runtime.native_body, &native_vptr) &&
         native_vptr == runtime.call.game_base +
                            g_build_profile.core.native_physics_body_vtable_rva;
}

bool NativePhysicsIdentityAlive(int mem, const protocol::Control& control) {
  std::uintptr_t interface_pointer = 0;
  std::uintptr_t native_pointer = 0;
  std::uintptr_t barrel_native_pointer = 0;
  std::uintptr_t native_vptr = 0;
  if (control.expected_player >
          UINTPTR_MAX - protocol::kBackendInterfaceOffset ||
      control.expected_physics_velocity_interface >
          UINTPTR_MAX - protocol::kNativeBodySlotOffset ||
      control.expected_barrel_owner >
          UINTPTR_MAX - protocol::kBarrelBackendOffset)
    return false;
  return g2::ReadAt(mem,
                    control.expected_player +
                        protocol::kBackendInterfaceOffset,
                    &interface_pointer) &&
         interface_pointer == control.expected_backend_interface &&
         g2::ReadAt(mem,
                    control.expected_physics_velocity_interface +
                        protocol::kNativeBodySlotOffset,
                    &native_pointer) &&
         native_pointer == control.expected_native_body &&
         g2::ReadAt(mem,
                    control.expected_barrel_owner +
                        protocol::kBarrelBackendOffset,
                    &barrel_native_pointer) &&
         barrel_native_pointer ==
             control.expected_physics_velocity_interface &&
         g2::ReadAt(mem, control.expected_native_body, &native_vptr) &&
         native_vptr == control.game_base +
                            g_build_profile.core.native_physics_body_vtable_rva;
}

bool StaticControlValid(const protocol::Control& control,
                        const Runtime& runtime, std::uint32_t limit) {
  if (std::memcmp(control.magic, protocol::kControlMagic,
                  sizeof(protocol::kControlMagic)) != 0 ||
      control.version != protocol::kVersion ||
      control.size != sizeof(protocol::Control) ||
      control.frame_limit != limit || control.generation == 0 ||
      control.session_id == 0 ||
      control.expected_begin_owner == 0 ||
      control.expected_begin_owner_vptr == 0 ||
      control.game_base != runtime.call.game_base ||
      control.expected_begin_owner_vptr !=
          runtime.call.game_base +
              g_build_profile.core.physics_context_vtable_rva ||
      (control.mode !=
           static_cast<std::uint32_t>(protocol::RunMode::kNeutral) &&
       control.mode !=
           static_cast<std::uint32_t>(protocol::RunMode::kRecord) &&
       control.mode !=
           static_cast<std::uint32_t>(protocol::RunMode::kReplay)) ||
      control.fixed_delta_us < recording::kMinimumFixedIntervalUs ||
      control.fixed_delta_us > recording::kMaximumFixedIntervalUs ||
      control.expected_main_object == 0 ||
      control.expected_interval_owner == 0 ||
      control.expected_interval_owner_vptr !=
          runtime.call.game_base +
              g_build_profile.core.physics_implementation_vtable_rva ||
      control.expected_player == 0 ||
      control.expected_player_vptr == 0 ||
      control.lifecycle_state_address == 0 ||
      control.expected_setter_object == 0 ||
      control.expected_main_object_vptr !=
          runtime.call.game_base +
              g_build_profile.core.main_time_source_vtable_rva ||
      control.expected_nitro_state == 0 ||
      control.expected_setter_vptr !=
          runtime.call.game_base +
              g_build_profile.core.adjusted_setter_vtable_rva ||
      control.expected_interval_owner >
          UINTPTR_MAX - protocol::kAdjustedSetterObjectOffset ||
      control.expected_setter_object !=
           control.expected_interval_owner +
               protocol::kAdjustedSetterObjectOffset ||
      control.expected_backend_interface == 0 ||
      control.expected_physics_velocity_interface == 0 ||
      control.expected_native_body == 0 ||
      control.expected_barrel_owner == 0 ||
      (control.expected_barrel_owner_vptr !=
           runtime.call.game_base +
               g_build_profile.core.vehicle_source_vtable_rva &&
       control.expected_barrel_owner_vptr !=
           runtime.call.game_base +
               g_build_profile.core.car_physics_body_source_vtable_rva))
    return false;
  const auto completion = static_cast<protocol::CompletionPolicy>(
      control.completion_policy);
  if ((completion != protocol::CompletionPolicy::kFixedFrameLimit &&
       completion != protocol::CompletionPolicy::kRaceLifecycle) ||
      (completion == protocol::CompletionPolicy::kRaceLifecycle &&
       control.mode !=
           static_cast<std::uint32_t>(protocol::RunMode::kRecord)))
    return false;
  const bool replay_mode =
      control.mode == static_cast<std::uint32_t>(protocol::RunMode::kReplay);
  if (replay_mode) {
    if (control.replay_frame_count != limit ||
        (control.replay_interval_count == 0 &&
         control.fixed_delta_us != 8333 && control.fixed_delta_us != 6944) ||
        control.replay_interval_count > protocol::kMaximumIntervalSamples ||
        !RecordingHashIsNonzero(control) ||
        control.replay_speed_factor < protocol::kMinimumReplaySpeedFactor ||
        control.replay_speed_factor > protocol::kMaximumReplaySpeedFactor ||
        control.replay_speed_reserved >
            protocol::kReplayCompletionAtomicRecord)
      return false;
  } else if (control.replay_frame_count != 0 ||
             control.replay_interval_count != 0 ||
             !RecordingHashIsZero(control) ||
             control.replay_speed_factor != 1 ||
             (control.mode == static_cast<std::uint32_t>(
                                  protocol::RunMode::kRecord)
                  ? control.replay_speed_reserved >
                        protocol::kRecordCheckpointBarrierHeld
                  : control.replay_speed_reserved != 0)) {
    return false;
  }
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index) {
    if (control.target_entry[index] !=
            runtime.call.game_base + g_build_profile.core.hook_rvas[index] ||
        control.target_tail[index] !=
            control.target_entry[index] + HookPatchSize(index) ||
        control.wrapper[index] == 0)
      return false;
  }
  if (control.lifecycle_target_entry !=
          runtime.call.game_base +
              g_build_profile.core.lifecycle_shared_enter_rva ||
      control.lifecycle_target_tail != control.lifecycle_target_entry + 16u ||
      control.lifecycle_wrapper == 0)
    return false;
  for (std::uint32_t index = 0;
       index < protocol::kInstalledSetterCount; ++index) {
    if (control.setter_slot[index] !=
            runtime.call.game_base +
                g_build_profile.core.adjusted_setter_vtable_rva +
                protocol::kSetterSlotOffsets[index] ||
        control.setter_original[index] !=
            runtime.call.game_base +
                g_build_profile.core.setter_original_rvas[index] ||
        control.setter_wrapper[index] == 0)
      return false;
  }
  for (std::uint32_t index = protocol::kInstalledSetterCount;
       index < protocol::kSetterCount; ++index)
    if (control.setter_slot[index] != 0 ||
        control.setter_original[index] != 0 ||
        control.setter_wrapper[index] != 0)
      return false;
  if (!ArchivedFieldsZero(control) &&
      !ArchivedReplayFieldsValid(control, limit) &&
      !ArchivedRecordFieldsValid(control, limit))
    return false;
  return true;
}

bool ReadReceipt(int mem, const Runtime& runtime, protocol::Control* control,
                 protocol::Evidence* evidence, bridge::StateV1* state) {
  return control != nullptr && evidence != nullptr && state != nullptr &&
         g2::ReadAt(mem, runtime.call.control, control) &&
         g2::ReadAt(mem, runtime.call.evidence, evidence) &&
         g2::ReadAt(mem, runtime.runtime, state);
}

bool RemoteBuildProfileMatches(int mem, const Runtime& runtime) {
  build_profile::Profile observed{};
  return g2::ReadAt(mem, runtime.build_profile, &observed) &&
         build_profile::Valid(observed) &&
         std::memcmp(&observed, &g_build_profile.core,
                     sizeof(observed)) == 0;
}

bool RemoteBuildProfileFresh(int mem, const Runtime& runtime) {
  build_profile::Profile observed{};
  return g2::ReadAt(mem, runtime.build_profile, &observed) &&
         BytesZero(&observed, sizeof(observed));
}

bool PublishRemoteBuildProfile(int mem, const Runtime& runtime) {
  if (!RemoteBuildProfileFresh(mem, runtime) ||
      !g2::WriteExact(mem, runtime.build_profile, &g_build_profile.core,
                      sizeof(g_build_profile.core)))
    return false;
  return RemoteBuildProfileMatches(mem, runtime);
}

// Lifecycle completion polling must remain constant-size even when G8 uses
// the extended frame capacity.  The terminal bit and fault status are both
// published in Control/Evidence; the full per-tick State is read exactly once
// by the final status/dump path after this waiter returns.
bool ReadCompletionBytes(int mem, std::uintptr_t address, void* output,
                         std::size_t size) {
  if (mem < 0 || address == 0 || output == nullptr || size == 0 ||
      address > UINTPTR_MAX - size)
    return false;
  // /proc/PID/mem occasionally reports EINTR or a short read while NativeBridge
  // mappings are busy.  A completion observer must not turn one such transport
  // hiccup into an early replay termination.  Retry this tiny read locally;
  // the outer waiter separately bounds consecutive failed samples.
  constexpr std::uint32_t kReadAttempts = 3;
  for (std::uint32_t attempt = 0; attempt < kReadAttempts; ++attempt) {
    std::size_t copied = 0;
    while (copied < size) {
      const ssize_t got = pread(
          mem, static_cast<std::uint8_t*>(output) + copied, size - copied,
          static_cast<off_t>(address + copied));
      if (got > 0) {
        copied += static_cast<std::size_t>(got);
        continue;
      }
      if (got < 0 && errno == EINTR) continue;
      break;
    }
    if (copied == size) return true;
    usleep(250);
  }
  return false;
}

bool ReadCompletionReceipt(int mem, const Runtime& runtime,
                           protocol::Control* control,
                           protocol::Evidence* evidence) {
  return control != nullptr && evidence != nullptr &&
         ReadCompletionBytes(mem, runtime.call.control, control,
                             sizeof(*control)) &&
         ReadCompletionBytes(mem, runtime.call.evidence, evidence,
                             sizeof(*evidence));
}

bool SealedLifecycleRecordingValid(
    const protocol::Control& control, const protocol::Evidence& evidence,
    const bridge::StateV1& state) {
  return control.mode ==
          static_cast<std::uint32_t>(protocol::RunMode::kRecord) &&
      control.completion_policy == static_cast<std::uint32_t>(
          protocol::CompletionPolicy::kRaceLifecycle) &&
      control.enabled == 0 && control.completed == 1 &&
      control.active_helpers == 0 && evidence.status == protocol::kComplete &&
      evidence.first_error == 0 &&
      evidence.completion_reason == static_cast<std::uint32_t>(
          protocol::CompletionReason::kRaceLifecycle) &&
      state.tick.coordinator.lifecycle == coordinator::Lifecycle::kInactive &&
      state.tick.coordinator.tick_phase == coordinator::TickPhase::kClosed &&
      state.tick.coordinator.generation == control.generation &&
      state.receipt_count != 0 &&
      state.receipt_count == evidence.recorded_frames &&
      (evidence.interval_records != 0 ||
       control.fixed_delta_us == 8333 || control.fixed_delta_us == 6944);
}

bool SealedLifecycleRecordForRearmValid(
    const protocol::Control& control, const protocol::Evidence& evidence,
    const bridge::StateV1& state) {
  const bool race_end =
      evidence.completion_reason == static_cast<std::uint32_t>(
          protocol::CompletionReason::kRaceLifecycle) &&
      state.tick.coordinator.lifecycle == coordinator::Lifecycle::kInactive;
  const bool manual_checkpoint =
      evidence.completion_reason == static_cast<std::uint32_t>(
          protocol::CompletionReason::kManualCheckpoint) &&
      state.tick.coordinator.lifecycle == coordinator::Lifecycle::kInRace;
  return control.mode ==
          static_cast<std::uint32_t>(protocol::RunMode::kRecord) &&
      control.completion_policy == static_cast<std::uint32_t>(
          protocol::CompletionPolicy::kRaceLifecycle) &&
      control.enabled == 0 && control.completed == 1 &&
      control.active_helpers == 0 && evidence.status == protocol::kComplete &&
      evidence.first_error == 0 && (race_end || manual_checkpoint) &&
      state.tick.coordinator.tick_phase == coordinator::TickPhase::kClosed &&
      (state.tick.coordinator.generation == control.generation ||
       (control.archived_generation != 0 &&
        state.tick.coordinator.generation ==
            control.archived_generation)) &&
      state.receipt_count != 0 &&
      state.receipt_count == evidence.recorded_frames &&
      (evidence.interval_records != 0 ||
       control.fixed_delta_us == 8333 || control.fixed_delta_us == 6944);
}

bool PausedReplayForBranchValid(const protocol::Control& control,
                                const protocol::Evidence& evidence,
                                const bridge::StateV1& state);

bool CompletedResidentSessionForRearmValid(
    const protocol::Control& control, const protocol::Evidence& evidence,
    const bridge::StateV1& state) {
  if (control.mode ==
      static_cast<std::uint32_t>(protocol::RunMode::kRecord))
    return SealedLifecycleRecordForRearmValid(control, evidence, state);
  if (control.mode ==
      static_cast<std::uint32_t>(protocol::RunMode::kReplay))
    return PausedReplayForBranchValid(control, evidence, state);
  return false;
}

bool PausedReplayForBranchValid(const protocol::Control& control,
                                const protocol::Evidence& evidence,
                                const bridge::StateV1& state) {
  return control.mode ==
             static_cast<std::uint32_t>(protocol::RunMode::kReplay) &&
      control.completion_policy == static_cast<std::uint32_t>(
          protocol::CompletionPolicy::kFixedFrameLimit) &&
      control.enabled == 0 && control.completed == 1 &&
      control.active_helpers == 0 && evidence.status == protocol::kComplete &&
      evidence.first_error == 0 &&
      evidence.completion_reason == static_cast<std::uint32_t>(
          protocol::CompletionReason::kFixedFrameLimit) &&
      state.tick.complete == 1 && !state.input_action.tick_open &&
      state.tick.coordinator.lifecycle == coordinator::Lifecycle::kInRace &&
      state.tick.coordinator.tick_phase == coordinator::TickPhase::kClosed &&
      state.tick.coordinator.tick == state.receipt_count &&
      state.receipt_count == control.frame_limit &&
      evidence.published_ticks == state.receipt_count &&
      evidence.recorded_frames == 0 && evidence.interval_records == 0;
}

bool ActiveReplayForBranchValid(const protocol::Control& control,
                                const protocol::Evidence& evidence,
                                const bridge::StateV1& state) {
  return control.mode ==
             static_cast<std::uint32_t>(protocol::RunMode::kReplay) &&
      control.completion_policy == static_cast<std::uint32_t>(
          protocol::CompletionPolicy::kFixedFrameLimit) &&
      control.enabled == 1 && control.completed == 0 &&
      control.active_helpers == 0 && evidence.status == protocol::kArmed &&
      evidence.first_error == 0 &&
      evidence.completion_reason == static_cast<std::uint32_t>(
          protocol::CompletionReason::kNone) &&
      state.tick.complete == 0 && !state.input_action.tick_open &&
      state.tick.coordinator.lifecycle == coordinator::Lifecycle::kInRace &&
      state.tick.coordinator.tick_phase == coordinator::TickPhase::kClosed &&
      state.tick.coordinator.tick == state.receipt_count &&
      state.receipt_count != 0 && state.receipt_count < control.frame_limit &&
      evidence.published_ticks == state.receipt_count &&
      evidence.recorded_frames == 0 && evidence.interval_records == 0;
}

bool OriginalTargetsMatch(int mem, const Runtime& runtime) {
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index) {
    const std::size_t size = HookPatchSize(index);
    std::uint8_t observed[16]{};
    if (pread(mem, observed, size,
              static_cast<off_t>(runtime.call.game_base +
                                 g_build_profile.core.hook_rvas[index])) !=
            static_cast<ssize_t>(size) ||
        std::memcmp(observed, kExpectedPrologues[index], size) != 0)
      return false;
  }
  std::uint8_t lifecycle[16]{};
  if (pread(mem, lifecycle, sizeof(lifecycle),
            static_cast<off_t>(runtime.call.game_base +
                g_build_profile.core.lifecycle_shared_enter_rva)) !=
          static_cast<ssize_t>(sizeof(lifecycle)) ||
      std::memcmp(lifecycle, kExpectedLifecyclePrologue,
                  sizeof(lifecycle)) != 0)
    return false;
  const std::uintptr_t random_rvas[protocol::kRandomHookCount] = {
      g_build_profile.core.barrel_random_bool_rva,
      g_build_profile.core.barrel_random_lerp_rva,
  };
  for (std::uint32_t index = 0; index < protocol::kRandomHookCount; ++index) {
    std::uint8_t observed[16]{};
    if (pread(mem, observed, sizeof(observed),
              static_cast<off_t>(runtime.call.game_base + random_rvas[index])) !=
            static_cast<ssize_t>(sizeof(observed)) ||
        std::memcmp(observed, kExpectedRandomOriginalPrefix[index],
                    sizeof(kExpectedRandomOriginalPrefix[index])) != 0)
      return false;
    std::uint32_t fourth_instruction = 0;
    std::memcpy(&fourth_instruction, observed + 12, sizeof(fourth_instruction));
    if ((fourth_instruction & 0xfc000000u) != 0x94000000u) return false;
  }
  for (std::uint32_t index = 0;
       index < protocol::kInstalledSetterCount; ++index) {
    std::uint8_t body[28]{};
    std::uintptr_t slot_value = 0;
    const std::uintptr_t original =
        runtime.call.game_base +
            g_build_profile.core.setter_original_rvas[index];
    const std::uintptr_t slot =
        runtime.call.game_base +
            g_build_profile.core.adjusted_setter_vtable_rva +
        protocol::kSetterSlotOffsets[index];
    if (pread(mem, body, sizeof(body), static_cast<off_t>(original)) !=
            static_cast<ssize_t>(sizeof(body)) ||
        std::memcmp(body, kExpectedSetterBodies[index], sizeof(body)) != 0 ||
        !g2::ReadAt(mem, slot, &slot_value) || slot_value != original)
      return false;
  }
  return true;
}

bool PreloadedFreshValid(const protocol::Control& control,
                         const protocol::Evidence& evidence,
                         const bridge::StateV1& state) {
  if (std::memcmp(control.magic, protocol::kControlMagic,
                  sizeof(protocol::kControlMagic)) != 0 ||
      control.version != protocol::kVersion ||
      control.size != sizeof(protocol::Control) || control.enabled != 0 ||
      control.frame_limit != 0 || control.generation != 0 ||
      control.completed != 0 || control.active_helpers != 0 ||
      control.completion_policy != static_cast<std::uint32_t>(
          protocol::CompletionPolicy::kFixedFrameLimit) ||
      control.session_id != 0 ||
      control.expected_begin_owner != 0 ||
      control.expected_begin_owner_vptr != 0 ||
      control.expected_interval_owner != 0 ||
      control.expected_interval_owner_vptr != 0 ||
      control.expected_player != 0 || control.expected_player_vptr != 0 ||
      control.lifecycle_state_address != 0 ||
      control.expected_main_object != 0 ||
      control.expected_main_object_vptr != 0 || control.game_base != 0 ||
      control.expected_nitro_state != 0 ||
      control.expected_setter_object != 0 ||
      control.expected_setter_vptr != 0 ||
      control.expected_backend_interface != 0 ||
      control.expected_physics_velocity_interface != 0 ||
      control.expected_native_body != 0 ||
      control.expected_barrel_owner != 0 ||
      control.expected_barrel_owner_vptr != 0 ||
      control.fixed_delta_us != 0 || control.replay_frame_count != 0 ||
      control.replay_interval_count != 0 ||
      control.replay_speed_factor != 0 ||
      control.replay_speed_reserved != 0 ||
      control.lifecycle_target_entry != 0 ||
      control.lifecycle_target_tail != 0 ||
      control.lifecycle_wrapper != 0 || control.pending_mode != 0 ||
      control.pending_state != 0 || control.pending_generation != 0 ||
      control.pending_reserved != 0)
    return false;
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index)
    if (control.target_entry[index] != 0 || control.target_tail[index] != 0 ||
        control.wrapper[index] != 0)
      return false;
  for (std::uint32_t index = 0; index < protocol::kSetterCount; ++index)
    if (evidence.setter_entries[index] != 0 ||
        evidence.setter_qualified_events[index] != 0 ||
        evidence.setter_overrides[index] != 0 ||
        evidence.setter_last_object[index] != 0 ||
        evidence.setter_last_bits[index] != 0 ||
        evidence.setter_cache_bits[index] != 0)
      return false;
  for (std::uint32_t index = 0; index < protocol::kSetterCount; ++index)
    if (control.setter_slot[index] != 0 ||
        control.setter_original[index] != 0 ||
        control.setter_wrapper[index] != 0)
      return false;
  if (!ArchivedFieldsZero(control)) return false;
  if (!RecordingHashIsZero(control)) return false;
  constexpr std::uint64_t kFreshFlags =
      protocol::kNoExternalPerFrameStop | protocol::kG3CoordinatorReused;
  if (std::memcmp(evidence.magic, protocol::kEvidenceMagic,
                  sizeof(protocol::kEvidenceMagic)) != 0 ||
      evidence.version != protocol::kVersion ||
      evidence.size != sizeof(protocol::Evidence) ||
      evidence.status != protocol::kPassive || evidence.first_error != 0 ||
      evidence.flags != kFreshFlags || evidence.install_calls != 0 ||
      evidence.restore_calls != 0 || evidence.ignored_events != 0 ||
      evidence.core_faults != 0 || evidence.recursive_entries != 0 ||
      evidence.published_ticks != 0 || state.receipt_count != 0 ||
      state.tick.complete != 0 || state.tick.coordinator.tick != 0 ||
      state.tick.coordinator.failures != 0)
    return false;
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index)
    if (evidence.wrapper_entries[index] != 0 ||
        evidence.qualified_events[index] != 0 ||
        evidence.first_tid[index] != 0 || evidence.last_tid[index] != 0)
      return false;
  return true;
}

bool CompleteReceiptValid(const protocol::Control& control,
                          const protocol::Evidence& evidence,
                          const bridge::StateV1& state, std::uint32_t limit,
                          std::uint32_t* reject_code);

bool RestoredSessionReusableValid(const protocol::Control& control,
                                  const protocol::Evidence& evidence,
                                  const bridge::StateV1& state) {
  constexpr std::uint64_t kRequiredRestoreFlags =
      protocol::kAllRestored | protocol::kAllSetterSlotsRestored;
  if (control.frame_limit == 0 ||
      control.frame_limit > protocol::kMaximumFrames ||
      control.enabled != 0 || control.completed != 1 ||
      control.active_helpers != 0 || evidence.status != protocol::kRestored ||
      evidence.first_error != 0 ||
      (evidence.flags & kRequiredRestoreFlags) != kRequiredRestoreFlags ||
      evidence.install_calls != 1 || evidence.restore_calls != 1 ||
      evidence.core_faults != 0 ||
      state.input_action.tick_open ||
      state.tick.coordinator.tick_phase != coordinator::TickPhase::kClosed ||
      state.tick.coordinator.failures != 0)
    return false;
  // Mirror the payload's RestoredSessionReusable() contract exactly.  The
  // completed run was already validated before Restore; applying the stricter
  // terminal receipt validator again after a UI pause rejects a legitimate
  // restored session because post-completion pass-through callbacks are not
  // part of the sealed run.  InstallPassive performs the same authoritative
  // restored-session check inside the payload before it resets any metadata.
  return true;
}

bool PassiveControlValid(const protocol::Control& control,
                         const Runtime& runtime) {
  if (std::memcmp(control.magic, protocol::kControlMagic,
                  sizeof(protocol::kControlMagic)) != 0 ||
      control.version != protocol::kVersion ||
      control.size != sizeof(protocol::Control) || control.enabled != 0 ||
      control.frame_limit != 0 || control.generation != 0 ||
      control.completed != 0 || control.active_helpers != 0 ||
      control.completion_policy != static_cast<std::uint32_t>(
          protocol::CompletionPolicy::kFixedFrameLimit) ||
      control.session_id != 0 ||
      control.expected_begin_owner != 0 ||
      control.expected_begin_owner_vptr != 0 ||
      control.expected_interval_owner != 0 ||
      control.expected_interval_owner_vptr != 0 ||
      control.expected_player != 0 || control.expected_player_vptr != 0 ||
      control.lifecycle_state_address != 0 ||
      control.expected_main_object != 0 ||
      control.expected_main_object_vptr != 0 ||
      control.expected_nitro_state != 0 ||
      control.expected_setter_object != 0 ||
      control.expected_setter_vptr != 0 ||
      control.expected_backend_interface != 0 ||
      control.expected_physics_velocity_interface != 0 ||
      control.expected_native_body != 0 ||
      control.expected_barrel_owner != 0 ||
      control.expected_barrel_owner_vptr != 0 ||
      control.game_base != runtime.call.game_base ||
      control.fixed_delta_us != 0 || control.replay_frame_count != 0 ||
      control.replay_interval_count != 0 ||
      control.replay_speed_factor != 0 ||
      control.replay_speed_reserved != 0)
    return false;
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index) {
    const std::uintptr_t expected_target =
        runtime.call.game_base + g_build_profile.core.hook_rvas[index];
    if (control.target_entry[index] != expected_target ||
        control.target_tail[index] != expected_target + HookPatchSize(index) ||
        control.wrapper[index] == 0)
      return false;
  }
  if (control.lifecycle_target_entry !=
          runtime.call.game_base +
              g_build_profile.core.lifecycle_shared_enter_rva ||
      control.lifecycle_target_tail != control.lifecycle_target_entry + 16u ||
      control.lifecycle_wrapper == 0 || control.pending_mode != 0 ||
      control.pending_state != 0 || control.pending_generation != 0 ||
      control.pending_reserved != 0)
    return false;
  for (std::uint32_t index = 0;
       index < protocol::kInstalledSetterCount; ++index) {
    const std::uintptr_t expected_slot =
        runtime.call.game_base +
            g_build_profile.core.adjusted_setter_vtable_rva +
        protocol::kSetterSlotOffsets[index];
    const std::uintptr_t expected_original =
        runtime.call.game_base +
            g_build_profile.core.setter_original_rvas[index];
    if (control.setter_slot[index] != expected_slot ||
        control.setter_original[index] != expected_original ||
        control.setter_wrapper[index] == 0)
      return false;
  }
  for (std::uint32_t index = protocol::kInstalledSetterCount;
       index < protocol::kSetterCount; ++index)
    if (control.setter_slot[index] != 0 ||
        control.setter_original[index] != 0 ||
        control.setter_wrapper[index] != 0)
      return false;
  if (!ArchivedFieldsZero(control)) return false;
  if (!RecordingHashIsZero(control)) return false;
  return true;
}

bool PassiveReceiptValid(const protocol::Control& control,
                         const protocol::Evidence& evidence,
                         const bridge::StateV1& state) {
  constexpr std::uint64_t kRequired =
      protocol::kGameIdentity | protocol::kAllPrologues |
      protocol::kNoExternalPerFrameStop | protocol::kG3CoordinatorReused |
      protocol::kPayloadPreloaded;
  if (std::memcmp(evidence.magic, protocol::kEvidenceMagic,
                  sizeof(protocol::kEvidenceMagic)) != 0 ||
      evidence.version != protocol::kVersion ||
      evidence.size != sizeof(protocol::Evidence) ||
      evidence.status != protocol::kPassiveInstalled ||
      evidence.first_error != 0 || (evidence.flags & kRequired) != kRequired ||
      (evidence.flags & (protocol::kLifecycleIdentity |
                         protocol::kObjectIdentity |
                         protocol::kAllRestored)) != 0 ||
      evidence.install_calls != 1 || evidence.restore_calls != 0 ||
      evidence.ignored_events != 0 || evidence.core_faults != 0 ||
      evidence.recursive_entries != 0 || evidence.published_ticks != 0 ||
      state.receipt_count != 0 || state.tick.complete != 0 ||
      state.tick.coordinator.tick != 0 || state.tick.coordinator.failures != 0)
    return false;
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index) {
    if (evidence.qualified_events[index] != 0 ||
        evidence.first_tid[index] != 0 || evidence.last_tid[index] != 0)
      return false;
  }
  for (std::uint32_t index = 0; index < protocol::kSetterCount; ++index)
    if (evidence.setter_entries[index] != 0 ||
        evidence.setter_qualified_events[index] != 0 ||
        evidence.setter_overrides[index] != 0 ||
        evidence.setter_last_object[index] != 0 ||
        evidence.setter_last_bits[index] != 0 ||
        evidence.setter_cache_bits[index] != 0)
      return false;
  return control.enabled == 0 && control.completed == 0 &&
         control.active_helpers == 0;
}

bool CompleteReceiptValid(const protocol::Control& control,
                           const protocol::Evidence& evidence,
                           const bridge::StateV1& state, std::uint32_t limit,
                           std::uint32_t* reject_code = nullptr) {
  constexpr std::uint64_t kRequiredFlags =
      protocol::kGameIdentity | protocol::kLifecycleIdentity |
      protocol::kObjectIdentity | protocol::kAllPrologues |
      protocol::kAllPatchesPublished | protocol::kAllPatchesReadBack |
      protocol::kAllLogicalRx | protocol::kNoExternalPerFrameStop |
      protocol::kPayloadPreloaded | protocol::kG3CoordinatorReused |
      protocol::kControlPairBound | protocol::kFixedDeltaBound |
      protocol::kNitroServiceBound | protocol::kExactIntervalStream |
      protocol::kSetterVtableBound | protocol::kAllSetterSlotsPublished |
      protocol::kPhysicsBackendBound | protocol::kBarrelTailBound |
      protocol::kDeterministicBarrelPrngBound;
  const bool recording_mode =
      control.mode == static_cast<std::uint32_t>(protocol::RunMode::kRecord);
  const bool replay_mode =
      control.mode == static_cast<std::uint32_t>(protocol::RunMode::kReplay);
  const bool lifecycle_completion =
      control.completion_policy == static_cast<std::uint32_t>(
          protocol::CompletionPolicy::kRaceLifecycle);
  const bool manual_checkpoint = lifecycle_completion &&
      evidence.completion_reason == static_cast<std::uint32_t>(
          protocol::CompletionReason::kManualCheckpoint);
  const std::uint32_t completed_frames = lifecycle_completion
      ? state.receipt_count
      : limit;
  const std::uint32_t expected_completion_reason =
      static_cast<std::uint32_t>(
          manual_checkpoint
              ? protocol::CompletionReason::kManualCheckpoint
              : lifecycle_completion
                    ? protocol::CompletionReason::kRaceLifecycle
                    : protocol::CompletionReason::kFixedFrameLimit);
  const auto reject = [&](std::uint32_t code) {
    if (reject_code != nullptr) *reject_code = code;
    std::printf(
        "G4_COMPLETE_REJECT code=%u active=%u core=%" PRIu64
        " frames=%u status=%u error=%u flags=0x%" PRIx64
        " missing=0x%" PRIx64 " completion=%u/%u mode=%u policy=%u"
        " generation=%u/%u"
        " published=%" PRIu64 " coordinator=%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%" PRIu64
        " submit=%" PRIu64 "/%" PRIu64
        " control=%" PRIu64 ",%" PRIu64
        " fixed=%" PRIu64 " interval=%" PRIu64 ",%" PRIu64
        " physics=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        "\n",
        code, control.active_helpers, evidence.core_faults, completed_frames,
        evidence.status, evidence.first_error, evidence.flags,
        kRequiredFlags & ~evidence.flags, evidence.completion_reason,
        expected_completion_reason, control.mode, control.completion_policy,
        control.generation, state.tick.coordinator.generation,
        evidence.published_ticks, state.tick.coordinator.ticks_begun,
        state.tick.coordinator.pre_physics_events,
        state.tick.coordinator.physics_interval_calls,
        state.tick.coordinator.tick,
        evidence.wrapper_entries[protocol::kSubmitHook],
        evidence.qualified_events[protocol::kSubmitHook],
        evidence.control_pair_reads, evidence.control_pair_writes,
        evidence.fixed_delta_writes, evidence.interval_calls,
        evidence.interval_overrides, evidence.physics_equal_frames,
        evidence.physics_corrected_frames, evidence.physics_correction_writes,
        evidence.physics_skipped_frames);
    return false;
  };
  if (completed_frames == 0 || completed_frames > limit)
    return reject(30);
  const bool sparse_updates = control.fixed_delta_us == 8333 || control.fixed_delta_us == 6944;
  std::uint64_t integrated_frames = 0;
  for (std::uint32_t i = 0; i < completed_frames; ++i)
    integrated_frames += state.tick.receipts[i].physics_interval_calls != 0;
  if (!sparse_updates && integrated_frames != completed_frames) return reject(32);
  // QueueArchivedSession publishes the next session's Control before Retry,
  // while the sealed receipts intentionally remain owned by the predecessor
  // generation.  Validate that immutable receipt identity against itself and
  // the retained coordinator generation instead of comparing it with the
  // already-staged next Control.  This keeps strict identity validation while
  // allowing cancel-queue -> replay of a successfully completed race.
  const std::uint64_t receipt_session_id = state.tick.receipts[0].session_id;
  const std::uint32_t receipt_generation =
      state.tick.receipts[0].generation;
  const bool current_receipt_generation =
      receipt_generation == control.generation;
  const bool archived_receipt_generation =
      control.archived_generation != 0 &&
      receipt_generation == control.archived_generation;
  if (receipt_session_id == 0 || receipt_generation == 0 ||
      (!current_receipt_generation && !archived_receipt_generation) ||
      state.tick.coordinator.generation != receipt_generation ||
      (state.tick.coordinator.session_id != 0 &&
       state.tick.coordinator.session_id != receipt_session_id))
    return reject(31);
  if (control.completed != 1 || control.enabled != 0 ||
      control.active_helpers != 0)
    return reject(1);
  if ((evidence.status != protocol::kComplete &&
       evidence.status != protocol::kRestored) ||
      evidence.first_error != 0 ||
      (evidence.flags & kRequiredFlags) != kRequiredFlags ||
      evidence.core_faults != 0 ||
      evidence.published_ticks != completed_frames ||
       evidence.completion_reason != expected_completion_reason)
    return reject(2);
  if (state.tick.complete != (lifecycle_completion ? 0u : 1u) ||
      state.receipt_count != completed_frames ||
      state.tick.coordinator.ticks_published != completed_frames ||
      state.tick.coordinator.ticks_begun != completed_frames ||
      state.tick.coordinator.pre_physics_events != integrated_frames ||
      state.tick.coordinator.physics_interval_calls < integrated_frames ||
      state.tick.tick_begin_entries != completed_frames ||
      state.tick.pre_physics_entries != integrated_frames ||
      state.tick.physics_interval_calls < integrated_frames)
    return reject(3);
  if (evidence.qualified_events[protocol::kSubmitHook] != completed_frames ||
      (replay_mode &&
       (evidence.wrapper_entries[protocol::kLogicDispatcherHook] == 0 ||
        evidence.qualified_events[protocol::kLogicDispatcherHook] <
            completed_frames)) ||
      evidence.interval_calls != state.tick.physics_interval_calls ||
      evidence.control_pair_reads != completed_frames ||
      evidence.control_pair_writes != 0 ||
      evidence.fixed_delta_writes !=
          ((recording_mode || replay_mode) ? completed_frames : 0u))
    return reject(4);
  if (evidence.interval_overrides != 0 ||
      evidence.interval_records !=
          (recording_mode ? evidence.interval_calls : 0u) ||
      (!replay_mode && evidence.suppressed_nitro_calls != 0) ||
      (!replay_mode && evidence.injected_nitro_calls != 0) ||
      evidence.recorded_frames !=
          (recording_mode ? completed_frames : 0u))
    return reject(5);
  if (evidence.physics_equal_frames + evidence.physics_corrected_frames +
              evidence.physics_skipped_frames !=
          (replay_mode ? completed_frames : 0u) ||
      evidence.physics_skipped_frames != 0 ||
      evidence.physics_correction_writes !=
          evidence.physics_corrected_frames * 2u)
    return reject(6);
  if (evidence.barrel_rbx_overrides >
          evidence.qualified_events[protocol::kBarrelRollHook] ||
      evidence.barrel_angular_overrides >
          evidence.qualified_events[protocol::kBarrelYawHook] ||
      evidence.barrel_rbx_records !=
          (recording_mode
               ? evidence.qualified_events[protocol::kBarrelRollHook]
               : 0u) ||
      evidence.barrel_angular_records !=
          (recording_mode
               ? evidence.qualified_events[protocol::kBarrelYawHook]
               : 0u) ||
      (!replay_mode &&
       (evidence.barrel_rbx_overrides != 0 ||
        evidence.barrel_angular_overrides != 0)))
    return reject(8);
  if (state.tick.coordinator.tick_phase != coordinator::TickPhase::kClosed ||
      state.tick.coordinator.failures != 0 ||
      (lifecycle_completion
           ? (manual_checkpoint
                  ? (state.tick.coordinator.lifecycle !=
                         coordinator::Lifecycle::kInRace ||
                     state.tick.coordinator.tick != completed_frames)
                  : (state.tick.coordinator.lifecycle !=
                         coordinator::Lifecycle::kInactive ||
                     state.tick.coordinator.tick != 0 ||
                     state.tick.coordinator.race_ends == 0))
           : (state.tick.coordinator.lifecycle !=
                  coordinator::Lifecycle::kInRace ||
              state.tick.coordinator.tick != completed_frames)))
    return reject(7);
  std::uint64_t replay_setter_overrides[protocol::kSetterCount]{};
  if (replay_mode) {
    for (std::uint32_t tick = 0; tick < completed_frames; ++tick) {
      replay_setter_overrides[protocol::kBrakeSetter] +=
          state.receipts[tick].brake_calls;
      replay_setter_overrides[protocol::kSteeringSetter] +=
          state.receipts[tick].steering_calls;
      replay_setter_overrides[protocol::kAcceleratorSetter] +=
          state.receipts[tick].accelerator_calls;
    }
  }
  for (std::uint32_t index = 0;
       index < protocol::kInstalledSetterCount; ++index) {
    if (evidence.setter_qualified_events[index] >
            evidence.setter_entries[index] ||
        evidence.setter_overrides[index] != replay_setter_overrides[index] ||
        (index < protocol::kInstalledSetterCount &&
         evidence.setter_qualified_events[index] != 0 &&
         evidence.setter_last_object[index] !=
             control.expected_setter_object) ||
        (index >= protocol::kInstalledSetterCount &&
         (evidence.setter_entries[index] != 0 ||
          evidence.setter_qualified_events[index] != 0 ||
          evidence.setter_last_object[index] != 0 ||
          evidence.setter_last_bits[index] != 0)))
      return reject(10 + index);
  }
  if (!g4::AxisBitsValid(evidence.setter_cache_bits[protocol::kBrakeSetter],
                         1.05f) ||
      !g4::AxisBitsValid(
          evidence.setter_cache_bits[protocol::kSteeringSetter], 1.0f) ||
      !g4::AxisBitsValid(
          evidence.setter_cache_bits[protocol::kAcceleratorSetter], 1.05f))
    return reject(20);
  for (std::uint32_t index = 0; index < completed_frames; ++index) {
    const coordinator::TickScratch& receipt = state.tick.receipts[index];
    const g4::TickReceiptV1& action = state.receipts[index];
    const bool zero_integration = sparse_updates && receipt.physics_interval_calls == 0;
    const auto expected_boundary = zero_integration
        ? (coordinator::kCompleteTickBoundaryMask & ~coordinator::kBoundaryPrePhysics) |
            coordinator::kBoundaryNoIntegration
        : coordinator::kCompleteTickBoundaryMask;
    if (receipt.tick != index ||
        receipt.session_id != receipt_session_id ||
        receipt.generation != receipt_generation ||
        receipt.boundary_flags != expected_boundary ||
        receipt.selected_packet_index !=
            (replay_mode ? index : coordinator::kNoPacket) ||
        receipt.begin_tid == 0 ||
        (zero_integration ? receipt.pre_physics_tid != 0 : receipt.pre_physics_tid == 0) ||
        receipt.final_writer_tid == 0 || receipt.end_tid == 0 ||
        (!zero_integration && receipt.physics_interval_calls == 0) || action.tick != index ||
        action.replay_packet_present != replay_mode ||
        action.fixed_delta_us != control.fixed_delta_us ||
        action.physics_interval_calls != receipt.physics_interval_calls ||
        action.setter_sequence_at_end < action.setter_sequence_at_begin ||
        action.setter_sequence_at_end - action.setter_sequence_at_begin !=
            static_cast<std::uint64_t>(action.brake_calls) +
                action.steering_calls + action.accelerator_calls)
      {
        std::printf(
            "G4_RECEIPT_REJECT tick=%u boundary=0x%x selected=%zu "
            "tids=%u,%u,%u,%u intervals=%u/%u action_tick=%" PRIu64
            " replay=%u delta=%" PRId64 " setters=%" PRIu64 ",%" PRIu64
            "/%u,%u,%u\n",
            index, receipt.boundary_flags, receipt.selected_packet_index,
            receipt.begin_tid, receipt.pre_physics_tid,
            receipt.final_writer_tid, receipt.end_tid,
            receipt.physics_interval_calls, action.physics_interval_calls,
            action.tick, action.replay_packet_present ? 1u : 0u,
            action.fixed_delta_us, action.setter_sequence_at_begin,
            action.setter_sequence_at_end, action.brake_calls,
            action.steering_calls, action.accelerator_calls);
        return reject(100 + index);
      }
  }
  for (std::uint32_t index = protocol::kInstalledSetterCount;
       index < protocol::kSetterCount; ++index)
    if (control.setter_slot[index] != 0 ||
        control.setter_original[index] != 0 ||
        control.setter_wrapper[index] != 0)
      return reject(200 + index);
  return true;
}

bool CancelledQueuedPredecessorControl(
    const protocol::Control& staged, const protocol::Evidence& evidence,
    const bridge::StateV1& state, protocol::Control* predecessor) {
  if (predecessor == nullptr || staged.enabled != 0 || staged.completed != 1 ||
      staged.active_helpers != 0 || staged.pending_mode != 0 ||
      staged.pending_state != 0 || staged.pending_generation != 0 ||
      staged.pending_reserved != 0 || staged.archived_generation == 0 ||
      staged.archived_generation >= UINT32_MAX ||
      staged.generation != staged.archived_generation + 1u ||
      staged.archived_frame_count == 0 ||
      staged.archived_frame_count > protocol::kMaximumFrames ||
      state.tick.coordinator.generation != staged.archived_generation)
    return false;

  *predecessor = staged;
  predecessor->generation =
      static_cast<std::uint32_t>(staged.archived_generation);
  predecessor->frame_limit =
      static_cast<std::uint32_t>(staged.archived_frame_count);

  const auto reason = static_cast<protocol::CompletionReason>(
      evidence.completion_reason);
  if (reason == protocol::CompletionReason::kRaceLifecycle ||
      reason == protocol::CompletionReason::kManualCheckpoint) {
    predecessor->mode =
        static_cast<std::uint32_t>(protocol::RunMode::kRecord);
    predecessor->completion_policy = static_cast<std::uint32_t>(
        protocol::CompletionPolicy::kRaceLifecycle);
    predecessor->replay_frame_count = 0;
    predecessor->replay_interval_count = 0;
  } else if (reason == protocol::CompletionReason::kFixedFrameLimit) {
    predecessor->mode =
        static_cast<std::uint32_t>(protocol::RunMode::kReplay);
    predecessor->completion_policy = static_cast<std::uint32_t>(
        protocol::CompletionPolicy::kFixedFrameLimit);
    predecessor->replay_frame_count = predecessor->frame_limit;
    predecessor->replay_interval_count = staged.archived_interval_count;
  } else {
    return false;
  }
  return true;
}

bool RecordedBuffersValid(int mem, const Runtime& runtime,
                           const protocol::Control& control,
                           const protocol::Evidence& evidence,
                           const bridge::StateV1& state,
                           bool require_recorded_native_identity = true) {
  const bool sparse_updates =
      control.fixed_delta_us == 8333 || control.fixed_delta_us == 6944;
  if (control.mode ==
      static_cast<std::uint32_t>(protocol::RunMode::kNeutral))
    return true;
  if (control.mode == static_cast<std::uint32_t>(protocol::RunMode::kReplay)) {
    if (control.replay_frame_count != control.frame_limit ||
        (control.replay_interval_count == 0 && !sparse_updates) ||
        control.replay_interval_count > protocol::kMaximumIntervalSamples ||
        !RecordingHashIsNonzero(control) ||
        (require_recorded_native_identity &&
         !NativePhysicsIdentityAlive(mem, control)))
      return false;
    std::uint64_t interval_total = 0;
    std::uint64_t injected_total = 0;
    std::uint64_t suppressed_total = 0;
    for (std::uint32_t index = 0; index < control.frame_limit; ++index) {
      recording::RecordingFrameV1 frame{};
      const std::uintptr_t address =
          runtime.replay_frames + sizeof(frame) * index;
      const auto& action = state.receipts[index];
      if (pread(mem, &frame, sizeof(frame), static_cast<off_t>(address)) !=
              static_cast<ssize_t>(sizeof(frame)) ||
          !g4::PhysicsRecordingFrameValid(frame) || frame.tick != index ||
          frame.monotonic_ns !=
              static_cast<std::uint64_t>(index) * control.fixed_delta_us *
                  1000ULL ||
          g4::FloatBits(frame.steering) != action.steering_bits ||
          g4::FloatBits(frame.brake) != action.brake_bits ||
          action.recorded_nitro_calls != frame.nitro_activation_count ||
          action.injected_nitro_calls != frame.nitro_activation_count ||
          action.natural_nitro_calls != 0)
        return false;
      interval_total += action.physics_interval_calls;
      injected_total += action.injected_nitro_calls;
      suppressed_total += action.suppressed_nitro_calls;
    }
    // AluTasV2's default keeps its independent Physics Interval override off.
    // Live getter calls pass through their dynamic natural result. The source
    // stream remains diagnostic evidence and its call count need not equal a
    // fresh process.
    if (interval_total != evidence.interval_calls ||
        injected_total != evidence.injected_nitro_calls ||
        suppressed_total != evidence.suppressed_nitro_calls)
      return false;
    std::uint64_t last_tick = UINT64_MAX;
    std::uint32_t next_ordinal = 0;
    for (std::uint32_t index = 0; index < control.replay_interval_count;
         ++index) {
      g4::IntervalSampleV1 sample{};
      const std::uintptr_t address =
          runtime.replay_intervals + sizeof(sample) * index;
      if (pread(mem, &sample, sizeof(sample), static_cast<off_t>(address)) !=
              static_cast<ssize_t>(sizeof(sample)) ||
          sample.tick >= control.frame_limit ||
          !g4::IntervalBitsValid(sample.output_bits))
        return false;
      if (sample.tick != last_tick) {
        if ((!sparse_updates && last_tick == UINT64_MAX && sample.tick != 0) ||
            (last_tick != UINT64_MAX &&
             (sample.tick <= last_tick ||
              (!sparse_updates && sample.tick != last_tick + 1))) ||
            sample.ordinal != 0)
          return false;
        last_tick = sample.tick;
        next_ordinal = 0;
      }
      if (sample.ordinal != next_ordinal++) return false;
    }
    return sparse_updates || last_tick + 1 == control.frame_limit;
  }
  const bool lifecycle_completion =
      control.completion_policy == static_cast<std::uint32_t>(
          protocol::CompletionPolicy::kRaceLifecycle);
  const std::uint32_t recorded_frame_count = lifecycle_completion
      ? static_cast<std::uint32_t>(evidence.recorded_frames)
      : control.frame_limit;
  if (control.mode != static_cast<std::uint32_t>(protocol::RunMode::kRecord) ||
      recorded_frame_count == 0 ||
      recorded_frame_count > control.frame_limit ||
      evidence.recorded_frames != recorded_frame_count ||
      (evidence.interval_records == 0 && !sparse_updates) ||
      evidence.interval_records > protocol::kMaximumIntervalSamples ||
      (!lifecycle_completion && !NativePhysicsIdentityAlive(mem, control)))
    return false;

  std::uint64_t interval_total = 0;
  for (std::uint32_t index = 0; index < recorded_frame_count; ++index) {
    recording::RecordingFrameV1 frame{};
    const std::uintptr_t address =
        runtime.recorded_frames + sizeof(frame) * index;
    if (pread(mem, &frame, sizeof(frame), static_cast<off_t>(address)) !=
            static_cast<ssize_t>(sizeof(frame)) ||
        !g4::PhysicsRecordingFrameValid(frame) || frame.tick != index ||
        frame.monotonic_ns !=
            static_cast<std::uint64_t>(index) * control.fixed_delta_us *
                1000ULL ||
        g4::FloatBits(frame.steering) != state.receipts[index].steering_bits ||
        g4::FloatBits(frame.brake) != state.receipts[index].brake_bits ||
        g4::FloatBits(frame.accelerator) != 0 ||
        frame.nitro_activation_count !=
            state.receipts[index].recorded_nitro_calls ||
        frame.skip_override_flags != g4::kPhysicsRecordSkipFlags)
      return false;
    interval_total += state.receipts[index].physics_interval_calls;
  }
  if (interval_total != evidence.interval_records) return false;

  std::uint64_t last_tick = 0;
  std::uint32_t next_ordinal = 0;
  for (std::uint64_t index = 0; index < evidence.interval_records; ++index) {
    g4::IntervalSampleV1 sample{};
    const std::uintptr_t address =
        runtime.recorded_intervals + sizeof(sample) * index;
    if (pread(mem, &sample, sizeof(sample), static_cast<off_t>(address)) !=
            static_cast<ssize_t>(sizeof(sample)) ||
        sample.tick >= recorded_frame_count ||
        !g4::IntervalBitsValid(sample.output_bits))
      return false;
    if (index == 0 || sample.tick != last_tick) {
      if (index != 0 && sample.tick <= last_tick) return false;
      last_tick = sample.tick;
      next_ordinal = 0;
    }
    if (sample.ordinal != next_ordinal++) return false;
  }
  return true;
}

std::size_t HookPatchSize(std::uint32_t index) {
  return index < protocol::kHookCount ? 16u : 0u;
}

bool BuildHookPatch(std::uint32_t index, const protocol::Control& control,
                    std::uint8_t patch[16]) {
  std::memset(patch, 0, 16);
  if (index >= protocol::kHookCount) return false;
  g2::AbsoluteJump(patch, control.wrapper[index]);
  return true;
}

bool TargetsMatch(int mem, const protocol::Control& control,
                   bool installed) {
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index) {
    const std::size_t size = HookPatchSize(index);
    std::uint8_t observed[16]{}, expected[16]{};
    if (installed) {
      if (!BuildHookPatch(index, control, expected)) return false;
    } else {
      std::memcpy(expected, kExpectedPrologues[index], sizeof(expected));
    }
    if (pread(mem, observed, size,
               static_cast<off_t>(control.target_entry[index])) !=
            static_cast<ssize_t>(size) ||
        std::memcmp(observed, expected, size) != 0)
      return false;
  }
  std::uint8_t lifecycle_observed[16]{}, lifecycle_expected[16]{};
  if (installed) {
    g2::AbsoluteJump(lifecycle_expected, control.lifecycle_wrapper);
  } else {
    std::memcpy(lifecycle_expected, kExpectedLifecyclePrologue,
                sizeof(lifecycle_expected));
  }
  if (control.lifecycle_target_entry == 0 ||
      pread(mem, lifecycle_observed, sizeof(lifecycle_observed),
            static_cast<off_t>(control.lifecycle_target_entry)) !=
          static_cast<ssize_t>(sizeof(lifecycle_observed)) ||
      std::memcmp(lifecycle_observed, lifecycle_expected,
                  sizeof(lifecycle_observed)) != 0)
    return false;
  for (std::uint32_t index = 0;
       index < protocol::kInstalledSetterCount; ++index) {
    std::uintptr_t observed = 0;
    const std::uintptr_t expected =
        installed ? control.setter_wrapper[index]
                  : control.setter_original[index];
    if (control.setter_slot[index] == 0 || expected == 0 ||
        !g2::ReadAt(mem, control.setter_slot[index], &observed) ||
        observed != expected)
      return false;
  }
  for (std::uint32_t index = protocol::kInstalledSetterCount;
       index < protocol::kSetterCount; ++index)
    if (control.setter_slot[index] != 0 ||
        control.setter_original[index] != 0 ||
        control.setter_wrapper[index] != 0)
      return false;
  return true;
}

bool PassiveTargetMatches(int mem, const protocol::Control& control) {
  return TargetsMatch(mem, control, false);
}

int FailG4(pid_t pid, g2::FrozenSet* frozen, bool uncertain,
           const char* stage, int code) {
  bool detached = true;
  if (frozen != nullptr) detached = g2::DetachAll(frozen);
  bool killed = false;
  if (uncertain || !detached) killed = KillUncertainProcess(pid);
  std::fprintf(stderr,
      "G4_TICK_COORDINATOR_CONTROLLER passed=0 stage=%s code=%d "
      "uncertain=%d detached=%d process_killed=%d errno=%d\n",
      stage, code, uncertain ? 1 : 0, detached ? 1 : 0,
      killed ? 1 : 0, errno);
  return code;
}

}  // namespace g4_controller_v1

int main(int argc, char** argv) {
  using namespace g4_controller_v1;
  std::uint64_t record_delta_us = 16667;
  if (const char* selected = std::getenv("A9TAS_RECORD_DELTA_US")) {
    if (!g2::ParseNumber(selected, 10, &record_delta_us) ||
        (record_delta_us != 16667 && record_delta_us != 8333 &&
         record_delta_us != 6944)) {
      std::fprintf(stderr, "invalid record timestep; expected 16667/8333/6944 us\n");
      return 2;
    }
  }
  Action action{};
  if (argc < 2 || !ParseAction(argv[1], &action)) {
    std::fprintf(stderr,
        "usage: %s ACTION PID START_TICKS GAME_BASE_HEX STEP_OWNER_HEX LIMIT "
        "[REPLAY_INPUT] OUTPUT [REPLAY_SPEED_1_TO_8] [REPLAY_BARRIER_0_TO_2] "
        "I_ACCEPT_G4_TICK_COORDINATOR_V1\n", argv[0]);
    return 2;
  }
  const bool rearm_action = IsRearmAction(action);
  const bool branch_rearm_action =
      action == Action::kRearmPausedReplayRecord ||
      action == Action::kWaitForCompletionPauseAndRearm;
  const bool queued_rearm_action =
      action == Action::kQueueArchivedRecord ||
      action == Action::kQueueArchivedReplay;
  const bool replay_action = action == Action::kArmReplay ||
      action == Action::kRearmArchivedReplay ||
      action == Action::kQueueArchivedReplay;
  const bool record_rearm_action =
      action == Action::kRearmArchivedRecord || branch_rearm_action ||
      action == Action::kQueueArchivedRecord;
  const bool replay_barrier_supplied = replay_action && argc == 12;
  const bool replay_speed_supplied = replay_action && (argc == 11 || argc == 12);
  const bool target_wait_action =
      action == Action::kWaitForCompletionAndPause ||
      action == Action::kWaitForCompletionPauseAndRearm ||
      action == Action::kWaitForCompletionAndStop ||
      action == Action::kWaitForRetryCountdownAndPause ||
      action == Action::kWaitForPendingActivation;
  const bool cancellation_signal_supplied = target_wait_action && argc == 10;
  const int acknowledgement_index = replay_action
      ? (replay_barrier_supplied ? 11 : replay_speed_supplied ? 10 : 9)
      : (cancellation_signal_supplied ? 9 : 8);
  const int output_index = replay_action ? 8 : 7;
  const char* replay_input = replay_action ? argv[7] : nullptr;
  const char* output_path = argc > output_index ? argv[output_index] : nullptr;
  const char* cancellation_signal = cancellation_signal_supplied ? argv[8] : nullptr;
  const bool argument_count_valid = replay_action ?
      (argc == 10 || argc == 11 || argc == 12) :
      (target_wait_action ?
       (argc == 9 || argc == 10) : argc == 9);
  if (!argument_count_valid ||
      std::strcmp(argv[acknowledgement_index], kAcknowledgement) != 0) {
    std::fprintf(stderr,
        "usage: %s ACTION PID START_TICKS GAME_BASE_HEX STEP_OWNER_HEX LIMIT "
        "[REPLAY_INPUT] OUTPUT [REPLAY_SPEED_1_TO_8] [REPLAY_BARRIER_0_TO_2] "
        "I_ACCEPT_G4_TICK_COORDINATOR_V1\n", argv[0]);
    return 2;
  }
  std::uint64_t pid_raw = 0, ticks = 0, game_base = 0, outer_owner = 0;
  std::uint64_t limit_raw = 0, replay_speed_raw = 1, replay_barrier_raw = 0;
  if (!g2::ParseNumber(argv[2], 10, &pid_raw) ||
      !g2::ParseNumber(argv[3], 10, &ticks) ||
      !g2::ParseNumber(argv[4], 16, &game_base) ||
      !g2::ParseNumber(argv[5], 16, &outer_owner) ||
      !g2::ParseNumber(argv[6], 10, &limit_raw) ||
      (replay_speed_supplied &&
       !g2::ParseNumber(argv[9], 10, &replay_speed_raw)) ||
      (replay_barrier_supplied &&
       !g2::ParseNumber(argv[10], 10, &replay_barrier_raw)) ||
      pid_raw == 0 || pid_raw > INT32_MAX || ticks == 0 || game_base == 0 ||
       outer_owner == 0 || limit_raw == 0 ||
       limit_raw > protocol::kMaximumFrames ||
      replay_speed_raw < protocol::kMinimumReplaySpeedFactor ||
       replay_speed_raw > protocol::kMaximumReplaySpeedFactor ||
       replay_barrier_raw > protocol::kReplayCompletionAtomicRecord ||
       output_path == nullptr || access(output_path, F_OK) == 0 ||
       (cancellation_signal_supplied &&
        !SafeCancellationSignalPath(cancellation_signal)) ||
       (replay_action &&
        (replay_input == nullptr || access(replay_input, R_OK) != 0)))
    return 2;
  const pid_t pid = static_cast<pid_t>(pid_raw);
  const std::uint32_t limit = static_cast<std::uint32_t>(limit_raw);
  if (!ReadBuildProfileBundle(kBuildProfilePath, &g_build_profile)) {
    std::fprintf(stderr,
                 "G4_BUILD_PROFILE passed=0 path=%s\n", kBuildProfilePath);
    return 16;
  }
  ReplayBundleV1 replay_bundle{};
  if (replay_action &&
      !ReadReplayBundle(replay_input, limit, &replay_bundle)) {
    std::fprintf(stderr,
                 "G4_REPLAY_LOAD passed=0 stage=source_bundle\n");
    return 15;
  }
  Runtime runtime{};
  if (!ResolveArtifacts(pid, ticks, game_base, &runtime,
                        action == Action::kStatus))
    return FailG4(pid, nullptr, false, "artifact_runtime", 3);

  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
  const int mem = open(mem_path,
      ((action == Action::kStatus ||
        action == Action::kWaitForCompletion ||
         action == Action::kWaitForCompletionAndStop ||
         action == Action::kWaitForRetryCountdownAndPause ||
         action == Action::kWaitForPendingActivation ||
         action == Action::kResumeStoppedAtPauseMenu ||
        action == Action::kPauseProbe ||
        action == Action::kWaitForReplayProgress ||
        action == Action::kPassiveStatus ||
        action == Action::kDiagnose || action == Action::kDumpRecord ||
        action == Action::kDumpReplayDiagnostic)
           ? O_RDONLY
           : O_RDWR) |
          O_CLOEXEC);
  if (mem < 0) return FailG4(pid, nullptr, false, "open_mem", 4);
  if (action != Action::kInstallPassive &&
      !RemoteBuildProfileMatches(mem, runtime)) {
    close(mem);
    return FailG4(pid, nullptr, false, "build_profile_binding", 4);
  }

  if (action == Action::kWaitForPendingActivation) {
    protocol::Control queued_control{};
    protocol::Evidence queued_evidence{};
    auto queued_state_storage = std::make_unique<bridge::StateV1>();
    auto& queued_state = *queued_state_storage;
    const bool receipt_read =
        ReadReceipt(mem, runtime, &queued_control, &queued_evidence,
                    &queued_state);
    const bool queue_shape = receipt_read &&
        StaticControlValid(queued_control, runtime, limit) &&
        queued_control.pending_state == 1 &&
        queued_evidence.first_error == 0;
    const bool initial_targets_match = receipt_read &&
        TargetsMatch(mem, queued_control, true);
    const bool initial_quiescent = queued_control.active_helpers == 0;
    const bool archived_queue = queue_shape && initial_targets_match &&
        initial_quiescent &&
        queued_control.enabled == 0 && queued_control.completed == 1 &&
        queued_control.pending_reserved == 0 &&
        queued_control.pending_generation == queued_control.generation &&
        (queued_control.pending_mode == static_cast<std::uint32_t>(
             protocol::RunMode::kRecord) ||
         queued_control.pending_mode == static_cast<std::uint32_t>(
             protocol::RunMode::kReplay)) &&
        queued_evidence.status == protocol::kQueued;
    // A direct-Retry queue is published before the current recording closes.
    // Java can begin this waiter as soon as it observes kComplete, while the
    // lifecycle wrapper that published completion is still unwinding.  It may
    // therefore see active_helpers != 0, and Retry may already have retired
    // the old race objects.  Neither is a rejected queue: this action is
    // read-only and the payload owns rebinding at the next qualified lifecycle
    // edge.  Accept the exact pending-generation shape here, then require the
    // full static control and freshly rebound TargetsMatch in
    // common_activation before reporting success.
    const bool active_retry_queue = queue_shape &&
        queued_control.pending_reserved == 1 &&
        queued_control.pending_mode == static_cast<std::uint32_t>(
            protocol::RunMode::kRecord) &&
        queued_control.pending_generation == queued_control.generation + 1u &&
        ((queued_control.enabled == 1 && queued_control.completed == 0 &&
          queued_evidence.status == protocol::kArmed) ||
         (queued_control.enabled == 0 && queued_control.completed == 1 &&
          queued_evidence.status == protocol::kComplete));
    const bool queued = archived_queue || active_retry_queue;
    const std::uint64_t prior_activations =
        queued_evidence.pending_activations;
    const bool already_active_retry = active_retry_queue &&
        queued_control.enabled == 1 && queued_control.completed == 0 &&
        prior_activations != 0 && initial_targets_match;
    const std::uint32_t expected_generation = already_active_retry
        ? queued_control.generation : active_retry_queue
        ? queued_control.pending_generation : queued_control.generation;
    bool activated = already_active_retry;
    bool rejected = !queued;
    bool cancelled = false;
    protocol::Control observed_control = queued_control;
    protocol::Evidence observed_evidence = queued_evidence;
    auto observed_storage = std::make_unique<bridge::StateV1>(queued_state);
    auto& observed_state = *observed_storage;
    if (queued && !activated) {
      // Five minutes is a user-facing wait for Retry, not a wall-clock guess
      // about the game.  Each sample reads only the fixed payload receipt.
      constexpr std::uint32_t kPollCount = 150000;
      for (std::uint32_t poll = 0; poll < kPollCount; ++poll) {
        if (!ReadReceipt(mem, runtime, &observed_control,
                         &observed_evidence, &observed_state)) {
          rejected = true;
          break;
        }
        if (observed_control.pending_state == 3 ||
            observed_evidence.first_error != 0 ||
            observed_evidence.status == protocol::kFault) {
          rejected = true;
          break;
        }
        const bool common_activation =
            observed_control.generation == expected_generation &&
            observed_control.enabled == 1 &&
            observed_control.completed == 0 &&
            observed_evidence.status == protocol::kArmed &&
            observed_evidence.pending_activations == prior_activations + 1 &&
            StaticControlValid(observed_control, runtime, limit) &&
            TargetsMatch(mem, observed_control, true);
        const bool pending_transition = active_retry_queue
            ? observed_control.pending_mode == static_cast<std::uint32_t>(
                  protocol::RunMode::kRecord) &&
              observed_control.pending_state == 1 &&
              observed_control.pending_generation ==
                  observed_control.generation + 1u &&
              observed_control.pending_reserved == 1
            : observed_control.pending_mode == 0 &&
              observed_control.pending_state == 0 &&
              observed_control.pending_generation == 0 &&
              observed_control.pending_reserved == 0;
        activated = common_activation && pending_transition;
        if (activated) break;
        // If Retry activation and host cancellation become visible in the
        // same poll, the game-owned lifecycle edge wins.  The host can then
        // cancel the newly queued following generation without pretending the
        // current generation never started.
        if (cancellation_signal_supplied &&
            access(cancellation_signal, F_OK) == 0) {
          cancelled = true;
          break;
        }
        usleep(2000);
      }
    }
    FILE* report = std::fopen(output_path, "wb");
    const bool written = report != nullptr &&
        std::fprintf(report,
            "G4_PENDING_WAIT passed=%d cancelled=%d rejected=%d "
            "generation=%u pending=%u,%u activations=%" PRIu64
            "->%" PRIu64 " status=%d error=%u initial_helpers=%u "
            "initial_targets=%d\n"
            "G10_CONTROLLER_FILE_RECEIPT action=wait-activation "
            "activated=%d cancelled=%d detached=1 process_killed=0\n",
            activated ? 1 : 0, cancelled ? 1 : 0, rejected ? 1 : 0,
            observed_control.generation, observed_control.pending_mode,
            observed_control.pending_state, prior_activations,
            observed_evidence.pending_activations, observed_evidence.status,
            observed_evidence.first_error, queued_control.active_helpers,
            initial_targets_match ? 1 : 0, activated ? 1 : 0,
            cancelled ? 1 : 0) > 0 &&
        std::fclose(report) == 0;
    if (report == nullptr) {}
    close(mem);
    return written && (activated || cancelled) ? 0 : 10;
  }

  if (action == Action::kWaitForRetryCountdownAndPause) {
    protocol::Control control{};
    protocol::Evidence evidence{};
    auto state_storage = std::make_unique<bridge::StateV1>();
    auto& state = *state_storage;
    std::uint32_t receipt_reject = 0;
    const bool archived_ready =
        ReadReceipt(mem, runtime, &control, &evidence, &state) &&
        StaticControlValid(control, runtime, limit) &&
        CompleteReceiptValid(control, evidence, state, limit, &receipt_reject) &&
        TargetsMatch(mem, control, evidence.status != protocol::kRestored);
    bool cancelled = false;
    bool countdown = false;
    bool pause_injected = false;
    bool relocated = false;
    std::uint32_t stable_samples = 0;
    std::uint32_t observed_lifecycle = UINT32_MAX;
    std::uintptr_t lifecycle_address = runtime.lifecycle_state;
    if (archived_ready) {
      // Stay inside one already-authorized controller process while the result
      // screen changes to Retry.  This removes the old 750 ms root-launch
      // cadence and the additional one-second foreground delay.
      constexpr std::uint32_t kPollCount = 24000;
      for (std::uint32_t poll = 0; poll < kPollCount; ++poll) {
        if (cancellation_signal_supplied &&
            access(cancellation_signal, F_OK) == 0) {
          cancelled = true;
          break;
        }
        bool valid = g2::ReadAt(mem, lifecycle_address, &observed_lifecycle);
        bool sample_relocated = false;
        // A full private-RW lifecycle scan is intentionally throttled; doing it
        // every 10 ms while the result screen is open would create avoidable
        // CPU and memory pressure.  Once a new object is found, its exact state
        // address is cached and the second proof sample costs one direct read.
        if ((!valid || observed_lifecycle != lifecycle::kCountdownState) &&
            poll % 10u == 0) {
          lifecycle::Resolution current_race{};
          const lifecycle::Profile lifecycle_profile = LifecycleProfile();
          if (lifecycle::ResolveCountdownObject(
                  pid, mem, runtime.call.game_base, lifecycle_profile,
                  &current_race)) {
            observed_lifecycle = current_race.selected.state;
            valid = true;
            sample_relocated = true;
            lifecycle_address = current_race.selected.state_address;
          }
        }
        if (valid && observed_lifecycle == lifecycle::kCountdownState) {
          relocated = relocated || sample_relocated;
          if (++stable_samples >= 2) {
            countdown = true;
            break;
          }
        } else {
          stable_samples = 0;
        }
        usleep(10000);
      }
    }
    if (countdown && !cancelled) {
      const pid_t child = fork();
      if (child == 0) {
        execl("/system/bin/sh", "sh", "/system/bin/input", "keyevent", "111",
              static_cast<char*>(nullptr));
        _exit(127);
      }
      int child_status = 0;
      const bool child_reaped = child > 0 &&
          waitpid(child, &child_status, 0) == child;
      pause_injected = child_reaped && WIFEXITED(child_status) &&
          WEXITSTATUS(child_status) == 0;
    }
    FILE* report = std::fopen(output_path, "wb");
    const bool written = report != nullptr &&
        std::fprintf(report,
            "G8_RETRY_WAIT passed=%d archived=%d lifecycle=%u valid=%d "
            "stable=%u relocated=%d pause=%d cancelled=%d reject=%u\n",
            archived_ready && countdown && pause_injected && !cancelled ? 1 : 0,
            archived_ready ? 1 : 0, observed_lifecycle,
            countdown ? 1 : 0, stable_samples, relocated ? 1 : 0,
            pause_injected ? 1 : 0, cancelled ? 1 : 0, receipt_reject) > 0 &&
        std::fclose(report) == 0;
    if (report == nullptr) {}
    close(mem);
    std::printf(
        "G4_RETRY_WAIT passed=%d archived=%d lifecycle=%u stable=%u "
        "pause=%d cancelled=%d written=%d\n",
        archived_ready && countdown && pause_injected && !cancelled && written ? 1 : 0,
        archived_ready ? 1 : 0, observed_lifecycle, stable_samples,
        pause_injected ? 1 : 0, cancelled ? 1 : 0, written ? 1 : 0);
    // Cancellation is an expected control-flow receipt, not a transport error.
    if (cancelled && written) return 0;
    return archived_ready && countdown && pause_injected && written ? 0 : 22;
  }

  if (action == Action::kResumeStoppedAtPauseMenu) {
    protocol::Control control{};
    protocol::Evidence evidence{};
    auto state_storage = std::make_unique<bridge::StateV1>();
    auto& state = *state_storage;
    std::uint32_t receipt_reject = 0;
    const bool terminal = ReadReceipt(mem, runtime, &control, &evidence, &state) &&
        StaticControlValid(control, runtime, limit) &&
        CompleteReceiptValid(control, evidence, state, limit, &receipt_reject) &&
        TargetsMatch(mem, control, true);
    bool process_stopped = false;
    char status_path[64]{};
    std::snprintf(status_path, sizeof(status_path), "/proc/%d/status", pid);
    if (FILE* process_status = std::fopen(status_path, "rb")) {
      char line[256]{};
      while (std::fgets(line, sizeof(line), process_status) != nullptr) {
        if (std::strncmp(line, "State:", 6) == 0) {
          const char* cursor = line + 6;
          while (*cursor == ' ' || *cursor == '\t') ++cursor;
          process_stopped = *cursor == 'T' || *cursor == 't';
          break;
        }
      }
      std::fclose(process_status);
    }
    bool input_staged = false;
    bool resumed = false;
    if (terminal && process_stopped) {
      const pid_t child = fork();
      if (child == 0) {
        execl("/system/bin/sh", "sh", "/system/bin/input", "keyevent", "111",
              static_cast<char*>(nullptr));
        _exit(127);
      }
      int child_status = 0;
      bool child_reaped = false;
      if (child > 0) {
        for (std::uint32_t poll = 0; poll < 100; ++poll) {
          const pid_t waited = waitpid(child, &child_status, WNOHANG);
          if (waited == child) { child_reaped = true; break; }
          if (waited < 0) break;
          usleep(1000);
        }
        resumed = kill(pid, SIGCONT) == 0;
        if (!child_reaped)
          child_reaped = waitpid(child, &child_status, 0) == child;
        input_staged = child_reaped && WIFEXITED(child_status) &&
            WEXITSTATUS(child_status) == 0;
      }
    }
    const bool passed = terminal && process_stopped && input_staged && resumed;
    FILE* report = std::fopen(output_path, "wb");
    const bool written = report != nullptr &&
        std::fprintf(report,
            "G8_HARD_RESUME passed=%d terminal=%d stopped=%d esc=%d resumed=%d "
            "ticks=%u reject=%u\n",
            passed ? 1 : 0, terminal ? 1 : 0, process_stopped ? 1 : 0,
            input_staged ? 1 : 0, resumed ? 1 : 0, state.receipt_count,
            receipt_reject) > 0 && std::fclose(report) == 0;
    if (report == nullptr) {}
    close(mem);
    std::printf("G8_HARD_RESUME passed=%d stopped=%d esc=%d resumed=%d ticks=%u\n",
                passed && written ? 1 : 0, process_stopped ? 1 : 0,
                input_staged ? 1 : 0, resumed ? 1 : 0, state.receipt_count);
    return passed && written ? 0 : 20;
  }

  if (action == Action::kWaitForReplayProgress) {
    // Replace host wall-clock guessing with the runtime's authoritative
    // packet receipt. This action is read-only and returns as soon as replay
    // has safely moved well beyond countdown/tick 0.
    constexpr std::uint32_t kMinimumProgressTicks = 120;
    constexpr std::uint32_t kProgressPollCount = 3000;
    protocol::Control control{};
    protocol::Evidence evidence{};
    auto state_storage = std::make_unique<bridge::StateV1>();
    auto& state = *state_storage;
    bool reached = false;
    for (std::uint32_t poll = 0; poll < kProgressPollCount; ++poll) {
      if (!ReadReceipt(mem, runtime, &control, &evidence, &state)) break;
      if (evidence.status == protocol::kFault || evidence.first_error != 0u)
        break;
      if (state.receipt_count >= kMinimumProgressTicks) {
        reached = true;
        break;
      }
      usleep(10000);
    }
    const bool valid = reached &&
        StaticControlValid(control, runtime, limit) &&
        control.enabled == 1u && control.completed == 0u &&
        evidence.status == protocol::kArmed &&
        evidence.first_error == protocol::kErrorNone &&
        state.tick.coordinator.lifecycle == coordinator::Lifecycle::kInRace &&
        state.tick.coordinator.failures == 0u &&
        state.receipt_count >= kMinimumProgressTicks &&
        state.receipt_count < limit &&
        state.tick.coordinator.ticks_published == state.receipt_count &&
        ReplayProgressCursorValid(
            state.receipt_count, state.tick.coordinator.tick,
            state.tick.coordinator.replay_head,
            state.tick.coordinator.tick_phase,
            state.tick.coordinator.active_packet_present,
            state.input_action.tick_open) &&
        TargetsMatch(mem, control, true) &&
        NativePhysicsIdentityAlive(mem, control);
    FILE* report = std::fopen(output_path, "wb");
    const bool written = report != nullptr &&
        std::fprintf(
            report,
            "G8_PROGRESS_WAIT passed=%d minimum=%u ticks=%u "
            "coordinator=%" PRIu64 " replay_head=%zu phase=%u error=%u\n",
            valid ? 1 : 0, kMinimumProgressTicks, state.receipt_count,
            state.tick.coordinator.tick, state.tick.coordinator.replay_head,
            static_cast<std::uint32_t>(state.tick.coordinator.tick_phase),
            evidence.first_error) > 0 && std::fclose(report) == 0;
    if (report == nullptr) {}
    close(mem);
    std::printf(
        "G8_PROGRESS_WAIT passed=%d minimum=%u ticks=%u "
        "replay_head=%zu error=%u\n",
        valid && written ? 1 : 0, kMinimumProgressTicks,
        state.receipt_count, state.tick.coordinator.replay_head,
        evidence.first_error);
    return valid && written ? 0 : 19;
  }

  if (action == Action::kPauseProbe) {
    protocol::Control before_control{}, after_control{};
    protocol::Evidence before_evidence{}, after_evidence{};
    auto before_storage = std::make_unique<bridge::StateV1>();
    auto after_storage = std::make_unique<bridge::StateV1>();
    auto& before_state = *before_storage;
    auto& after_state = *after_storage;
    const bool before_read = ReadReceipt(
        mem, runtime, &before_control, &before_evidence, &before_state);
    // One controller process owns both samples. This is a read-only wall-clock
    // stability proof and avoids repeated root invocations while the game is
    // paused. No payload command, ptrace stop or game write occurs here.
    constexpr useconds_t kPauseProbeDelayUs = 2000000;
    if (before_read) usleep(kPauseProbeDelayUs);
    const bool after_read = before_read && ReadReceipt(
        mem, runtime, &after_control, &after_evidence, &after_state);
    const auto active_snapshot_valid = [&](const protocol::Control& control,
                                           const protocol::Evidence& evidence,
                                           const bridge::StateV1& state) {
      return StaticControlValid(control, runtime, limit) &&
             control.enabled == 1u && control.completed == 0u &&
             evidence.status == protocol::kArmed &&
             evidence.first_error == protocol::kErrorNone &&
             state.tick.complete == 0u &&
             state.tick.coordinator.lifecycle ==
                 coordinator::Lifecycle::kInRace &&
             state.tick.coordinator.failures == 0u &&
             state.receipt_count != 0u &&
             state.receipt_count < limit &&
             state.tick.coordinator.ticks_published == state.receipt_count &&
             ReplayProgressCursorValid(
                 state.receipt_count, state.tick.coordinator.tick,
                 state.tick.coordinator.replay_head,
                 state.tick.coordinator.tick_phase,
                 state.tick.coordinator.active_packet_present,
                 state.input_action.tick_open) &&
             TargetsMatch(mem, control, true) &&
             NativePhysicsIdentityAlive(mem, control);
    };
    const bool before_valid = before_read && active_snapshot_valid(
        before_control, before_evidence, before_state);
    const bool after_valid = after_read && active_snapshot_valid(
        after_control, after_evidence, after_state);
    const bool stable = before_valid && after_valid &&
        std::memcmp(&before_control, &after_control,
                    sizeof(before_control)) == 0 &&
        std::memcmp(&before_state, &after_state, sizeof(before_state)) == 0 &&
        before_control.generation == after_control.generation &&
        before_state.receipt_count == after_state.receipt_count &&
        before_state.tick.coordinator.tick ==
            after_state.tick.coordinator.tick &&
        before_state.tick.coordinator.replay_head ==
            after_state.tick.coordinator.replay_head &&
        before_state.tick.tick_begin_entries ==
            after_state.tick.tick_begin_entries &&
        before_state.tick.pre_physics_entries ==
            after_state.tick.pre_physics_entries &&
        before_state.tick.physics_interval_calls ==
            after_state.tick.physics_interval_calls &&
        before_state.tick.final_writer_returns ==
            after_state.tick.final_writer_returns &&
        before_evidence.fixed_delta_writes ==
            after_evidence.fixed_delta_writes &&
        before_evidence.control_pair_writes ==
            after_evidence.control_pair_writes &&
        before_evidence.injected_nitro_calls ==
            after_evidence.injected_nitro_calls &&
        before_evidence.physics_equal_frames ==
            after_evidence.physics_equal_frames &&
        before_evidence.physics_corrected_frames ==
            after_evidence.physics_corrected_frames &&
        before_evidence.physics_correction_writes ==
            after_evidence.physics_correction_writes &&
        before_evidence.barrel_rbx_overrides ==
            after_evidence.barrel_rbx_overrides &&
        before_evidence.barrel_angular_overrides ==
            after_evidence.barrel_angular_overrides;
    FILE* report = std::fopen(output_path, "wb");
    const bool written = report != nullptr &&
        std::fprintf(
            report,
            "G8_PAUSE_PROBE passed=%d delay_ms=2000 generation=%u "
            "ticks=%u,%u coordinator=%" PRIu64 ",%" PRIu64
            " replay_head=%zu,%zu phase=%u,%u begin=%" PRIu64 ",%" PRIu64
            " interval=%" PRIu64 ",%" PRIu64
            " final=%" PRIu64 ",%" PRIu64
            " fixed_delta=%" PRIu64 ",%" PRIu64
            " control_writes=%" PRIu64 ",%" PRIu64
            " nitro=%" PRIu64 ",%" PRIu64
            " correction=%" PRIu64 ",%" PRIu64
            " error=%u,%u\n",
            stable ? 1 : 0, before_control.generation,
            before_state.receipt_count, after_state.receipt_count,
            before_state.tick.coordinator.tick,
            after_state.tick.coordinator.tick,
            before_state.tick.coordinator.replay_head,
            after_state.tick.coordinator.replay_head,
            static_cast<std::uint32_t>(
                before_state.tick.coordinator.tick_phase),
            static_cast<std::uint32_t>(
                after_state.tick.coordinator.tick_phase),
            before_state.tick.tick_begin_entries,
            after_state.tick.tick_begin_entries,
            before_state.tick.pre_physics_entries,
            after_state.tick.pre_physics_entries,
            before_state.tick.final_writer_returns,
            after_state.tick.final_writer_returns,
            before_evidence.fixed_delta_writes,
            after_evidence.fixed_delta_writes,
            before_evidence.control_pair_writes,
            after_evidence.control_pair_writes,
            before_evidence.injected_nitro_calls,
            after_evidence.injected_nitro_calls,
            before_evidence.physics_correction_writes,
            after_evidence.physics_correction_writes,
            before_evidence.first_error, after_evidence.first_error) > 0 &&
        std::fclose(report) == 0;
    if (report == nullptr) {}
    close(mem);
    std::printf(
        "G8_PAUSE_PROBE passed=%d delay_ms=2000 generation=%u "
        "ticks=%u,%u replay_head=%zu,%zu error=%u,%u\n",
        stable && written ? 1 : 0, before_control.generation,
        before_state.receipt_count, after_state.receipt_count,
        before_state.tick.coordinator.replay_head,
        after_state.tick.coordinator.replay_head,
        before_evidence.first_error, after_evidence.first_error);
    return stable && written ? 0 : 18;
  }

  bool target_pause_injected = false;
  bool target_completion_barrier_released = false;
  bool target_process_stopped = false;
  bool target_process_resumed = false;
  bool target_early_pause = false;
  bool wait_cancelled = false;
  bool target_completion_barrier = false;
  bool target_terminal_observed = false;
  bool target_atomic_handoff = false;
  std::uint32_t target_handoff_prefix_ticks = 0;
  bool record_checkpoint_barrier_held = false;
  std::uint32_t record_checkpoint_initial_ticks = 0;
  if (action == Action::kCheckpointAtNextClosedTick) {
    protocol::Control checkpoint_control{};
    protocol::Evidence checkpoint_evidence{};
    auto checkpoint_state_storage = std::make_unique<bridge::StateV1>();
    auto& checkpoint_state = *checkpoint_state_storage;
    const bool active_record =
        ReadCompletionReceipt(mem, runtime, &checkpoint_control,
                              &checkpoint_evidence) &&
        g2::ReadAt(mem, runtime.runtime, &checkpoint_state) &&
        StaticControlValid(checkpoint_control, runtime, limit) &&
        checkpoint_control.mode == static_cast<std::uint32_t>(
                                       protocol::RunMode::kRecord) &&
        checkpoint_control.completion_policy ==
            static_cast<std::uint32_t>(
                protocol::CompletionPolicy::kRaceLifecycle) &&
        checkpoint_control.enabled == 1 &&
        checkpoint_control.completed == 0 &&
        checkpoint_control.replay_speed_reserved ==
            protocol::kReplayCompletionBarrierDisabled &&
        checkpoint_evidence.status == protocol::kArmed &&
        checkpoint_evidence.first_error == 0;
    if (!active_record) {
      close(mem);
      return FailG4(pid, nullptr, false, "checkpoint_active_record", 10);
    }
    record_checkpoint_initial_ticks = checkpoint_state.receipt_count;
    const std::uintptr_t barrier_address = runtime.call.control +
        offsetof(protocol::Control, replay_speed_reserved);
    const std::uint32_t requested =
        protocol::kRecordCheckpointBarrierRequested;
    std::uint32_t observed_barrier = UINT32_MAX;
    const bool request_published =
        g2::WriteExact(mem, barrier_address, &requested, sizeof(requested)) &&
        g2::ReadAt(mem, barrier_address, &observed_barrier) &&
        observed_barrier == requested;
    if (!request_published) {
      close(mem);
      return FailG4(pid, nullptr, true, "checkpoint_request", 10);
    }

    // The payload acknowledges only after a new authoritative Tick has fully
    // closed.  Polling controls latency only; it cannot move the cut point.
    constexpr std::uint32_t kCheckpointPollCount = 5000;
    for (std::uint32_t poll = 0; poll < kCheckpointPollCount; ++poll) {
      protocol::Control waiting_control{};
      protocol::Evidence waiting_evidence{};
      auto waiting_state_storage = std::make_unique<bridge::StateV1>();
      auto& waiting_state = *waiting_state_storage;
      if (ReadCompletionReceipt(mem, runtime, &waiting_control,
                                &waiting_evidence) &&
          g2::ReadAt(mem, runtime.runtime, &waiting_state)) {
        if (waiting_evidence.status == protocol::kFault ||
            waiting_evidence.first_error != 0)
          break;
        if (waiting_control.replay_speed_reserved ==
                protocol::kRecordCheckpointBarrierHeld &&
            waiting_control.enabled == 1 &&
            waiting_control.completed == 0 &&
            waiting_control.active_helpers == 0 &&
            waiting_state.receipt_count > record_checkpoint_initial_ticks &&
            !waiting_state.input_action.tick_open &&
            waiting_state.tick.coordinator.tick_phase ==
                coordinator::TickPhase::kClosed) {
          record_checkpoint_barrier_held = true;
          break;
        }
      }
      usleep(1000);
    }
    if (!record_checkpoint_barrier_held) {
      const std::uint32_t disabled =
          protocol::kReplayCompletionBarrierDisabled;
      observed_barrier = UINT32_MAX;
      const bool released =
          g2::WriteExact(mem, barrier_address, &disabled, sizeof(disabled)) &&
          g2::ReadAt(mem, barrier_address, &observed_barrier) &&
          observed_barrier == disabled;
      close(mem);
      return FailG4(pid, nullptr, !released,
                    "checkpoint_boundary_timeout", 10);
    }

    // Queue pause while the completed Tick is synchronously held.  The later
    // SealPausedRecord command runs with all threads stopped and therefore sees
    // a closed cursor rather than deleting a partial Tick.
    const pid_t child = fork();
    if (child == 0) {
      execl("/system/bin/sh", "sh", "/system/bin/input", "keyevent", "111",
            static_cast<char*>(nullptr));
      _exit(127);
    }
    int child_status = 0;
    const bool child_reaped = child > 0 &&
        waitpid(child, &child_status, 0) == child;
    target_pause_injected = child > 0 && child_reaped &&
        WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;
    if (!target_pause_injected) {
      const std::uint32_t disabled =
          protocol::kReplayCompletionBarrierDisabled;
      observed_barrier = UINT32_MAX;
      const bool released =
          g2::WriteExact(mem, barrier_address, &disabled, sizeof(disabled)) &&
          g2::ReadAt(mem, barrier_address, &observed_barrier) &&
          observed_barrier == disabled;
      close(mem);
      return FailG4(pid, nullptr, !released,
                    "checkpoint_pause_delivery", 10);
    }
  }
  if (action == Action::kWaitForCompletion ||
      action == Action::kWaitForCompletionAndPause ||
      action == Action::kWaitForCompletionPauseAndRearm ||
      action == Action::kWaitForCompletionAndStop) {
    // Keep one read-only /proc/PID/mem descriptor open while the user drives
    // and triggers Retry.  This avoids repeated root invocations/toasts and
    // never freezes, patches or writes the game.  The payload itself owns the
    // lifecycle transition and publishes the terminal receipt.
    // One poll is roughly 1 ms.  This must be 180000 (180 seconds), not 18000:
    // a normal full lap is already longer than 18 seconds.
    constexpr std::uint32_t kPollCount = 180000;
    constexpr std::uint32_t kMaximumConsecutiveReadFailures = 1000;
    std::uint32_t completion_read_failures = 0;
    std::uint32_t consecutive_completion_read_failures = 0;
    std::uint32_t maximum_consecutive_completion_read_failures = 0;
    bool terminal = false;
    for (std::uint32_t poll = 0; poll < kPollCount; ++poll) {
      if (cancellation_signal_supplied &&
          access(cancellation_signal, F_OK) == 0) {
        wait_cancelled = true;
        break;
      }
      protocol::Control waiting_control{};
      protocol::Evidence waiting_evidence{};
      if (!ReadCompletionReceipt(mem, runtime, &waiting_control,
                                 &waiting_evidence)) {
        ++completion_read_failures;
        ++consecutive_completion_read_failures;
        maximum_consecutive_completion_read_failures = std::max(
            maximum_consecutive_completion_read_failures,
            consecutive_completion_read_failures);
        // A vanished process is terminal transport failure.  A live process is
        // allowed one full second of consecutive read disturbance before the
        // bounded waiter gives up; isolated failures never end the replay.
        errno = 0;
        const bool process_gone = kill(pid, 0) != 0 && errno == ESRCH;
        if (process_gone || consecutive_completion_read_failures >=
                                kMaximumConsecutiveReadFailures)
          break;
        usleep(1000);
        continue;
      }
      consecutive_completion_read_failures = 0;
      if (action == Action::kWaitForCompletionPauseAndRearm) {
        const bool waiting_for_atomic_handoff =
            waiting_control.mode == static_cast<std::uint32_t>(
                                        protocol::RunMode::kReplay) &&
            waiting_control.replay_speed_reserved ==
                protocol::kReplayCompletionAtomicRecord &&
            waiting_control.generation != 0 &&
            waiting_control.generation != UINT32_MAX &&
            waiting_control.pending_mode == 0u &&
            waiting_control.pending_state == 0u &&
            waiting_control.pending_generation == 0u &&
            waiting_control.pending_reserved == 0u;
        if (waiting_for_atomic_handoff && target_handoff_prefix_ticks == 0)
          target_handoff_prefix_ticks = waiting_control.replay_frame_count;
        const bool atomic_handoff_complete =
            waiting_control.mode == static_cast<std::uint32_t>(
                                        protocol::RunMode::kRecord) &&
            waiting_control.replay_speed_reserved ==
                protocol::kReplayCompletionBarrierDisabled &&
            waiting_control.pending_mode == 0 &&
            waiting_control.pending_state == 0 &&
            waiting_control.pending_generation == 0 &&
            waiting_control.pending_reserved == 0 &&
            waiting_control.enabled == 1 && waiting_control.completed == 0 &&
            waiting_evidence.status == protocol::kArmed &&
            waiting_evidence.first_error == 0 &&
            (waiting_evidence.flags &
             protocol::kReplayRecordHandoffComplete) != 0 &&
            waiting_control.archived_frame_count != 0 &&
            waiting_evidence.recorded_frames >=
                waiting_control.archived_frame_count;
        if (atomic_handoff_complete) {
          if (target_handoff_prefix_ticks == 0)
            target_handoff_prefix_ticks = static_cast<std::uint32_t>(
                waiting_control.archived_frame_count);
          target_atomic_handoff = true;
          target_completion_barrier = true;
          terminal = true;
          target_terminal_observed = true;
          break;
        }
        if (!waiting_for_atomic_handoff) {
          std::fprintf(stderr,
                       "G4_WAIT terminal=0 atomic_handoff=0 mode=%u "
                       "barrier=%u generation=%u pending=%u,%u,%u,%u "
                       "enabled=%u completed=%u status=%d error=%u "
                       "ticks=%u recorded=%llu flags=0x%llx "
                       "diagnostic=0x%llx g3=%d g4=%d\n",
                       waiting_control.mode,
                       waiting_control.replay_speed_reserved,
                       waiting_control.generation,
                       waiting_control.pending_mode,
                       waiting_control.pending_generation,
                       waiting_control.pending_state,
                       waiting_control.pending_reserved,
                       waiting_control.enabled,
                       waiting_control.completed,
                       waiting_evidence.status,
                       waiting_evidence.first_error,
                       static_cast<unsigned>(waiting_evidence.published_ticks),
                       static_cast<unsigned long long>(
                           waiting_evidence.recorded_frames),
                       static_cast<unsigned long long>(waiting_evidence.flags),
                       static_cast<unsigned long long>(
                           waiting_evidence.reserved[0]),
                       waiting_evidence.last_g3_result,
                       waiting_evidence.last_g4_result);
          break;
        }
      } else if (action == Action::kWaitForCompletionAndPause) {
        target_completion_barrier =
            waiting_control.replay_speed_reserved ==
                protocol::kReplayCompletionBarrierEnabled;
        if (!target_completion_barrier) {
          std::fprintf(stderr,
                       "G4_WAIT terminal=0 replay_completion_barrier=0\n");
          break;
        }
      }
      // Completion is published from inside a hook.  Do not freeze or inspect
      // the terminal receipt until that outer wrapper has dropped its helper
      // reference; otherwise a perfectly valid race end can be observed as
      // `completed=1, active_helpers=1` and be rejected by the host.
      if ((waiting_control.completed != 0 &&
           waiting_control.active_helpers == 0) ||
          waiting_evidence.status == protocol::kFault) {
        terminal = true;
        target_terminal_observed = true;
        break;
      }
      // This loop owns one already-open /proc/PID/mem fd and performs no root
      // process launch. Accuracy no longer depends on this polling cadence:
      // the payload holds the exact terminal replay Tick synchronously.
      usleep(1000);
    }
    if (!terminal && !wait_cancelled)
      std::fprintf(stderr,
                   "G4_WAIT terminal=0 timeout_ms=180000\n");
    if (completion_read_failures != 0)
      std::fprintf(stderr,
                   "G4_WAIT_READ failures=%u max_consecutive=%u\n",
                   completion_read_failures,
                   maximum_consecutive_completion_read_failures);
    if (terminal &&
        (action == Action::kWaitForCompletionAndPause ||
         action == Action::kWaitForCompletionPauseAndRearm) &&
        !target_pause_injected) {
      // The payload is holding the exact terminal Tick. Queue ESC while that
      // barrier is closed; replay->record rearm releases it only after suffix
      // recording is authoritative.
      const pid_t child = fork();
      if (child == 0) {
        execl("/system/bin/sh", "sh", "/system/bin/input", "keyevent", "111",
              static_cast<char*>(nullptr));
        _exit(127);
      }
      int child_status = 0;
      const bool child_reaped = child > 0 &&
          waitpid(child, &child_status, 0) == child;
      const bool input_injected = child > 0 && child_reaped &&
          WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;
      target_pause_injected = input_injected;
      if (target_pause_injected &&
          action == Action::kWaitForCompletionAndPause) {
        // Inspection is a terminal replay, not a replay->record handoff.  The
        // payload holds the exact completed Tick while ESC is queued, but no
        // later rearm exists to clear completed.  Explicitly lower only the
        // completion-barrier field after the input command has completed so
        // the game thread can consume ESC and enter its ordinary pause menu.
        // Keep completed/status and all sealed receipts intact for validation.
        const std::uint32_t disabled =
            protocol::kReplayCompletionBarrierDisabled;
        std::uint32_t observed = UINT32_MAX;
        const std::uintptr_t barrier_address = runtime.call.control +
            offsetof(protocol::Control, replay_speed_reserved);
        target_completion_barrier_released =
            g2::WriteExact(mem, barrier_address, &disabled,
                           sizeof(disabled)) &&
            g2::ReadAt(mem, barrier_address, &observed) &&
            observed == disabled;
      }
    } else if (terminal && action == Action::kWaitForCompletionAndStop) {
      // Exact-cut mode: freeze immediately at the authoritative terminal Tick
      // and deliberately leave the game stopped.  A separate, user-triggered
      // resume-stop transaction queues ESC, resumes this exact process, and
      // lets the host rearm the suffix while the pause menu is visible.
      target_process_stopped = kill(pid, SIGSTOP) == 0;
      target_pause_injected = target_process_stopped;
    }
    // Cancellation deliberately falls through to the ordinary status writer.
    // The caller needs the exact last closed replay cursor in order to convert
    // an interrupted prefix load into a suffix recording without guessing.
  }

  if (action == Action::kWaitForCompletionPauseAndRearm &&
      (!target_completion_barrier || !target_atomic_handoff ||
       !target_pause_injected ||
       wait_cancelled)) {
    if (wait_cancelled) {
      FILE* report = std::fopen(output_path, "wb");
      const bool written = report != nullptr &&
          std::fprintf(report,
                       "G4_HANDOFF action=23 cancelled=1 target_barrier=%d "
                       "target_pause=%d\n",
                       target_completion_barrier ? 1 : 0,
                       target_pause_injected ? 1 : 0) > 0 &&
          std::fclose(report) == 0;
      if (report == nullptr) {}
      close(mem);
      return written ? 0 : 10;
    }
    close(mem);
    // A completed replay with its payload barrier still closed cannot safely
    // be returned to the caller.  Earlier cancellation has not mutated the
    // game; a terminal pause-delivery failure is uncertain and must not resume.
    const bool terminal_barrier_uncertain =
        target_terminal_observed && target_completion_barrier;
    return FailG4(pid, nullptr, terminal_barrier_uncertain,
                  "handoff_pause_delivery",
                  10);
  }

  if (action == Action::kWaitForCompletionPauseAndRearm) {
    protocol::Control control{};
    protocol::Evidence evidence{};
    const bool read = ReadCompletionReceipt(mem, runtime, &control, &evidence);
    const bool handoff_valid = read && target_atomic_handoff &&
        target_handoff_prefix_ticks != 0 &&
        StaticControlValid(control, runtime, limit) &&
        TargetsMatch(mem, control, true) && NativePhysicsIdentityAlive(mem, control) &&
        control.mode == static_cast<std::uint32_t>(protocol::RunMode::kRecord) &&
        control.completion_policy == static_cast<std::uint32_t>(
            protocol::CompletionPolicy::kRaceLifecycle) &&
        control.enabled == 1 && control.completed == 0 &&
        control.pending_mode == 0 && control.pending_state == 0 &&
        control.pending_generation == 0 && control.pending_reserved == 0 &&
        control.archived_frame_count == target_handoff_prefix_ticks &&
        evidence.status == protocol::kArmed && evidence.first_error == 0 &&
        (evidence.flags & protocol::kReplayRecordHandoffComplete) != 0 &&
        evidence.recorded_frames >= target_handoff_prefix_ticks;
    FILE* report = handoff_valid ? std::fopen(output_path, "wb") : nullptr;
    const bool written = report != nullptr &&
        std::fprintf(report,
                     "G4_ACTION action=23 pid=%d limit=%u status=%d ticks=%" PRIu64
                     " prefix_ticks=%u detached=1 handoff=1 pause=1\n",
                     pid, limit, evidence.status, evidence.recorded_frames,
                     target_handoff_prefix_ticks) > 0 &&
        std::fclose(report) == 0;
    if (report == nullptr) {}
    close(mem);
    std::printf(
        "G4_ATOMIC_HANDOFF passed=%d prefix_ticks=%u recorded_ticks=%" PRIu64
        " pause=%d\n",
        handoff_valid && written ? 1 : 0, target_handoff_prefix_ticks,
        evidence.recorded_frames, target_pause_injected ? 1 : 0);
    return handoff_valid && written ? 0 : 10;
  }

  if (action == Action::kStatus ||
      action == Action::kWaitForCompletion ||
      action == Action::kWaitForCompletionAndPause ||
      action == Action::kWaitForCompletionAndStop) {
    protocol::Control control{};
    protocol::Evidence evidence{};
    auto state_storage = std::make_unique<bridge::StateV1>();
    auto& state = *state_storage;
    std::uint32_t receipt_reject = 0;
    const bool read = ReadReceipt(mem, runtime, &control, &evidence, &state);
    const bool static_valid =
        read && StaticControlValid(control, runtime, limit);
    const bool receipt_valid =
        read && CompleteReceiptValid(control, evidence, state, limit,
                                     &receipt_reject);
    const bool buffers_valid =
        read && RecordedBuffersValid(mem, runtime, control, evidence, state);
    const bool restored_receipt =
        read && evidence.status == protocol::kRestored;
    const bool targets_valid =
        read && TargetsMatch(mem, control, !restored_receipt);
    const bool native_valid = read &&
        (control.completion_policy == static_cast<std::uint32_t>(
             protocol::CompletionPolicy::kRaceLifecycle) ||
         NativePhysicsIdentityAlive(mem, control));
    const bool pause_release_valid =
        action != Action::kWaitForCompletionAndPause ||
        (target_pause_injected && target_completion_barrier_released);
    const bool complete = static_valid && receipt_valid && buffers_valid &&
                          targets_valid && native_valid &&
                          (action != Action::kWaitForCompletionAndPause ||
                           target_completion_barrier) &&
                          pause_release_valid;
    // Evidence.last_lifecycle_state is intentionally frozen with the sealed
    // recording. The race-owned lifecycle object can also retire immediately
    // after state 9, so the originally bound address is not a durable Retry
    // observer. Use the address published by the installed payload: this
    // standalone status process has not run ResolveInstallObjects, so its
    // Runtime::lifecycle_state can be zero. Only scan when the bound running
    // state cannot answer the query (or a sealed race may have retired).
    std::uint32_t live_lifecycle = UINT32_MAX;
    bool live_lifecycle_valid =
        static_valid && control.lifecycle_state_address != 0 &&
        g2::ReadAt(mem, control.lifecycle_state_address, &live_lifecycle);
    bool live_lifecycle_relocated = false;
    const bool bound_session_active = static_valid && control.enabled == 1u &&
        control.completed == 0u && evidence.status == protocol::kArmed;
    if (a9tas::status_observation_v1::ShouldRelocateLifecycle(
            bound_session_active, live_lifecycle_valid, live_lifecycle)) {
      lifecycle::Resolution current_race{};
      const lifecycle::Profile lifecycle_profile = LifecycleProfile();
      if (lifecycle::ResolveCountdownObject(
              pid, mem, runtime.call.game_base, lifecycle_profile,
              &current_race)) {
        live_lifecycle = current_race.selected.state;
        live_lifecycle_valid = true;
        live_lifecycle_relocated = true;
      }
    }
    std::uint64_t pre_first_setters = 0;
    std::uint64_t between_tick_setters = 0;
    std::uint64_t open_brake = 0, open_steering = 0, open_accelerator = 0;
    if (read && state.receipt_count != 0 &&
        state.receipt_count <= protocol::kMaximumFrames) {
      pre_first_setters = state.receipts[0].setter_sequence_at_begin;
      for (std::uint32_t index = 0; index < state.receipt_count; ++index) {
        const auto& receipt = state.receipts[index];
        open_brake += receipt.brake_calls;
        open_steering += receipt.steering_calls;
        open_accelerator += receipt.accelerator_calls;
        if (index != 0) {
          const std::uint64_t prior_end =
              state.receipts[index - 1].setter_sequence_at_end;
          if (receipt.setter_sequence_at_begin >= prior_end)
            between_tick_setters +=
                receipt.setter_sequence_at_begin - prior_end;
        }
      }
    }
    FILE* report = std::fopen(output_path, "wb");
    const bool written = report != nullptr &&
        std::fprintf(report,
            "G4_STATUS complete=%d ticks=%u begin=%" PRIu64
            " interval=%" PRIu64 " interval_calls=%" PRIu64
            " final=%" PRIu64 " end=%" PRIu64
            " nitro=%" PRIu64 ",%" PRIu64 ",%" PRIu64
            " setters=%" PRIu64 ",%" PRIu64 ",%" PRIu64
            "/%" PRIu64 ",%" PRIu64 ",%" PRIu64
            "/%" PRIu64 ",%" PRIu64 ",%" PRIu64
            " setter_total=%u,%u,%u/%" PRIu64
            " setter_phase=%" PRIu64 "/%" PRIu64 ",%" PRIu64
            ",%" PRIu64 "/%" PRIu64
            " ignored=%" PRIu64
            " qualified_tids=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            "/%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
            " core_results=%d,%d error=%u"
            " checks=%d,%d,%d,%d,%d,%d reject=%u"
            " control=%u,%u evidence=%d,0x%016" PRIx64
             " records=%" PRIu64 ",%" PRIu64 ",%" PRIu64
             " physics=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
             " barrel=%" PRIu64 ",%" PRIu64 "/%" PRIu64 ",%" PRIu64
             "/%" PRIu64 ",%" PRIu64
             " barrel_random=%" PRIu64 ",%" PRIu64 ",%" PRIu64
             " completion=%u,%u lifecycle=%u terminal_lifecycle=%u"
             " live_lifecycle=%u"
             " live_lifecycle_valid=%d live_lifecycle_relocated=%d"
             " coordinator=%u,%" PRIu64 ",%u,%" PRIu64
             " fault_context=0x%" PRIx64 " fault_tick=%" PRIu64
             " coordinator_result=%d coordinator_intervals=%" PRIu64
             " interval_probe=%" PRIu64 ",%" PRIu64 ",0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64
             " tick_config=%u,%u idle_final=%" PRIu64
             " fast_replay=%u,%" PRIu64 ",%" PRIu64
             " target_barrier=%d target_pause=%d target_release=%d target_stop=%d target_resume=%d early_pause=%d cancelled=%d\n",
            complete ? 1 : 0, state.receipt_count,
            state.tick.tick_begin_entries, state.tick.pre_physics_entries,
            state.tick.physics_interval_calls,
            evidence.qualified_events[1], evidence.qualified_events[2],
            evidence.natural_nitro_calls, evidence.suppressed_nitro_calls,
            evidence.injected_nitro_calls,
            evidence.setter_entries[0], evidence.setter_entries[1],
            evidence.setter_entries[2],
            evidence.setter_qualified_events[0],
            evidence.setter_qualified_events[1],
            evidence.setter_qualified_events[2],
            evidence.setter_overrides[0], evidence.setter_overrides[1],
            evidence.setter_overrides[2],
            state.input_action.setter_cache.brake_calls,
            state.input_action.setter_cache.steering_calls,
            state.input_action.setter_cache.accelerator_calls,
            state.input_action.setter_cache.event_sequence,
            pre_first_setters, open_brake, open_steering, open_accelerator,
            between_tick_setters,
            evidence.ignored_events,
            evidence.first_tid[0], evidence.first_tid[1],
            evidence.first_tid[2], evidence.first_tid[3],
            evidence.last_tid[0], evidence.last_tid[1],
            evidence.last_tid[2], evidence.last_tid[3],
            state.last_g3_result, state.last_g4_result,
            evidence.first_error, read ? 1 : 0, static_valid ? 1 : 0,
            receipt_valid ? 1 : 0, buffers_valid ? 1 : 0,
            targets_valid ? 1 : 0, native_valid ? 1 : 0,
            receipt_reject,
            control.enabled, control.completed, evidence.status, evidence.flags,
            evidence.recorded_frames, evidence.interval_records,
            evidence.fixed_delta_writes, evidence.physics_equal_frames,
             evidence.physics_corrected_frames,
             evidence.physics_correction_writes,
             evidence.physics_skipped_frames,
             evidence.qualified_events[protocol::kBarrelRollHook],
             evidence.qualified_events[protocol::kBarrelYawHook],
             evidence.barrel_rbx_overrides,
             evidence.barrel_angular_overrides,
             evidence.barrel_rbx_records,
             evidence.barrel_angular_records,
             evidence.barrel_random_bool_calls,
             evidence.barrel_random_lerp_calls,
             evidence.barrel_random_resets, control.completion_policy,
             evidence.completion_reason, evidence.last_lifecycle_state,
             static_cast<std::uint32_t>(evidence.reserved[0] >> 56u),
             live_lifecycle, live_lifecycle_valid ? 1 : 0,
             live_lifecycle_relocated ? 1 : 0,
             state.tick.complete,
            state.tick.coordinator.ticks_published,
            static_cast<std::uint32_t>(state.tick.coordinator.tick_phase),
            state.tick.coordinator.failures,
            evidence.reserved[0], evidence.last_tick,
            static_cast<std::int32_t>(state.tick.coordinator.last_result),
            state.tick.coordinator.physics_interval_calls,
            evidence.wrapper_entries[protocol::kTickHook],
            evidence.qualified_events[protocol::kTickHook],
            evidence.last_object[protocol::kTickHook],
            control.expected_interval_owner,
            evidence.last_vptr[protocol::kTickHook],
            control.expected_interval_owner_vptr,
            control.mode, control.fixed_delta_us,
            state.tick.idle_final_writer_returns,
            control.replay_speed_factor,
            evidence.wrapper_entries[protocol::kLogicDispatcherHook],
            evidence.qualified_events[protocol::kLogicDispatcherHook],
             target_completion_barrier ? 1 : 0,
             target_pause_injected ? 1 : 0,
             target_completion_barrier_released ? 1 : 0,
             target_process_stopped ? 1 : 0,
             target_process_resumed ? 1 : 0,
             target_early_pause ? 1 : 0,
             wait_cancelled ? 1 : 0) > 0 &&
        std::fclose(report) == 0;
    if (report == nullptr) {}
    close(mem);
    std::printf(
        "G4_TICK_COORDINATOR_STATUS passed=%d complete=%d ticks=%u "
        "begin=%" PRIu64 " interval=%" PRIu64
        " interval_calls=%" PRIu64 " final=%" PRIu64
        " end=%" PRIu64
        " entries=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        " nitro=%" PRIu64 ",%" PRIu64 ",%" PRIu64
        " setters=%" PRIu64 ",%" PRIu64 ",%" PRIu64
        "/%" PRIu64 ",%" PRIu64 ",%" PRIu64
        "/%" PRIu64 ",%" PRIu64 ",%" PRIu64
        " setter_cache=%08x,%08x,%08x"
        " setter_total=%u,%u,%u/%" PRIu64
        " setter_phase=%" PRIu64 "/%" PRIu64 ",%" PRIu64
        ",%" PRIu64 "/%" PRIu64
        " ignored=%" PRIu64 " lifecycle=%u live_lifecycle=%u"
        " live_lifecycle_valid=%d live_lifecycle_relocated=%d "
        "objects=0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64
        ",0x%" PRIx64
        " vptrs=0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64
        ",0x%" PRIx64
        " qualified_tids=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        "/%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
        " core_results=%d,%d error=%u fault_context=0x%" PRIx64
        " fault_tick=%" PRIu64
        " checks=%d,%d,%d,%d,%d,%d reject=%u"
        " control=%u,%u evidence=%d,0x%016" PRIx64
        " records=%" PRIu64 ",%" PRIu64 ",%" PRIu64
        " physics=%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
         " barrel=%" PRIu64 ",%" PRIu64 "/%" PRIu64 ",%" PRIu64
         "/%" PRIu64 ",%" PRIu64
         " barrel_random=%" PRIu64 ",%" PRIu64 ",%" PRIu64
         " completion=%u,%u terminal_lifecycle=%u"
        " fast_replay=%u,%" PRIu64 ",%" PRIu64
        " coordinator=%u,%" PRIu64 ",%u,%" PRIu64 "\n",
        complete && written ? 1 : 0, complete ? 1 : 0, state.receipt_count,
        state.tick.tick_begin_entries, state.tick.pre_physics_entries,
        state.tick.physics_interval_calls,
        evidence.qualified_events[1], evidence.qualified_events[2],
        evidence.wrapper_entries[0], evidence.wrapper_entries[1],
        evidence.wrapper_entries[2], evidence.wrapper_entries[3],
        evidence.wrapper_entries[4], evidence.wrapper_entries[5],
        evidence.wrapper_entries[6],
        evidence.natural_nitro_calls, evidence.suppressed_nitro_calls,
        evidence.injected_nitro_calls,
        evidence.setter_entries[0], evidence.setter_entries[1],
        evidence.setter_entries[2],
        evidence.setter_qualified_events[0],
        evidence.setter_qualified_events[1],
        evidence.setter_qualified_events[2],
        evidence.setter_overrides[0], evidence.setter_overrides[1],
        evidence.setter_overrides[2],
        evidence.setter_cache_bits[0], evidence.setter_cache_bits[1],
        evidence.setter_cache_bits[2],
        state.input_action.setter_cache.brake_calls,
        state.input_action.setter_cache.steering_calls,
        state.input_action.setter_cache.accelerator_calls,
        state.input_action.setter_cache.event_sequence,
        pre_first_setters, open_brake, open_steering, open_accelerator,
        between_tick_setters,
        evidence.ignored_events, evidence.last_lifecycle_state,
        live_lifecycle, live_lifecycle_valid ? 1 : 0,
        live_lifecycle_relocated ? 1 : 0,
        evidence.last_object[0], evidence.last_object[1],
        evidence.last_object[2], evidence.last_object[3],
        evidence.last_vptr[0], evidence.last_vptr[1],
        evidence.last_vptr[2], evidence.last_vptr[3],
        evidence.first_tid[0], evidence.first_tid[1],
        evidence.first_tid[2], evidence.first_tid[3],
        evidence.last_tid[0], evidence.last_tid[1],
        evidence.last_tid[2], evidence.last_tid[3],
        state.last_g3_result, state.last_g4_result,
        evidence.first_error,
        static_cast<std::uint64_t>(
            evidence.reserved[0] & 0x00ffffffffffffffULL),
        evidence.last_tick,
        read ? 1 : 0, static_valid ? 1 : 0, receipt_valid ? 1 : 0,
        buffers_valid ? 1 : 0, targets_valid ? 1 : 0,
        native_valid ? 1 : 0, receipt_reject, control.enabled, control.completed,
        evidence.status, evidence.flags, evidence.recorded_frames,
        evidence.interval_records, evidence.fixed_delta_writes,
        evidence.physics_equal_frames, evidence.physics_corrected_frames,
        evidence.physics_correction_writes, evidence.physics_skipped_frames,
        evidence.qualified_events[protocol::kBarrelRollHook],
        evidence.qualified_events[protocol::kBarrelYawHook],
         evidence.barrel_rbx_overrides, evidence.barrel_angular_overrides,
         evidence.barrel_rbx_records, evidence.barrel_angular_records,
         evidence.barrel_random_bool_calls,
         evidence.barrel_random_lerp_calls,
         evidence.barrel_random_resets,
         control.completion_policy, evidence.completion_reason,
         static_cast<std::uint32_t>(evidence.reserved[0] >> 56u),
        control.replay_speed_factor,
        evidence.wrapper_entries[protocol::kLogicDispatcherHook],
        evidence.qualified_events[protocol::kLogicDispatcherHook],
        state.tick.complete, state.tick.coordinator.ticks_published,
        static_cast<std::uint32_t>(state.tick.coordinator.tick_phase),
        state.tick.coordinator.failures);
    // A readable status receipt is evidence even when its semantic verdict is
    // negative.  Return success for the transport so the host can restore all
    // hooks before it evaluates `complete`; the runner validates the explicit
    // complete/error/check fields carried by the status receipt.
    return written ? 0 : 10;
  }

  if (action == Action::kDumpReplayDiagnostic) {
    protocol::Control control{};
    protocol::Evidence evidence{};
    auto state_storage = std::make_unique<bridge::StateV1>();
    auto& state = *state_storage;
    const bool valid =
        ReadReceipt(mem, runtime, &control, &evidence, &state) &&
        StaticControlValid(control, runtime, limit) &&
        CompleteReceiptValid(control, evidence, state, limit) &&
        RecordedBuffersValid(mem, runtime, control, evidence, state) &&
        TargetsMatch(mem, control, evidence.status != protocol::kRestored) &&
        control.mode ==
            static_cast<std::uint32_t>(protocol::RunMode::kReplay);
    if (!valid) {
      close(mem);
      std::printf("G5_REPLAY_DIAGNOSTIC_DUMP passed=0 frames=%u\n", limit);
      return 10;
    }

    std::vector<recording::RecordingFrameV1> scratch(limit);
    std::vector<recording::RecordingFrameV1> source(limit);
    const std::size_t bytes = scratch.size() * sizeof(scratch[0]);
    bool read_buffers =
        pread(mem, scratch.data(), bytes,
              static_cast<off_t>(runtime.recorded_frames)) ==
            static_cast<ssize_t>(bytes) &&
        pread(mem, source.data(), bytes,
              static_cast<off_t>(runtime.replay_frames)) ==
            static_cast<ssize_t>(bytes);
    close(mem);

    std::vector<protocol::ReplayPhysicsDiagnosticRecordV1> records(limit);
    std::uint64_t equal = 0;
    std::uint64_t corrected = 0;
    for (std::uint32_t index = 0; read_buffers && index < limit; ++index) {
      const auto& natural = scratch[index];
      const auto& target = source[index];
      const auto kind = static_cast<g4::PhysicsCorrectionKindV1>(
          natural.reserved);
      const bool natural_equal =
          g4::ComponentFloatRangesEqual(
              natural.transform_bits, target.transform_bits,
              recording::kTransformSize) &&
          g4::ComponentFloatRangesEqual(
              natural.linear_velocity_bits, target.linear_velocity_bits,
              recording::kLinearVelocitySize);
      auto natural_identity = natural;
      auto target_identity = target;
      std::memset(natural_identity.transform_bits, 0,
                  sizeof(natural_identity.transform_bits));
      std::memset(natural_identity.linear_velocity_bits, 0,
                  sizeof(natural_identity.linear_velocity_bits));
      natural_identity.reserved = 0;
      std::memset(target_identity.transform_bits, 0,
                  sizeof(target_identity.transform_bits));
      std::memset(target_identity.linear_velocity_bits, 0,
                  sizeof(target_identity.linear_velocity_bits));
      target_identity.reserved = 0;
      const bool class_valid =
          (kind == g4::PhysicsCorrectionKindV1::kNaturalEqual &&
           natural_equal) ||
          (kind == g4::PhysicsCorrectionKindV1::kCorrectBoth &&
           !natural_equal);
      if (natural.tick != index || target.tick != index ||
          !g4::RawPhysicsBitsValid(natural.transform_bits,
                                   natural.linear_velocity_bits) ||
          !g4::PhysicsRecordingFrameValid(target) || !class_valid ||
          std::memcmp(&natural_identity, &target_identity,
                      sizeof(target_identity)) != 0) {
        read_buffers = false;
        break;
      }
      auto& record = records[index];
      record.tick = index;
      record.flags = natural_equal ? protocol::kDiagnosticNaturalEqual
                                   : protocol::kDiagnosticCorrected;
      std::memcpy(record.natural_transform, natural.transform_bits,
                  sizeof(record.natural_transform));
      std::memcpy(record.natural_linear, natural.linear_velocity_bits,
                  sizeof(record.natural_linear));
      equal += natural_equal ? 1u : 0u;
      corrected += natural_equal ? 0u : 1u;
    }
    read_buffers = read_buffers && equal == evidence.physics_equal_frames &&
                   corrected == evidence.physics_corrected_frames;

    protocol::ReplayPhysicsDiagnosticHeaderV1 header{};
    std::memcpy(header.magic, protocol::kReplayPhysicsDiagnosticMagic,
                sizeof(header.magic));
    header.version = protocol::kReplayPhysicsDiagnosticVersion;
    header.header_size = sizeof(header);
    header.record_size = sizeof(records[0]);
    header.frame_count = limit;
    header.fixed_delta_us = control.fixed_delta_us;
    header.flags = protocol::kDiagnosticNaturalEqual |
                   protocol::kDiagnosticCorrected;
    header.session_id = control.session_id;
    header.generation = control.generation;
    std::memcpy(header.recording_sha256, control.recording_sha256,
                sizeof(header.recording_sha256));

    FILE* output = read_buffers ? std::fopen(output_path, "wb") : nullptr;
    bool written = output != nullptr;
    if (written)
      written = std::fwrite(&header, sizeof(header), 1, output) == 1 &&
                std::fwrite(records.data(), sizeof(records[0]), records.size(),
                            output) == records.size();
    if (output != nullptr && std::fclose(output) != 0) written = false;

    // Export the TickReceipt array that the runtime already produced and the
    // strict status verifier already consumed. This is host-side evidence, not
    // another game hook or collector.
    const std::string receipt_path = std::string(output_path) + ".ticks.csv";
    FILE* receipt_output = written ? std::fopen(receipt_path.c_str(), "wb")
                                   : nullptr;
    bool receipts_written = receipt_output != nullptr;
    if (receipts_written) {
      receipts_written = std::fprintf(
          receipt_output,
          "tick,physics_interval_calls,steering_calls,brake_calls,"
          "accelerator_calls,natural_nitro_calls,suppressed_nitro_calls,"
          "injected_nitro_calls\n") > 0;
    }
    for (std::uint32_t index = 0; receipts_written && index < limit; ++index) {
      const auto& receipt = state.receipts[index];
      receipts_written = receipt.tick == index &&
          receipt.physics_interval_calls > 0 &&
          std::fprintf(
              receipt_output,
              "%" PRIu64 ",%u,%u,%u,%u,%u,%u,%u\n",
              receipt.tick, receipt.physics_interval_calls,
              receipt.steering_calls, receipt.brake_calls,
              receipt.accelerator_calls, receipt.natural_nitro_calls,
              receipt.suppressed_nitro_calls,
              receipt.injected_nitro_calls) > 0;
    }
    if (receipt_output != nullptr && std::fclose(receipt_output) != 0)
      receipts_written = false;
    written = written && receipts_written;
    std::printf(
        "G5_REPLAY_DIAGNOSTIC_DUMP passed=%d frames=%u equal=%" PRIu64
        " corrected=%" PRIu64
        " source_bound=1 before_correction=1 tick_receipts=%d\n",
        written ? 1 : 0, limit, equal, corrected,
        receipts_written ? 1 : 0);
    return written ? 0 : 10;
  }

  if (action == Action::kDumpRecord) {
    protocol::Control control{};
    protocol::Evidence evidence{};
    auto state_storage = std::make_unique<bridge::StateV1>();
    auto& state = *state_storage;
    const bool valid =
        ReadReceipt(mem, runtime, &control, &evidence, &state) &&
        StaticControlValid(control, runtime, limit) &&
        CompleteReceiptValid(control, evidence, state, limit) &&
        RecordedBuffersValid(mem, runtime, control, evidence, state) &&
        TargetsMatch(mem, control, evidence.status != protocol::kRestored) &&
        control.mode ==
            static_cast<std::uint32_t>(protocol::RunMode::kRecord);
    if (!valid) {
      close(mem);
      std::printf("G4_RECORD_DUMP passed=0 frames=%" PRIu64
                  " intervals=%" PRIu64 "\n",
                  evidence.recorded_frames, evidence.interval_records);
      return 10;
    }
    const std::uint32_t frame_count =
        control.completion_policy == static_cast<std::uint32_t>(
            protocol::CompletionPolicy::kRaceLifecycle)
            ? static_cast<std::uint32_t>(evidence.recorded_frames)
            : control.frame_limit;
    std::vector<recording::RecordingFrameV1> frames(frame_count);
    std::vector<g4::IntervalSampleV1> intervals(evidence.interval_records);
    const std::size_t frame_bytes = frames.size() * sizeof(frames[0]);
    const std::size_t interval_bytes = intervals.size() * sizeof(intervals[0]);
    const bool read_buffers =
        pread(mem, frames.data(), frame_bytes,
              static_cast<off_t>(runtime.recorded_frames)) ==
            static_cast<ssize_t>(frame_bytes) &&
        pread(mem, intervals.data(), interval_bytes,
              static_cast<off_t>(runtime.recorded_intervals)) ==
            static_cast<ssize_t>(interval_bytes);
    close(mem);
    protocol::RecordingBundleHeaderV1 header{};
    std::memcpy(header.magic, protocol::kRecordingBundleMagic,
                sizeof(header.magic));
    const bool sparse_bundle = control.fixed_delta_us == 8333 || control.fixed_delta_us == 6944;
    header.version = sparse_bundle ? protocol::kSparseRecordingBundleVersion
                                  : protocol::kRecordingBundleVersion;
    header.header_size = sizeof(header);
    header.frame_size = sizeof(frames[0]);
    header.interval_size = sizeof(intervals[0]);
    header.frame_count = frame_count;
    header.interval_count = static_cast<std::uint32_t>(intervals.size());
    header.fixed_delta_us = control.fixed_delta_us;
    header.flags = sparse_bundle ? protocol::kSparseRecordingBundleFlags
                                : protocol::kRecordingBundleFlags;
    header.session_id = control.session_id;
    header.generation = control.generation;
    FILE* output = read_buffers ? std::fopen(output_path, "wb") : nullptr;
    bool written = output != nullptr;
    if (written)
      written = std::fwrite(&header, sizeof(header), 1, output) == 1 &&
                std::fwrite(frames.data(), sizeof(frames[0]), frames.size(),
                            output) == frames.size() &&
                std::fwrite(intervals.data(), sizeof(intervals[0]),
                            intervals.size(), output) == intervals.size();
    if (output != nullptr && std::fclose(output) != 0) written = false;
  std::printf("G4_RECORD_DUMP passed=%d frames=%zu intervals=%zu "
              "physics_valid=1\n",
                written ? 1 : 0, frames.size(), intervals.size());
    return written ? 0 : 10;
  }

  if (action == Action::kPassiveStatus) {
    protocol::Control control{};
    protocol::Evidence evidence{};
    auto state_storage = std::make_unique<bridge::StateV1>();
    auto& state = *state_storage;
    const bool read = ReadReceipt(mem, runtime, &control, &evidence, &state);
    const bool passive = read && PassiveControlValid(control, runtime) &&
                         PassiveReceiptValid(control, evidence, state) &&
                         PassiveTargetMatches(mem, control);
    FILE* report = std::fopen(output_path, "wb");
    const bool written = report != nullptr &&
        std::fprintf(report,
            "G4_PASSIVE installed=%d entries=%" PRIu64 ",%" PRIu64
            ",%" PRIu64 ",%" PRIu64 " error=%u\n",
            passive ? 1 : 0, evidence.wrapper_entries[0],
            evidence.wrapper_entries[1], evidence.wrapper_entries[2],
            evidence.wrapper_entries[3], evidence.first_error) > 0 &&
        std::fclose(report) == 0;
    close(mem);
    std::printf(
        "G4_PASSIVE_STATUS passed=%d installed=%d entries=%" PRIu64
        ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 " error=%u\n",
        passive && written ? 1 : 0, passive ? 1 : 0,
        evidence.wrapper_entries[0], evidence.wrapper_entries[1],
        evidence.wrapper_entries[2], evidence.wrapper_entries[3],
        evidence.first_error);
    return passive && written ? 0 : 14;
  }

  if (action == Action::kDiagnose) {
    const bool resolved = ResolveInstallObjects(pid, outer_owner, &runtime);
    protocol::Control retained{};
    const bool retained_read =
        g2::ReadAt(mem, runtime.call.control, &retained);
    const std::uintptr_t retained_main =
        static_cast<std::uintptr_t>(retained.expected_main_object);
    const std::uintptr_t retained_interval =
        static_cast<std::uintptr_t>(retained.expected_interval_owner);
    const std::uintptr_t pair_main =
        resolved ? runtime.main_object : retained_main;
    const std::uintptr_t pair_interval =
        resolved ? runtime.interval_owner : retained_interval;
    std::uint64_t main_pair = 0, interval_pair = 0;
    const bool retained_identity =
        retained_read && retained_main != 0 && retained_interval != 0 &&
        retained.game_base == game_base;
    const bool pairs = (resolved || retained_identity) &&
        g2::ReadAt(mem, pair_main + protocol::kControlPairOffset,
                   &main_pair) &&
        g2::ReadAt(mem, pair_interval + protocol::kControlPairOffset,
                   &interval_pair);
    close(mem);
    std::printf(
        "G4_OBJECT_DIAG passed=%d begin_owner=0x%" PRIxPTR
        " interval=0x%" PRIxPTR " player=0x%" PRIxPTR
        " lifecycle=0x%" PRIxPTR " main=0x%" PRIxPTR
        " retained=%d retained_main=0x%" PRIxPTR
        " retained_interval=0x%" PRIxPTR
        " pairs=%d main_pair=0x%016" PRIx64
        " interval_pair=0x%016" PRIx64
        " main_axes=%.9g,%.9g interval_axes=%.9g,%.9g\n",
        resolved ? 1 : 0, runtime.begin_owner, runtime.interval_owner,
        runtime.player, runtime.lifecycle_object, runtime.main_object,
        retained_identity ? 1 : 0, retained_main, retained_interval,
        pairs ? 1 : 0, main_pair, interval_pair,
        static_cast<double>(g4::BitsFloat(static_cast<std::uint32_t>(main_pair))),
        static_cast<double>(g4::BitsFloat(static_cast<std::uint32_t>(main_pair >> 32))),
        static_cast<double>(g4::BitsFloat(static_cast<std::uint32_t>(interval_pair))),
        static_cast<double>(g4::BitsFloat(static_cast<std::uint32_t>(interval_pair >> 32))));
    return (resolved || retained_identity) && pairs ? 0 : 13;
  }

  const pid_t call_tid = UniqueSignalCatcher(pid);
  g2::FrozenSet frozen{};
  if (call_tid <= 0 || !g2::FreezeStable(pid, call_tid, &frozen)) {
    close(mem);
    return FailG4(pid, &frozen, false, "freeze", 6);
  }

#if defined(A9TAS_G4_NATIVE_ARM64_CONTROLLER)
  // Resolve again under the complete stop. The pre-stop resolution proves
  // that the requested action addresses a supported process; this second
  // resolution proves that the exact payload and immutable return trap did
  // not drift before the command transaction acquired ownership.
  a9tas::native_arm64_g4_payload_resolver_v1::Layout stopped_layout{};
  const char* stopped_failure="none";
  a9tas::native_arm64_immutable_trap_resolver_v1::Report stopped_trap{};
  const bool stopped_has_immutable_trap=
      a9tas::native_arm64_immutable_trap_resolver_v1::Resolve(
          pid,&stopped_trap);
  if (!stopped_has_immutable_trap) stopped_trap={};
  const bool stopped_native_identity=
      a9tas::native_arm64_g4_payload_resolver_v1::Resolve(
          pid,mem,&stopped_layout,&stopped_failure) &&
      stopped_layout.load_bias==runtime.call.payload_base &&
      stopped_layout.command==runtime.call.native_command &&
      stopped_layout.control==runtime.call.control &&
      stopped_layout.evidence==runtime.call.evidence &&
      stopped_layout.runtime==runtime.runtime &&
      stopped_layout.recorded_frames==runtime.recorded_frames &&
      stopped_layout.recorded_intervals==runtime.recorded_intervals &&
      stopped_layout.replay_frames==runtime.replay_frames &&
      stopped_layout.replay_intervals==runtime.replay_intervals &&
      stopped_layout.build_profile==runtime.build_profile &&
      stopped_trap.address==runtime.call.native_trap;
  if (!stopped_native_identity) {
    close(mem);
    return FailG4(pid,&frozen,false,"native_stopped_identity",6);
  }
#endif

  // Object resolution must use a mapping snapshot taken after the complete
  // process is stopped.  This keeps lifecycle/player identity and the
  // StepOptions-to-implementation owner binding in one stable snapshot.
  protocol::Control existing{};
  protocol::Evidence existing_evidence{};
  auto existing_state_storage = std::make_unique<bridge::StateV1>();
  auto& existing_state = *existing_state_storage;
  bool restoring_passive = false;
  bool archived_bundle_matches = false;
  bool replacing_cancelled_queued_session = false;
  if (action == Action::kInstallPassive) {
    runtime.call.maps = ReadMaps(pid);
    const bool receipt_read_for_install =
        !runtime.call.maps.empty() &&
        ReadReceipt(mem, runtime, &existing, &existing_evidence,
                    &existing_state);
    const bool fresh_preload = receipt_read_for_install &&
        PreloadedFreshValid(existing, existing_evidence, existing_state) &&
        RemoteBuildProfileFresh(mem, runtime);
    const bool restored_reuse = receipt_read_for_install &&
        RestoredSessionReusableValid(existing, existing_evidence,
                                     existing_state) &&
        RemoteBuildProfileMatches(mem, runtime);
    const bool original_targets = OriginalTargetsMatch(mem, runtime);
    if (!fresh_preload && !restored_reuse && original_targets) {
      // No payload command has run and every code/vtable target, including the
      // two random hooks, is still original.  This is stale session metadata,
      // not an uncertain mutation; report it separately so Android does not
      // convert a clean retryable rejection into a game force-stop.
      close(mem);
      return FailG4(pid, &frozen, false, "clean_original_precondition", 5);
    }
    if ((!fresh_preload && !restored_reuse) || !original_targets) {
      close(mem);
      return FailG4(pid, &frozen, false, "preloaded_precondition", 5);
    }
    if (fresh_preload && !PublishRemoteBuildProfile(mem, runtime)) {
      close(mem);
      return FailG4(pid, &frozen, false, "build_profile_publish", 5);
    }
  } else if (action == Action::kSealPausedRecord ||
             action == Action::kCheckpointAtNextClosedTick) {
    runtime.call.maps = ReadMaps(pid);
    const bool active_record = !runtime.call.maps.empty() &&
        ReadReceipt(mem, runtime, &existing, &existing_evidence,
                    &existing_state) &&
        StaticControlValid(existing, runtime, limit) &&
        existing.mode ==
            static_cast<std::uint32_t>(protocol::RunMode::kRecord) &&
        existing.completion_policy == static_cast<std::uint32_t>(
            protocol::CompletionPolicy::kRaceLifecycle) &&
        existing.enabled == 1 && existing.completed == 0 &&
        existing.active_helpers == 0 &&
        existing_evidence.status == protocol::kArmed &&
        existing_evidence.first_error == 0 &&
        existing_state.tick.complete == 0 &&
        existing_state.tick.coordinator.lifecycle ==
            coordinator::Lifecycle::kInRace &&
        existing_state.receipt_count != 0 &&
        PausedRecordCursorResolvable(
            existing_state.receipt_count,
            existing_state.tick.coordinator.tick,
            existing_state.tick.coordinator.ticks_begun,
            existing_state.tick.coordinator.tick_phase,
            existing_state.input_action.tick_open);
    if (!active_record) {
      close(mem);
      return FailG4(pid, &frozen, false, "seal_precondition", 5);
    }
  } else if (action == Action::kQueueActiveRecordRetry) {
    runtime.call.maps = ReadMaps(pid);
    const bool active_record = !runtime.call.maps.empty() &&
        ReadReceipt(mem, runtime, &existing, &existing_evidence,
                    &existing_state) &&
        StaticControlValid(existing, runtime, limit) &&
        TargetsMatch(mem, existing, true) &&
        NativePhysicsIdentityAlive(mem, existing) &&
        existing.mode == static_cast<std::uint32_t>(
            protocol::RunMode::kRecord) &&
        existing.completion_policy == static_cast<std::uint32_t>(
            protocol::CompletionPolicy::kRaceLifecycle) &&
        existing.enabled == 1 && existing.completed == 0 &&
        existing.active_helpers == 0 && existing.pending_state == 0 &&
        existing.pending_mode == 0 && existing.pending_generation == 0 &&
        existing.pending_reserved == 0 &&
        existing_evidence.status == protocol::kArmed &&
        existing_evidence.first_error == 0;
    if (!active_record) {
      close(mem);
      return FailG4(pid, &frozen, false, "active_record_queue", 5);
    }
  } else if (action == Action::kCancelPending) {
    runtime.call.maps = ReadMaps(pid);
    const bool receipt_read_for_cancel = !runtime.call.maps.empty() &&
        ReadReceipt(mem, runtime, &existing, &existing_evidence,
                    &existing_state);
    const bool active_record_queue = receipt_read_for_cancel &&
        existing.enabled == 1 && existing.completed == 0 &&
        existing_evidence.status == protocol::kArmed &&
        existing.pending_mode == static_cast<std::uint32_t>(
            protocol::RunMode::kRecord) &&
        existing.pending_state == 1 &&
        existing.pending_generation == existing.generation + 1u &&
        existing.pending_reserved == 1;
    const bool archived_queue = receipt_read_for_cancel &&
        existing.enabled == 0 && existing.completed == 1 &&
        existing_evidence.status == protocol::kQueued &&
        existing.pending_state == 1 &&
        ((existing.pending_generation == existing.generation &&
          existing.pending_reserved == 0) ||
         (existing.pending_mode == static_cast<std::uint32_t>(
              protocol::RunMode::kRecord) &&
           existing.pending_generation == existing.generation + 1u &&
           existing.pending_reserved == 1));
    const bool completed_record_queue = receipt_read_for_cancel &&
        existing.enabled == 0 && existing.completed == 1 &&
        existing_evidence.status == protocol::kComplete &&
        existing.pending_mode == static_cast<std::uint32_t>(
            protocol::RunMode::kRecord) &&
        existing.pending_state == 1 &&
        existing.pending_generation == existing.generation + 1u &&
        existing.pending_reserved == 1;
    if ((!active_record_queue && !archived_queue &&
         !completed_record_queue) ||
        !StaticControlValid(existing, runtime, limit) ||
        !TargetsMatch(mem, existing, true) ||
        existing.active_helpers != 0 || existing_evidence.first_error != 0) {
      close(mem);
      return FailG4(pid, &frozen, false, "cancel_pending_precondition", 5);
    }
  } else if (IsSessionConfigurationAction(action)) {
    runtime.call.maps = ReadMaps(pid);
    const bool receipt_read_for_config =
        !runtime.call.maps.empty() &&
        ReadReceipt(mem, runtime, &existing, &existing_evidence,
                    &existing_state);
    protocol::Control cancelled_queue_predecessor{};
    replacing_cancelled_queued_session = receipt_read_for_config &&
        CancelledQueuedPredecessorControl(
            existing, existing_evidence, existing_state,
            &cancelled_queue_predecessor);
    std::uintptr_t resolution_owner = outer_owner;
    std::uintptr_t current_interval = 0;
    if ((rearm_action || (action == Action::kArmLifecycleRecord &&
         receipt_read_for_config && existing_evidence.last_object[protocol::kTickHook] != 0)) &&
        !queued_rearm_action) {
      current_interval = static_cast<std::uintptr_t>(
          existing_evidence.last_object[protocol::kTickHook]);
      const std::uintptr_t current_vptr = static_cast<std::uintptr_t>(
          existing_evidence.last_vptr[protocol::kTickHook]);
      if (!receipt_read_for_config || current_interval == 0 ||
          (current_interval & 7u) != 0 ||
          current_vptr != runtime.call.game_base +
                              g_build_profile.core
                                  .physics_implementation_vtable_rva ||
          current_interval > UINTPTR_MAX -
                                 static_cast<std::uintptr_t>(
                                     -kStepOptionsThisAdjustment)) {
        close(mem);
        return FailG4(pid, &frozen, false, "rearm_live_owner", 5);
      }
      resolution_owner =
          current_interval + static_cast<std::uintptr_t>(
                                 -kStepOptionsThisAdjustment);
    }
    const bool objects_resolved = queued_rearm_action
        ? receipt_read_for_config && RemoteBuildProfileMatches(mem, runtime)
        : branch_rearm_action
        ? receipt_read_for_config &&
              current_interval == existing.expected_interval_owner &&
              BindPausedReplayRuntime(existing, &runtime) &&
              RemoteBuildProfileMatches(mem, runtime)
        : receipt_read_for_config &&
              ResolveInstallObjects(pid, resolution_owner, &runtime);
    if (!objects_resolved) {
      std::fprintf(stderr, "G4_OBJECT_DIAG receipt_read=%u owner=0x%" PRIxPTR "\n",
                   receipt_read_for_config ? 1u : 0u, resolution_owner);
      close(mem);
      return FailG4(pid, &frozen, false,
                    branch_rearm_action ? "branch_retained_objects"
                                        : "object_resolution",
                    5);
    }
    if (rearm_action && receipt_read_for_config) {
      const bool common_rearm = existing.frame_limit != 0 &&
          existing.generation < UINT32_MAX &&
          StaticControlValid(existing, runtime, existing.frame_limit) &&
          TargetsMatch(mem, existing, true);
      if (branch_rearm_action) {
        const bool completed_branch =
            CompleteReceiptValid(existing, existing_evidence, existing_state,
                                 existing.frame_limit) &&
            RecordedBuffersValid(mem, runtime, existing, existing_evidence,
                                 existing_state, false) &&
            PausedReplayForBranchValid(existing, existing_evidence,
                                       existing_state);
        const bool interrupted_branch = ActiveReplayForBranchValid(
            existing, existing_evidence, existing_state);
        archived_bundle_matches = common_rearm &&
            (completed_branch || interrupted_branch);
      } else {
        // AluTasV2 keeps its manager/hooks resident and replaces only the
        // active recording or replay state.  A clean completed session is the
        // predecessor receipt; it does not have to contain the same bytes as
        // the next selected replay.
        const bool cancelled_queue_receipt =
            replacing_cancelled_queued_session &&
            CompleteReceiptValid(cancelled_queue_predecessor,
                                 existing_evidence, existing_state,
                                 cancelled_queue_predecessor.frame_limit) &&
            (cancelled_queue_predecessor.mode !=
                     static_cast<std::uint32_t>(protocol::RunMode::kRecord) ||
             RecordedBuffersValid(mem, runtime, cancelled_queue_predecessor,
                                  existing_evidence, existing_state, false)) &&
            CompletedResidentSessionForRearmValid(
                cancelled_queue_predecessor, existing_evidence,
                existing_state);
        const bool current_receipt =
            CompleteReceiptValid(existing, existing_evidence, existing_state,
                                 existing.frame_limit) &&
            RecordedBuffersValid(mem, runtime, existing, existing_evidence,
                                 existing_state, false) &&
            CompletedResidentSessionForRearmValid(
                existing, existing_evidence, existing_state);
        archived_bundle_matches = common_rearm &&
            (cancelled_queue_receipt || current_receipt);
      }
    }
    const bool configuration_precondition = rearm_action
        ? archived_bundle_matches
        : receipt_read_for_config && PassiveControlValid(existing, runtime) &&
              PassiveReceiptValid(existing, existing_evidence,
                                  existing_state) &&
              PassiveTargetMatches(mem, existing);
    if (!configuration_precondition) {
      close(mem);
      return FailG4(pid, &frozen, false,
                    rearm_action ? "archive_precondition"
                                 : "passive_precondition",
                    5);
    }
  } else {
    if (!g2::ReadAt(mem, runtime.call.control, &existing)) {
      close(mem);
      return FailG4(pid, &frozen, false, "restore_control", 5);
    }
    restoring_passive = PassiveControlValid(existing, runtime);
    if (!restoring_passive && !StaticControlValid(existing, runtime, limit)) {
      close(mem);
      return FailG4(pid, &frozen, false, "restore_control", 5);
    }
  }

  bool command_ok = false;
  protocol::Control control{};
  protocol::Evidence evidence{};
  auto state_storage = std::make_unique<bridge::StateV1>();
  auto& state = *state_storage;
  std::uint64_t guest_return = 0;
  long rip_bias = 0;
  bool guest_called = false;
  bool receipt_read = false;
  bool static_valid = false;
  bool control_valid = false;
  bool evidence_valid = false;
  bool target_valid = false;
  bool begin_valid = false;
  if (action == Action::kInstallPassive) {
    guest_called = g2::CallGuest(
        pid, &runtime.call, &frozen,
        static_cast<std::uint64_t>(protocol::Command::kInstallPassive),
        kInstallTag, &guest_return, &rip_bias);
    receipt_read = ReadReceipt(mem, runtime, &control, &evidence, &state);
    control_valid = receipt_read && PassiveControlValid(control, runtime);
    evidence_valid =
        receipt_read && PassiveReceiptValid(control, evidence, state);
    static_valid = control_valid && evidence_valid;
    target_valid = receipt_read && PassiveTargetMatches(mem, control);
    begin_valid = receipt_read && control.expected_begin_owner == 0 &&
                  control.expected_begin_owner_vptr == 0;
    command_ok = guest_called && static_valid && target_valid && begin_valid;
  } else if (action == Action::kSealPausedRecord ||
             action == Action::kCheckpointAtNextClosedTick) {
    guest_called = g2::CallGuest(
        pid, &runtime.call, &frozen,
        static_cast<std::uint64_t>(protocol::Command::kSealPausedRecord),
        kArmTag, &guest_return, &rip_bias);
    receipt_read = ReadReceipt(mem, runtime, &control, &evidence, &state);
    static_valid = receipt_read && StaticControlValid(control, runtime, limit);
    const bool manual_checkpoint = receipt_read &&
        evidence.completion_reason == static_cast<std::uint32_t>(
            protocol::CompletionReason::kManualCheckpoint);
    const bool retry_or_race_end = receipt_read &&
        evidence.completion_reason == static_cast<std::uint32_t>(
            protocol::CompletionReason::kRaceLifecycle);
    evidence_valid = receipt_read &&
        CompleteReceiptValid(control, evidence, state, limit) &&
        (manual_checkpoint || retry_or_race_end);
    // A confirmed Retry may already have retired the previous race objects.
    // They are still required for a checkpoint that will be dumped, but they
    // must not turn a valid race-end discard into an artificial crash.
    target_valid = retry_or_race_end ||
        (receipt_read && TargetsMatch(mem, control, true));
    begin_valid = retry_or_race_end ||
        (receipt_read && NativePhysicsIdentityAlive(mem, control));
    command_ok = guest_called && static_valid && evidence_valid &&
        target_valid && begin_valid && control.enabled == 0 &&
        control.completed == 1 && control.active_helpers == 0 &&
        evidence.status == protocol::kComplete;
  } else if (action == Action::kQueueActiveRecordRetry) {
    guest_called = g2::CallGuest(
        pid, &runtime.call, &frozen,
        static_cast<std::uint64_t>(protocol::Command::kQueueActiveRecordRetry),
        kRearmTag, &guest_return, &rip_bias);
    receipt_read = ReadReceipt(mem, runtime, &control, &evidence, &state);
    static_valid = receipt_read && StaticControlValid(control, runtime, limit);
    target_valid = receipt_read && TargetsMatch(mem, control, true);
    begin_valid = receipt_read && NativePhysicsIdentityAlive(mem, control);
    command_ok = guest_called && receipt_read && static_valid && target_valid &&
        begin_valid && control.enabled == 1 && control.completed == 0 &&
        control.active_helpers == 0 && control.pending_mode ==
            static_cast<std::uint32_t>(protocol::RunMode::kRecord) &&
        control.pending_state == 1 &&
        control.pending_generation == control.generation + 1u &&
        control.pending_reserved == 1 && evidence.status == protocol::kArmed &&
        evidence.first_error == 0;
  } else if (action == Action::kCancelPending) {
    const bool was_archived = existing.enabled == 0 && existing.completed == 1;
    guest_called = g2::CallGuest(
        pid, &runtime.call, &frozen,
        static_cast<std::uint64_t>(protocol::Command::kCancelPending),
        kRearmTag, &guest_return, &rip_bias);
    receipt_read = ReadReceipt(mem, runtime, &control, &evidence, &state);
    static_valid = receipt_read && StaticControlValid(control, runtime, limit);
    target_valid = receipt_read && TargetsMatch(mem, control, true);
    begin_valid = true;
    command_ok = guest_called && receipt_read && static_valid && target_valid &&
        control.pending_mode == 0 && control.pending_state == 0 &&
        control.pending_generation == 0 && control.pending_reserved == 0 &&
        control.active_helpers == 0 && evidence.first_error == 0 &&
        (was_archived
             ? control.enabled == 0 && control.completed == 1 &&
                   evidence.status == protocol::kComplete
             : control.enabled == 1 && control.completed == 0 &&
                   evidence.status == protocol::kArmed);
  } else if (IsSessionConfigurationAction(action)) {
    std::uintptr_t begin_vptr = 0, interval_vptr = 0, player_vptr = 0,
                   main_vptr = 0, barrel_vptr = 0;
    std::uintptr_t setter_vptr = 0;
    std::uintptr_t nitro_service = 0;
    std::uint32_t phase = 0;
    const bool stopped_identity = queued_rearm_action ||
        (g2::ReadAt(mem, runtime.begin_owner, &begin_vptr) &&
        begin_vptr == runtime.begin_owner_vptr &&
        g2::ReadAt(mem, runtime.interval_owner, &interval_vptr) &&
        interval_vptr == runtime.interval_owner_vptr &&
        g2::ReadAt(mem, runtime.barrel_owner, &barrel_vptr) &&
        barrel_vptr == runtime.barrel_owner_vptr &&
        g2::ReadAt(mem, runtime.player, &player_vptr) &&
        player_vptr == runtime.player_vptr &&
        g2::ReadAt(mem, runtime.main_object, &main_vptr) &&
        main_vptr == runtime.main_object_vptr &&
        g2::ReadAt(mem, runtime.setter_object, &setter_vptr) &&
        setter_vptr == runtime.setter_object_vptr &&
        g2::ReadAt(mem,
                   runtime.interval_owner + protocol::kNitroServiceOffset,
                   &nitro_service) && nitro_service <= UINTPTR_MAX - 8 &&
        nitro_service + 8 == runtime.nitro_state &&
        g2::ReadAt(mem, runtime.lifecycle_state, &phase) &&
        phase == (branch_rearm_action ? 3u : 2u) &&
        NativePhysicsIdentityAlive(mem, runtime));
    if (!stopped_identity) {
      close(mem);
      return FailG4(pid, &frozen, false, "stopped_identity", 7);
    }
    control = existing;
    control.frame_limit = limit;
    control.mode = static_cast<std::uint32_t>(
        (action == Action::kArmRecord ||
         action == Action::kArmLifecycleRecord || record_rearm_action)
            ? protocol::RunMode::kRecord
             : replay_action
                   ? protocol::RunMode::kReplay
                  : protocol::RunMode::kNeutral);
    control.completion_policy = static_cast<std::uint32_t>(
        (action == Action::kArmLifecycleRecord || record_rearm_action)
            ? protocol::CompletionPolicy::kRaceLifecycle
            : protocol::CompletionPolicy::kFixedFrameLimit);
    // Replay owns its recorded timestep; continuation keeps that same time
    // domain. Only a fresh recording uses the user's requested frequency.
    control.fixed_delta_us = replay_action
        ? replay_bundle.header.fixed_delta_us
        : branch_rearm_action ? existing.fixed_delta_us
        : static_cast<std::uint32_t>(record_delta_us);
    control.replay_speed_factor = replay_action
        ? static_cast<std::uint32_t>(replay_speed_raw)
        : 1u;
    control.replay_speed_reserved = replay_action
        ? static_cast<std::uint32_t>(replay_barrier_raw)
        : protocol::kReplayCompletionBarrierDisabled;
    // QueueArchivedSession reserves the next generation before Retry.  If the
    // user ends continuous mode before that pending session activates,
    // CancelPending clears only the pending fields: Control still describes
    // the unused next generation while the sealed receipts remain on
    // archived_generation.  Reuse that already-reserved identity instead of
    // skipping another generation and falsely archiving the unused one.
    const std::uint32_t prior_generation = replacing_cancelled_queued_session
        ? static_cast<std::uint32_t>(existing.archived_generation)
        : existing.generation;
    control.generation = rearm_action ? prior_generation + 1u : 1u;
    control.session_id = replacing_cancelled_queued_session
        ? existing.session_id
        : rearm_action
        ? existing.session_id ^ 0x9e3779b97f4a7c15ULL ^
              static_cast<std::uint64_t>(control.generation)
        : (ticks << 16) ^ static_cast<std::uint64_t>(pid);
    if (control.session_id == 0) control.session_id = 1;
    if (!queued_rearm_action) {
      // An explicit arm/rearm replaces any resident next-race choice.  Copying
      // stale pending fields from the archived Control made an otherwise valid
      // atomic Replay->Record session fail before its first tick.
      control.pending_mode = 0u;
      control.pending_generation = 0u;
      control.pending_reserved = 0u;
      control.pending_state = 0u;
    }
    if (!queued_rearm_action) {
      control.expected_begin_owner = runtime.begin_owner;
      control.expected_begin_owner_vptr = runtime.begin_owner_vptr;
      control.expected_interval_owner = runtime.interval_owner;
      control.expected_interval_owner_vptr = runtime.interval_owner_vptr;
      control.expected_player = runtime.player;
      control.expected_player_vptr = runtime.player_vptr;
      control.lifecycle_state_address = runtime.lifecycle_state;
      control.expected_main_object = runtime.main_object;
      control.expected_main_object_vptr = runtime.main_object_vptr;
      control.expected_nitro_state = runtime.nitro_state;
      control.expected_setter_object = runtime.setter_object;
      control.expected_setter_vptr = runtime.setter_object_vptr;
      control.expected_backend_interface = runtime.backend_interface;
      control.expected_physics_velocity_interface =
          runtime.physics_velocity_interface;
      control.expected_native_body = runtime.native_body;
      control.expected_barrel_owner = runtime.barrel_owner;
      control.expected_barrel_owner_vptr = runtime.barrel_owner_vptr;
    }
    if (replay_action) {
      const std::size_t frame_bytes =
          replay_bundle.frames.size() * sizeof(replay_bundle.frames[0]);
      const std::size_t interval_bytes =
          replay_bundle.intervals.size() * sizeof(replay_bundle.intervals[0]);
      if (!g2::WriteExact(mem, runtime.replay_frames,
                          replay_bundle.frames.data(), frame_bytes) ||
          !g2::WriteExact(mem, runtime.replay_intervals,
                          replay_bundle.intervals.data(), interval_bytes)) {
        close(mem);
        return FailG4(pid, &frozen, false, "replay_buffer_publish", 8);
      }
      control.replay_frame_count =
          static_cast<std::uint32_t>(replay_bundle.frames.size());
      control.replay_interval_count =
          static_cast<std::uint32_t>(replay_bundle.intervals.size());
      std::memcpy(control.recording_sha256, replay_bundle.sha256.data(),
                  replay_bundle.sha256.size());
    } else if (record_rearm_action) {
      // Every Replay -> Record transition must discard the predecessor's
      // replay-only control metadata.  Previously this reset was limited to
      // paused branch continuation, so a normal completed replay followed by
      // Retry and rearm-record retained replay_frame_count/interval_count/SHA.
      // ArmControlValid then deterministically faulted at progress 0x1100
      // before the first recording Tick could be armed.
      control.replay_frame_count = 0;
      control.replay_interval_count = 0;
      std::memset(control.recording_sha256, 0,
                  sizeof(control.recording_sha256));
    }
    if (rearm_action) {
      control.archived_generation = prior_generation;
      // A cancelled queued session has already replaced Control with the next
      // replay configuration, while the sealed Evidence/State still belong to
      // archived_generation.  Keep the archived counts published by the queue
      // transaction; deriving them from the staged replay would mix two
      // sessions and make the payload reject the otherwise valid predecessor.
      control.archived_frame_count = replacing_cancelled_queued_session
          ? existing.archived_frame_count
          : existing_state.receipt_count;
      control.archived_interval_count = replacing_cancelled_queued_session
          ? existing.archived_interval_count
          : existing.mode == static_cast<std::uint32_t>(
                                 protocol::RunMode::kReplay)
                ? existing.replay_interval_count
                : existing_evidence.interval_records;
    } else {
      control.archived_generation = 0;
      control.archived_frame_count = 0;
      control.archived_interval_count = 0;
    }
    if (!g2::WriteExact(mem, runtime.call.control, &control, sizeof(control))) {
      close(mem);
      return FailG4(pid, &frozen, false, "configure", 8);
    }
    guest_called = g2::CallGuest(
        pid, &runtime.call, &frozen,
        static_cast<std::uint64_t>(
            queued_rearm_action
                ? (replay_action ? protocol::Command::kQueueArchivedReplay
                                 : protocol::Command::kQueueArchivedRecord)
                : branch_rearm_action
                ? protocol::Command::kRearmPausedReplayRecord
                : record_rearm_action
                ? protocol::Command::kRearmArchivedRecord
                : rearm_action ? protocol::Command::kRearmArchivedReplay
                               : protocol::Command::kArmSession),
        rearm_action ? kRearmTag : kArmTag, &guest_return, &rip_bias);
    receipt_read = ReadReceipt(mem, runtime, &control, &evidence, &state);
    static_valid = receipt_read && StaticControlValid(control, runtime, limit);
    target_valid = receipt_read && TargetsMatch(mem, control, true);
    begin_valid = queued_rearm_action ? receipt_read : receipt_read &&
                   control.expected_begin_owner == runtime.begin_owner &&
                   control.expected_begin_owner_vptr ==
                        runtime.begin_owner_vptr &&
                   control.expected_setter_object == runtime.setter_object &&
                   control.expected_setter_vptr == runtime.setter_object_vptr &&
                   control.expected_backend_interface ==
                       runtime.backend_interface &&
                   control.expected_physics_velocity_interface ==
                       runtime.physics_velocity_interface &&
                   control.expected_native_body == runtime.native_body &&
                   control.expected_barrel_owner == runtime.barrel_owner &&
                   control.expected_barrel_owner_vptr ==
                       runtime.barrel_owner_vptr &&
                   NativePhysicsIdentityAlive(mem, runtime);
    command_ok = guest_called && receipt_read && static_valid && target_valid &&
        begin_valid && control.active_helpers == 0 && evidence.first_error == 0 &&
        (queued_rearm_action
             ? control.enabled == 0 && control.completed == 1 &&
                   control.pending_state == 1 &&
                   evidence.status == protocol::kQueued
             : control.enabled == 1 && control.completed == 0 &&
                   evidence.status == protocol::kArmed);
  } else {
    if (!(restoring_passive ? PassiveTargetMatches(mem, existing)
                            : TargetsMatch(mem, existing, true)) ||
        existing.active_helpers != 0) {
      close(mem);
      return FailG4(pid, &frozen, false, "restore_precondition", 7);
    }
    guest_called = g2::CallGuest(pid, &runtime.call, &frozen,
        static_cast<std::uint64_t>(protocol::Command::kRestore),
        kRestoreTag, &guest_return, &rip_bias);
    receipt_read = ReadReceipt(mem, runtime, &control, &evidence, &state);
    static_valid = receipt_read &&
        (restoring_passive ? PassiveControlValid(control, runtime)
                           : StaticControlValid(control, runtime, limit));
    target_valid = receipt_read && TargetsMatch(mem, control, false);
    begin_valid = receipt_read &&
        (restoring_passive
             ? control.expected_begin_owner == 0 &&
                   control.expected_begin_owner_vptr == 0
              : control.expected_begin_owner != 0 &&
                    control.expected_begin_owner_vptr ==
                        runtime.call.game_base +
                            g_build_profile.core.physics_context_vtable_rva);
    command_ok = guest_called && receipt_read && static_valid &&
        control.enabled == 0 && control.active_helpers == 0 &&
        evidence.status == protocol::kRestored &&
        target_valid && begin_valid;
  }
  if (!command_ok) {
    std::fprintf(stderr,
        "G4_COMMAND_DIAG action=%u guest_called=%d guest=0x%" PRIx64
        " receipt=%d static=%d control=%d evidence=%d targets=%d "
        "begin_vptr=%d status=%d "
        "error=%u flags=0x%" PRIx64 " enabled=%u completed=%u "
        "active=%u ticks=%u\n",
        static_cast<unsigned>(action), guest_called ? 1 : 0, guest_return,
        receipt_read ? 1 : 0, static_valid ? 1 : 0,
        control_valid ? 1 : 0, evidence_valid ? 1 : 0,
        target_valid ? 1 : 0, begin_valid ? 1 : 0, evidence.status,
        evidence.first_error, evidence.flags, control.enabled,
        control.completed, control.active_helpers, state.receipt_count);
    // Persist the failure receipt before FailG4 may terminate an uncertain
    // process.  The runner must never lose the only evidence that explains a
    // bounded guest-call timeout.
    FILE* failure_report = std::fopen(output_path, "wb");
    if (failure_report != nullptr) {
      (void)std::fprintf(
          failure_report,
          "G4_FAILURE action=%u guest_called=%d guest=0x%" PRIx64
          " receipt=%d static=%d targets=%d begin_vptr=%d status=%d "
          "error=%u progress=0x%" PRIx64 " flags=0x%" PRIx64
          " enabled=%u completed=%u active=%u ticks=%u\n",
          static_cast<unsigned>(action), guest_called ? 1 : 0,
          guest_return, receipt_read ? 1 : 0, static_valid ? 1 : 0,
          target_valid ? 1 : 0, begin_valid ? 1 : 0, evidence.status,
          evidence.first_error,
          static_cast<std::uint64_t>(
              evidence.reserved[0] & 0x00ffffffffffffffULL),
          evidence.flags,
          control.enabled, control.completed, control.active_helpers,
          state.receipt_count);
      (void)std::fflush(failure_report);
      (void)std::fclose(failure_report);
    }
    close(mem);
    const bool authoritative_install =
        action == Action::kInstallPassive && guest_called && receipt_read &&
        target_valid && evidence.status == protocol::kPassiveInstalled &&
        evidence.first_error == 0 && control.enabled == 0 &&
        control.active_helpers == 0;
    // InstallPassive is deliberately validation-only: the payload does not
    // publish code hooks or setter slots until a later arm command.  A complete
    // fault receipt with zero published-patch flags therefore proves that the
    // game is still untouched even though the guest function returned false.
    // Detach cleanly instead of making a channel-profile mismatch look like a
    // game crash.  Ambiguous/no-receipt failures and all arm-time failures keep
    // the restore-or-terminate behavior.
    const bool authoritative_passive_rejection =
        action == Action::kInstallPassive && receipt_read &&
        evidence.status == protocol::kFault && evidence.first_error != 0 &&
        control.enabled == 0 && control.active_helpers == 0 &&
        state.receipt_count == 0 &&
        (evidence.flags & protocol::kAllPatchesPublished) == 0;
    return FailG4(pid, &frozen,
                  !authoritative_install && !authoritative_passive_rejection,
                   "guest_command", 9);
  }
  if (action == Action::kCheckpointAtNextClosedTick) {
    const std::uint32_t disabled =
        protocol::kReplayCompletionBarrierDisabled;
    std::uint32_t observed = UINT32_MAX;
    const std::uintptr_t barrier_address = runtime.call.control +
        offsetof(protocol::Control, replay_speed_reserved);
    const bool released = record_checkpoint_barrier_held &&
        g2::WriteExact(mem, barrier_address, &disabled, sizeof(disabled)) &&
        g2::ReadAt(mem, barrier_address, &observed) && observed == disabled;
    if (!released) {
      close(mem);
      return FailG4(pid, &frozen, true, "checkpoint_release", 10);
    }
    target_completion_barrier_released = true;
  }
  const std::size_t frozen_count =
      frozen.other_tids.size() + (frozen.call_attached ? 1u : 0u);
  const bool detached = g2::DetachAll(&frozen);
  FILE* report = std::fopen(output_path, "wb");
  const bool written = report != nullptr &&
      std::fprintf(report,
          "G4_ACTION action=%u pid=%d limit=%u guest=0x%" PRIx64
          " rip_bias=%ld status=%d ticks=%u detached=%d"
          " mode=%u barrier=%u generation=%u pending=%u,%u,%u,%u"
          " checkpoint=%d checkpoint_ticks=%u,%u pause=%d release=%d\n",
          static_cast<unsigned>(action), pid, limit, guest_return, rip_bias,
          evidence.status, state.receipt_count, detached ? 1 : 0,
          control.mode, control.replay_speed_reserved, control.generation,
          control.pending_mode, control.pending_generation,
          control.pending_state, control.pending_reserved,
          action == Action::kCheckpointAtNextClosedTick ? 1 : 0,
          record_checkpoint_initial_ticks, state.receipt_count,
          target_pause_injected ? 1 : 0,
          target_completion_barrier_released ? 1 : 0) > 0 &&
      std::fclose(report) == 0;
  if (report == nullptr) {}
  close(mem);
  if (!detached) return FailG4(pid, nullptr, true, "detach", 11);
  std::printf(
      "G4_TICK_COORDINATOR_CONTROLLER passed=%d action=%u pid=%d limit=%u "
      "status=%d ticks=%u frozen=%zu rollback=1 detach=1"
      " checkpoint=%d checkpoint_ticks=%u,%u pause=%d release=%d\n",
      written ? 1 : 0, static_cast<unsigned>(action), pid, limit,
      evidence.status, state.receipt_count,
      frozen_count, action == Action::kCheckpointAtNextClosedTick ? 1 : 0,
      record_checkpoint_initial_ticks, state.receipt_count,
      target_pause_injected ? 1 : 0,
      target_completion_barrier_released ? 1 : 0);
  return written ? 0 : 12;
}
