// Host-side transaction controller for the Android three-boundary G3 Gate.
// It reuses the live-proven G2 full-process freeze and NativeBridge guest-call
// machinery. Physics Interval owns BeginTick+PrePhysics; Final Writer and Tick
// End close the same logical tick. All three hooks are session-scoped.

#define main a9tas_g2_controller_embedded_main_v1
#include "g2_physics_interval_controller_v1.cpp"
#undef main

#include "g3_multi_hook_runtime_v1.h"
#include "race_lifecycle_object_resolver_v1.h"
#include "vehicle_state_resolver_v1.h"

#include <cinttypes>

namespace g3_controller_v1 {

namespace g2 = g2_controller_v1;
namespace protocol = a9tas::g3_multi_hook_runtime_v1;
namespace adapter = a9tas::g3_boundary_adapter_v1;
namespace coordinator = a9tas::g3_tick_coordinator_v1;
namespace lifecycle = a9tas::race_lifecycle_v1;
namespace vehicle = a9tas::vehicle_state_v1;

constexpr char kAcknowledgement[] = "I_ACCEPT_G3_TICK_COORDINATOR_V1";
constexpr char kBootstrapLocatorSymbol[] =
    "a9tas_bootstrap_g3_tick_coordinator_locator_v1";
constexpr char kBootstrapTrapSymbol[] =
    "a9tas_bootstrap_g3_tick_coordinator_return_trap_v1";
constexpr char kBootstrapCalibrateSymbol[] =
    "a9tas_bootstrap_g3_tick_coordinator_calibrate_tid_v1";
constexpr char kCommandSymbol[] = "a9tas_g3_command_v1";
constexpr char kControlLocatorSymbol[] = "a9tas_g3_control_data_v1";
constexpr char kEvidenceLocatorSymbol[] = "a9tas_g3_evidence_data_v1";
constexpr char kRuntimeLocatorSymbol[] = "a9tas_g3_runtime_data_v1";
constexpr std::uint64_t kLocatorMagic = 0x47335443314c4f43ULL;
constexpr std::int64_t kStepOptionsThisAdjustment = -0x2A78;
constexpr std::uint32_t kArmTag = 0x47334901;
constexpr std::uint32_t kRestoreTag = 0x47334902;
constexpr std::uint32_t kInstallTag = 0x47334903;

constexpr std::uint8_t kExpectedPrologues[protocol::kHookCount][16] = {
    {0xff,0xc3,0x00,0xd1,0xf5,0x53,0x01,0xa9,
     0xf3,0x7b,0x02,0xa9,0x35,0x11,0x91,0x52},
    {0xec,0x0f,0x17,0xfc,0xeb,0x2b,0x01,0x6d,
     0xe9,0x23,0x02,0x6d,0xfc,0x6f,0x03,0xa9},
    {0xff,0xc3,0x00,0xd1,0xf4,0x0b,0x00,0xf9,
     0xf3,0x7b,0x02,0xa9,0x28,0x00,0x40,0xf9},
};

enum class Action : std::uint32_t {
  kArm = 1,
  kStatus = 2,
  kRestore = 3,
  kDiagnose = 4,
  kPassiveStatus = 5,
  kInstallPassive = 6,
};

struct Runtime {
  g2::Runtime call{};
  std::uintptr_t runtime{};
  std::uintptr_t begin_owner{};
  std::uintptr_t begin_owner_vptr{};
  std::uintptr_t interval_owner{};
  std::uintptr_t interval_owner_vptr{};
  std::uintptr_t player{};
  std::uintptr_t player_vptr{};
  std::uintptr_t lifecycle_object{};
  std::uintptr_t lifecycle_state{};
};

bool ParseAction(const char* text, Action* action) {
  if (text == nullptr || action == nullptr) return false;
  if (std::strcmp(text, "arm") == 0) *action = Action::kArm;
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

bool ResolvePayloadLocator(const ElfImage& elf, pid_t pid,
                           std::uintptr_t base, const char* symbol,
                           std::uintptr_t* output) {
  return g2::ResolveLocatorAddress(elf, pid, base, symbol, output);
}

bool ResolveArtifacts(pid_t pid, std::uint64_t start_ticks,
                      std::uintptr_t requested_game_base, Runtime* runtime) {
  if (runtime == nullptr || !IsAlive(pid, start_ticks) ||
      TracerPid(pid) != 0 || requested_game_base == 0)
    return false;
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
      !g2::DataAddressValid(runtime->call.maps, runtime->call.control,
                            sizeof(protocol::Control)) ||
      !g2::DataAddressValid(runtime->call.maps, runtime->call.evidence,
                            sizeof(protocol::Evidence)) ||
      !g2::DataAddressValid(runtime->call.maps, runtime->runtime,
                            sizeof(adapter::State)))
    return false;

  const Mapping* game = FindMapping(runtime->call.maps, requested_game_base, 1);
  if (game == nullptr || game->start != requested_game_base ||
      game->offset != 0 || !game->readable || game->writable ||
      game->path.find("libAsphalt9.so") == std::string::npos)
    return false;
  FileImage game_file{};
  if (!ReadPinnedRegularFile(
          game->path.c_str(),
          "671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0",
          &game_file))
    return false;
  ElfImage game_elf{};
  if (!game_elf.Parse(game_file, EM_AARCH64) ||
      game_elf.build_id() !=
          "e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b")
    return false;
  runtime->call.game_base = requested_game_base;
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
  if (!lifecycle::ResolveCountdownObject(pid, mem, runtime->call.game_base,
                                         &race)) {
    std::fprintf(stderr,
        "G3_OBJECT_DIAG lifecycle=0 structural=%u countdown=%u scanned=%" PRIu64
        "\n", race.structurally_valid, race.countdown_candidates,
        race.scanned_bytes);
    close(mem);
    return false;
  }
  if (!vehicle::Resolve(pid, mem, runtime->call.game_base, &player)) {
    std::fprintf(stderr, "G3_OBJECT_DIAG vehicle=0\n");
    close(mem);
    return false;
  }

  std::uintptr_t outer_vptr = 0, implementation_vptr = 0, player_vptr = 0;
  std::int64_t adjustment = 0;
  const bool interval =
      g2::ReadAt(mem, outer_owner, &outer_vptr) &&
      outer_vptr == runtime->call.game_base +
                        a9tas::physics_interval_shadow_v2::kStepOptionsVtableRva &&
      outer_vptr >= sizeof(std::uintptr_t) * 4 &&
      g2::ReadAt(mem, outer_vptr - sizeof(std::uintptr_t) * 4, &adjustment) &&
      adjustment == kStepOptionsThisAdjustment &&
      outer_owner >= static_cast<std::uintptr_t>(-adjustment);
  if (!interval) {
    close(mem);
    return false;
  }
  const std::uintptr_t implementation =
      outer_owner - static_cast<std::uintptr_t>(-adjustment);
  const bool identities =
      g2::ReadAt(mem, implementation, &implementation_vptr) &&
      implementation_vptr ==
          runtime->call.game_base +
              adapter::kPhysicsImplementationVtableRva &&
      g2::ReadAt(mem, player.physics_base, &player_vptr);
  close(mem);
  if (!identities || player_vptr == 0) return false;

  runtime->call.outer_owner = outer_owner;
  runtime->call.outer_owner_vptr = outer_vptr;
  runtime->begin_owner = implementation;
  runtime->begin_owner_vptr = implementation_vptr;
  runtime->interval_owner = implementation;
  runtime->interval_owner_vptr = implementation_vptr;
  runtime->player = player.physics_base;
  runtime->player_vptr = player_vptr;
  runtime->lifecycle_object = race.selected.object;
  runtime->lifecycle_state = race.selected.state_address;
  return g2::DataAddressValid(runtime->call.maps, runtime->interval_owner,
                              sizeof(std::uintptr_t)) &&
         g2::DataAddressValid(runtime->call.maps, runtime->player,
                              sizeof(std::uintptr_t)) &&
         g2::DataAddressValid(runtime->call.maps, runtime->lifecycle_state,
                              sizeof(std::uint32_t));
}

std::size_t HookPatchSize(std::uint32_t index);

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
      control.expected_begin_owner != control.expected_interval_owner ||
      control.expected_begin_owner_vptr !=
          control.expected_interval_owner_vptr ||
      control.game_base != runtime.call.game_base ||
      control.begin_continuation != 0 ||
      control.begin_branch_destination != 0)
    return false;
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index) {
    if (control.target_entry[index] !=
            runtime.call.game_base +
                (index == 0 ? adapter::kPhysicsIntervalRva
                 : index == 1 ? adapter::kFinalWriterRva
                              : adapter::kFrameEventRva) ||
        control.target_tail[index] !=
            control.target_entry[index] + HookPatchSize(index) ||
        control.wrapper[index] == 0)
      return false;
  }
  for (std::uint64_t value : control.reserved)
    if (value != 0) return false;
  return true;
}

bool ReadReceipt(int mem, const Runtime& runtime, protocol::Control* control,
                 protocol::Evidence* evidence, adapter::State* state) {
  return control != nullptr && evidence != nullptr && state != nullptr &&
         g2::ReadAt(mem, runtime.call.control, control) &&
         g2::ReadAt(mem, runtime.call.evidence, evidence) &&
         g2::ReadAt(mem, runtime.runtime, state);
}

bool OriginalTargetsMatch(int mem, const Runtime& runtime) {
  constexpr std::uintptr_t kRvas[protocol::kHookCount] = {
      adapter::kPhysicsIntervalRva, adapter::kFinalWriterRva,
      adapter::kFrameEventRva};
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index) {
    const std::size_t size = HookPatchSize(index);
    std::uint8_t observed[16]{};
    if (pread(mem, observed, size,
              static_cast<off_t>(runtime.call.game_base + kRvas[index])) !=
            static_cast<ssize_t>(size) ||
        std::memcmp(observed, kExpectedPrologues[index], size) != 0)
      return false;
  }
  return true;
}

bool PreloadedFreshValid(const protocol::Control& control,
                         const protocol::Evidence& evidence,
                         const adapter::State& state) {
  if (std::memcmp(control.magic, protocol::kControlMagic,
                  sizeof(protocol::kControlMagic)) != 0 ||
      control.version != protocol::kVersion ||
      control.size != sizeof(protocol::Control) || control.enabled != 0 ||
      control.frame_limit != 0 || control.generation != 0 ||
      control.completed != 0 || control.active_helpers != 0 ||
      control.reserved0 != 0 || control.session_id != 0 ||
      control.expected_begin_owner != 0 ||
      control.expected_begin_owner_vptr != 0 ||
      control.expected_interval_owner != 0 ||
      control.expected_interval_owner_vptr != 0 ||
      control.expected_player != 0 || control.expected_player_vptr != 0 ||
      control.lifecycle_state_address != 0 || control.game_base != 0 ||
      control.begin_continuation != 0 ||
      control.begin_branch_destination != 0)
    return false;
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index)
    if (control.target_entry[index] != 0 || control.target_tail[index] != 0 ||
        control.wrapper[index] != 0)
      return false;
  for (std::uint64_t value : control.reserved)
    if (value != 0) return false;
  constexpr std::uint64_t kFreshFlags =
      protocol::kNoGameplayWrites | protocol::kNoExternalPerFrameStop;
  if (std::memcmp(evidence.magic, protocol::kEvidenceMagic,
                  sizeof(protocol::kEvidenceMagic)) != 0 ||
      evidence.version != protocol::kVersion ||
      evidence.size != sizeof(protocol::Evidence) ||
      evidence.status != protocol::kPassive || evidence.first_error != 0 ||
      evidence.flags != kFreshFlags || evidence.install_calls != 0 ||
      evidence.restore_calls != 0 || evidence.ignored_events != 0 ||
      evidence.core_faults != 0 || evidence.recursive_entries != 0 ||
      evidence.published_ticks != 0 || state.receipt_count != 0 ||
      state.complete != 0 || state.coordinator.tick != 0 ||
      state.coordinator.failures != 0)
    return false;
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index)
    if (evidence.wrapper_entries[index] != 0 ||
        evidence.qualified_events[index] != 0 ||
        evidence.first_tid[index] != 0 || evidence.last_tid[index] != 0)
      return false;
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
      control.reserved0 != 0 || control.session_id != 0 ||
      control.expected_begin_owner != 0 ||
      control.expected_begin_owner_vptr != 0 ||
      control.expected_interval_owner != 0 ||
      control.expected_interval_owner_vptr != 0 ||
      control.expected_player != 0 || control.expected_player_vptr != 0 ||
      control.lifecycle_state_address != 0 ||
      control.game_base != runtime.call.game_base ||
      control.begin_continuation != 0 ||
      control.begin_branch_destination != 0)
    return false;
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index) {
    const std::uintptr_t expected_target =
        runtime.call.game_base +
        (index == 0 ? adapter::kPhysicsIntervalRva
         : index == 1 ? adapter::kFinalWriterRva
                      : adapter::kFrameEventRva);
    if (control.target_entry[index] != expected_target ||
        control.target_tail[index] != expected_target + HookPatchSize(index) ||
        control.wrapper[index] == 0)
      return false;
  }
  for (std::uint64_t value : control.reserved)
    if (value != 0) return false;
  return true;
}

bool PassiveReceiptValid(const protocol::Control& control,
                         const protocol::Evidence& evidence,
                         const adapter::State& state) {
  constexpr std::uint64_t kRequired =
      protocol::kGameIdentity | protocol::kAllPrologues |
      protocol::kNoGameplayWrites | protocol::kNoExternalPerFrameStop |
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
      state.receipt_count != 0 || state.complete != 0 ||
      state.coordinator.tick != 0 || state.coordinator.failures != 0)
    return false;
  for (std::uint32_t index = 0; index < protocol::kHookCount; ++index) {
    if (evidence.qualified_events[index] != 0 ||
        evidence.first_tid[index] != 0 || evidence.last_tid[index] != 0)
      return false;
  }
  return control.enabled == 0 && control.completed == 0 &&
         control.active_helpers == 0;
}

bool CompleteReceiptValid(const protocol::Control& control,
                          const protocol::Evidence& evidence,
                          const adapter::State& state, std::uint32_t limit) {
  if (control.completed != 1 || control.enabled != 0 ||
      control.active_helpers != 0 ||
      (evidence.status != protocol::kComplete &&
       evidence.status != protocol::kRestored) ||
      evidence.first_error != 0 ||
      evidence.core_faults != 0 || evidence.published_ticks != limit ||
      state.complete != 1 || state.receipt_count != limit ||
      state.coordinator.ticks_published != limit ||
      state.coordinator.ticks_begun != limit ||
      state.coordinator.pre_physics_events != limit ||
      state.coordinator.physics_interval_calls < limit ||
      state.tick_begin_entries != limit ||
      state.pre_physics_entries != limit ||
      state.physics_interval_calls < limit ||
      state.coordinator.tick != limit ||
      state.coordinator.tick_phase != coordinator::TickPhase::kClosed ||
      state.coordinator.failures != 0)
    return false;
  for (std::uint32_t index = 0; index < limit; ++index) {
    const coordinator::TickScratch& receipt = state.receipts[index];
    if (receipt.tick != index ||
        receipt.session_id != control.session_id ||
        receipt.generation != control.generation ||
        receipt.boundary_flags != coordinator::kCompleteTickBoundaryMask ||
        receipt.selected_packet_index != coordinator::kNoPacket ||
        receipt.begin_tid == 0 || receipt.pre_physics_tid == 0 ||
        receipt.final_writer_tid == 0 || receipt.end_tid == 0 ||
        receipt.physics_interval_calls == 0)
      return false;
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
  return control.begin_continuation == 0;
}

bool PassiveTargetMatches(int mem, const protocol::Control& control) {
  return TargetsMatch(mem, control, false);
}

int FailG3(pid_t pid, g2::FrozenSet* frozen, bool uncertain,
           const char* stage, int code) {
  bool detached = true;
  if (frozen != nullptr) detached = g2::DetachAll(frozen);
  bool killed = false;
  if (uncertain || !detached) killed = KillUncertainProcess(pid);
  std::fprintf(stderr,
      "G3_TICK_COORDINATOR_CONTROLLER passed=0 stage=%s code=%d "
      "uncertain=%d detached=%d process_killed=%d errno=%d\n",
      stage, code, uncertain ? 1 : 0, detached ? 1 : 0,
      killed ? 1 : 0, errno);
  return code;
}

}  // namespace g3_controller_v1

int main(int argc, char** argv) {
  using namespace g3_controller_v1;
  if (argc != 9 || std::strcmp(argv[8], kAcknowledgement) != 0) {
    std::fprintf(stderr,
        "usage: %s ACTION PID START_TICKS GAME_BASE_HEX STEP_OWNER_HEX LIMIT "
        "OUTPUT I_ACCEPT_G3_TICK_COORDINATOR_V1\n", argv[0]);
    return 2;
  }
  Action action{};
  std::uint64_t pid_raw = 0, ticks = 0, game_base = 0, outer_owner = 0;
  std::uint64_t limit_raw = 0;
  if (!ParseAction(argv[1], &action) ||
      !g2::ParseNumber(argv[2], 10, &pid_raw) ||
      !g2::ParseNumber(argv[3], 10, &ticks) ||
      !g2::ParseNumber(argv[4], 16, &game_base) ||
      !g2::ParseNumber(argv[5], 16, &outer_owner) ||
      !g2::ParseNumber(argv[6], 10, &limit_raw) ||
      pid_raw == 0 || pid_raw > INT32_MAX || ticks == 0 || game_base == 0 ||
      outer_owner == 0 ||
      (limit_raw != 5 && limit_raw != 60 && limit_raw != 900) ||
      access(argv[7], F_OK) == 0)
    return 2;
  const pid_t pid = static_cast<pid_t>(pid_raw);
  const std::uint32_t limit = static_cast<std::uint32_t>(limit_raw);
  Runtime runtime{};
  if (!ResolveArtifacts(pid, ticks, game_base, &runtime))
    return FailG3(pid, nullptr, false, "artifact_runtime", 3);

  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
  const int mem = open(mem_path,
      ((action == Action::kStatus || action == Action::kPassiveStatus ||
        action == Action::kDiagnose)
           ? O_RDONLY
           : O_RDWR) |
          O_CLOEXEC);
  if (mem < 0) return FailG3(pid, nullptr, false, "open_mem", 4);

  if (action == Action::kStatus) {
    protocol::Control control{};
    protocol::Evidence evidence{};
    adapter::State state{};
    const bool read = ReadReceipt(mem, runtime, &control, &evidence, &state);
    const bool complete = read && StaticControlValid(control, runtime, limit) &&
                          CompleteReceiptValid(control, evidence, state, limit) &&
                          TargetsMatch(mem, control, true);
    FILE* report = std::fopen(argv[7], "wb");
    const bool written = report != nullptr &&
        std::fprintf(report,
            "G3_STATUS complete=%d ticks=%u begin=%" PRIu64
            " interval=%" PRIu64 " interval_calls=%" PRIu64
            " final=%" PRIu64 " end=%" PRIu64
            " ignored=%" PRIu64 " error=%u\n",
            complete ? 1 : 0, state.receipt_count,
            state.tick_begin_entries, state.pre_physics_entries,
            state.physics_interval_calls,
            evidence.qualified_events[1], evidence.qualified_events[2],
            evidence.ignored_events, evidence.first_error) > 0 &&
        std::fclose(report) == 0;
    if (report == nullptr) {}
    close(mem);
    std::printf(
        "G3_TICK_COORDINATOR_STATUS passed=%d complete=%d ticks=%u "
        "begin=%" PRIu64 " interval=%" PRIu64
        " interval_calls=%" PRIu64 " final=%" PRIu64
        " end=%" PRIu64 " entries=%" PRIu64 ",%" PRIu64 ",%" PRIu64
        " ignored=%" PRIu64 " lifecycle=%u "
        "objects=0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64
        " vptrs=0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64
        " error=%u fault_hook=%u fault_phase=%" PRIu64
        " fault_result=%" PRId64 " fault_tick=%" PRIu64 "\n",
        complete && written ? 1 : 0, complete ? 1 : 0, state.receipt_count,
        state.tick_begin_entries, state.pre_physics_entries,
        state.physics_interval_calls,
        evidence.qualified_events[1], evidence.qualified_events[2],
        evidence.wrapper_entries[0], evidence.wrapper_entries[1],
        evidence.wrapper_entries[2],
        evidence.ignored_events, evidence.last_lifecycle_state,
        evidence.last_object[0], evidence.last_object[1],
        evidence.last_object[2], evidence.last_vptr[0],
        evidence.last_vptr[1], evidence.last_vptr[2],
        evidence.first_error, evidence.reserved0, evidence.reserved1[0],
        static_cast<std::int64_t>(evidence.reserved1[1]),
        evidence.reserved1[2]);
    return complete && written ? 0 : 10;
  }

  if (action == Action::kPassiveStatus) {
    protocol::Control control{};
    protocol::Evidence evidence{};
    adapter::State state{};
    const bool read = ReadReceipt(mem, runtime, &control, &evidence, &state);
    const bool passive = read && PassiveControlValid(control, runtime) &&
                         PassiveReceiptValid(control, evidence, state) &&
                         PassiveTargetMatches(mem, control);
    FILE* report = std::fopen(argv[7], "wb");
    const bool written = report != nullptr &&
        std::fprintf(report,
            "G3_PASSIVE installed=%d entries=%" PRIu64 ",%" PRIu64
            ",%" PRIu64 " error=%u\n",
            passive ? 1 : 0, evidence.wrapper_entries[0],
            evidence.wrapper_entries[1], evidence.wrapper_entries[2],
            evidence.first_error) > 0 &&
        std::fclose(report) == 0;
    close(mem);
    std::printf(
        "G3_PASSIVE_STATUS passed=%d installed=%d entries=%" PRIu64
        ",%" PRIu64 ",%" PRIu64 " error=%u\n",
        passive && written ? 1 : 0, passive ? 1 : 0,
        evidence.wrapper_entries[0], evidence.wrapper_entries[1],
        evidence.wrapper_entries[2], evidence.first_error);
    return passive && written ? 0 : 14;
  }

  if (action == Action::kDiagnose) {
    const bool resolved = ResolveInstallObjects(pid, outer_owner, &runtime);
    close(mem);
    std::printf(
        "G3_OBJECT_DIAG passed=%d begin_owner=0x%" PRIxPTR
        " interval=0x%" PRIxPTR " player=0x%" PRIxPTR
        " lifecycle=0x%" PRIxPTR "\n",
        resolved ? 1 : 0, runtime.begin_owner, runtime.interval_owner,
        runtime.player, runtime.lifecycle_object);
    return resolved ? 0 : 13;
  }

  const pid_t call_tid = UniqueSignalCatcher(pid);
  g2::FrozenSet frozen{};
  if (call_tid <= 0 || !g2::FreezeStable(pid, call_tid, &frozen)) {
    close(mem);
    return FailG3(pid, &frozen, false, "freeze", 6);
  }

  // Object resolution must use a mapping snapshot taken after the complete
  // process is stopped.  This keeps lifecycle/player identity and the
  // StepOptions-to-implementation owner binding in one stable snapshot.
  protocol::Control existing{};
  protocol::Evidence existing_evidence{};
  adapter::State existing_state{};
  bool restoring_passive = false;
  if (action == Action::kInstallPassive) {
    runtime.call.maps = ReadMaps(pid);
    if (runtime.call.maps.empty() ||
        !ReadReceipt(mem, runtime, &existing, &existing_evidence,
                     &existing_state) ||
        !PreloadedFreshValid(existing, existing_evidence, existing_state) ||
        !OriginalTargetsMatch(mem, runtime)) {
      close(mem);
      return FailG3(pid, &frozen, false, "preloaded_precondition", 5);
    }
  } else if (action == Action::kArm) {
    runtime.call.maps = ReadMaps(pid);
    if (runtime.call.maps.empty() ||
        !ResolveInstallObjects(pid, outer_owner, &runtime)) {
      close(mem);
      return FailG3(pid, &frozen, false, "object_resolution", 5);
    }
    if (!ReadReceipt(mem, runtime, &existing, &existing_evidence,
                     &existing_state) ||
        !PassiveControlValid(existing, runtime) ||
        !PassiveReceiptValid(existing, existing_evidence, existing_state) ||
        !PassiveTargetMatches(mem, existing)) {
      close(mem);
      return FailG3(pid, &frozen, true, "passive_precondition", 5);
    }
  } else {
    if (!g2::ReadAt(mem, runtime.call.control, &existing)) {
      close(mem);
      return FailG3(pid, &frozen, false, "restore_control", 5);
    }
    restoring_passive = PassiveControlValid(existing, runtime);
    if (!restoring_passive && !StaticControlValid(existing, runtime, limit)) {
      close(mem);
      return FailG3(pid, &frozen, false, "restore_control", 5);
    }
  }

  bool command_ok = false;
  protocol::Control control{};
  protocol::Evidence evidence{};
  adapter::State state{};
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
  } else if (action == Action::kArm) {
    std::uintptr_t interval_vptr = 0, player_vptr = 0;
    std::uint32_t phase = 0;
    const bool stopped_identity =
        g2::ReadAt(mem, runtime.interval_owner, &interval_vptr) &&
        interval_vptr == runtime.interval_owner_vptr &&
        g2::ReadAt(mem, runtime.player, &player_vptr) &&
        player_vptr == runtime.player_vptr &&
        g2::ReadAt(mem, runtime.lifecycle_state, &phase) && phase == 2u;
    if (!stopped_identity) {
      close(mem);
      return FailG3(pid, &frozen, false, "stopped_identity", 7);
    }
    control = existing;
    control.frame_limit = limit;
    control.generation = 1;
    control.session_id = (ticks << 16) ^ static_cast<std::uint64_t>(pid);
    if (control.session_id == 0) control.session_id = 1;
    control.expected_begin_owner = runtime.begin_owner;
    control.expected_begin_owner_vptr = runtime.begin_owner_vptr;
    control.expected_interval_owner = runtime.interval_owner;
    control.expected_interval_owner_vptr = runtime.interval_owner_vptr;
    control.expected_player = runtime.player;
    control.expected_player_vptr = runtime.player_vptr;
    control.lifecycle_state_address = runtime.lifecycle_state;
    if (!g2::WriteExact(mem, runtime.call.control, &control, sizeof(control))) {
      close(mem);
      return FailG3(pid, &frozen, false, "configure", 8);
    }
    guest_called = g2::CallGuest(pid, &runtime.call, &frozen,
        static_cast<std::uint64_t>(protocol::Command::kArmSession),
        kArmTag, &guest_return, &rip_bias);
    receipt_read = ReadReceipt(mem, runtime, &control, &evidence, &state);
    static_valid = receipt_read && StaticControlValid(control, runtime, limit);
    target_valid = receipt_read && TargetsMatch(mem, control, true);
    begin_valid = receipt_read &&
                  control.expected_begin_owner == runtime.begin_owner &&
                  control.expected_begin_owner_vptr ==
                      runtime.begin_owner_vptr;
    command_ok = guest_called && receipt_read && static_valid &&
        control.enabled == 1 && control.completed == 0 &&
        control.active_helpers == 0 &&
        evidence.status == protocol::kArmed && evidence.first_error == 0 &&
        target_valid && begin_valid;
  } else {
    if (!(restoring_passive ? PassiveTargetMatches(mem, existing)
                            : TargetsMatch(mem, existing, true)) ||
        existing.active_helpers != 0) {
      close(mem);
      return FailG3(pid, &frozen, false, "restore_precondition", 7);
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
                   control.expected_begin_owner ==
                       control.expected_interval_owner &&
                   control.expected_begin_owner_vptr ==
                       control.expected_interval_owner_vptr);
    command_ok = guest_called && receipt_read && static_valid &&
        control.enabled == 0 && control.active_helpers == 0 &&
        evidence.status == protocol::kRestored &&
        (existing.completed == 0 ||
         CompleteReceiptValid(control, evidence, state, limit)) &&
        target_valid && begin_valid;
  }
  if (!command_ok) {
    std::fprintf(stderr,
        "G3_COMMAND_DIAG action=%u guest_called=%d guest=0x%" PRIx64
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
    close(mem);
    const bool authoritative_install =
        action == Action::kInstallPassive && guest_called && receipt_read &&
        target_valid && evidence.status == protocol::kPassiveInstalled &&
        evidence.first_error == 0 && control.enabled == 0 &&
        control.active_helpers == 0;
    return FailG3(pid, &frozen, !authoritative_install,
                  "guest_command", 9);
  }
  const std::size_t frozen_count =
      frozen.other_tids.size() + (frozen.call_attached ? 1u : 0u);
  const bool detached = g2::DetachAll(&frozen);
  FILE* report = std::fopen(argv[7], "wb");
  const bool written = report != nullptr &&
      std::fprintf(report,
          "G3_ACTION action=%u pid=%d limit=%u guest=0x%" PRIx64
          " rip_bias=%ld status=%d ticks=%u detached=%d\n",
          static_cast<unsigned>(action), pid, limit, guest_return, rip_bias,
          evidence.status, state.receipt_count, detached ? 1 : 0) > 0 &&
      std::fclose(report) == 0;
  if (report == nullptr) {}
  close(mem);
  if (!detached) return FailG3(pid, nullptr, true, "detach", 11);
  std::printf(
      "G3_TICK_COORDINATOR_CONTROLLER passed=%d action=%u pid=%d limit=%u "
      "status=%d ticks=%u frozen=%zu rollback=1 detach=1\n",
      written ? 1 : 0, static_cast<unsigned>(action), pid, limit,
      evidence.status, state.receipt_count,
      frozen_count);
  return written ? 0 : 12;
}
