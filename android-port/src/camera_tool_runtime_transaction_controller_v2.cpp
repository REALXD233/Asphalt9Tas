// Host transaction for the phase-aligned Camera Tool v2 payload.  It reuses
// the already live-proven RaceView node resolver and exact stop/write/restore
// primitives.  Runtime input changes are handled by a separate payload-only
// stream and never patch the callback slot.

#define A9TAS_CAMERA_TOOL_TRANSACTION_NO_MAIN
#include "camera_tool_transaction_controller_v1.cpp"
#undef A9TAS_CAMERA_TOOL_TRANSACTION_NO_MAIN

#include "camera_tool_runtime_protocol_v2.h"
#include "camera_build_profile_v1.h"
#include "vehicle_state_resolver_v1.h"

namespace runtime_camera = a9tas::camera_tool_runtime_v2;
namespace vehicle = a9tas::vehicle_state_v1;
namespace camera_profile = a9tas::camera_build_profile_v1;

namespace camera_tool_runtime_txn_v2 {
namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_CAMERA_TOOL_PHASE_ALIGNED_SINGLE_SLOT_V2";
constexpr char kPayloadBasename[] =
    "liba9tas_camera_tool_runtime_v2_build_only.so";
constexpr char kPayloadSha256[] =
    "71f125a8e79e305e57995b4eb10a580657a3c0154b13ac475f3a36f3e5f3315d";
// Shared HUD visibility query wrapper.  The input stream installs one aligned
// entry branch; the wrapper emulates the original leaf for non-target objects.
constexpr std::uintptr_t kHudVisibilityWrapperRva = 0x1E80;
constexpr std::uintptr_t kWrapperRva = 0x1F4C;
constexpr std::size_t kWrapperSize = 0xB80;
constexpr std::uintptr_t kControlPointerRva = 0x77C0;
constexpr std::uintptr_t kEvidencePointerRva = 0x7940;
constexpr std::uintptr_t kControlStorageRva = 0x6600;
constexpr std::uintptr_t kEvidenceStorageRva = 0x7800;
constexpr std::uintptr_t kManagerSourceOffset = 0xD8;
constexpr std::uint8_t kWrapperSignature[24] = {
    0xff,0xc3,0x05,0xd1,0xef,0x3b,0x0d,0x6d,
    0xed,0x33,0x0e,0x6d,0xeb,0x2b,0x0f,0x6d,
    0xe9,0x23,0x10,0x6d,0xfd,0x7b,0x11,0xa9,
};

enum class Action : std::uint32_t {
  kInstall = 1,
  kStatus = 2,
  kDeactivate = 3,
  kUninstall = 4,
};

enum ReportFlag : std::uint32_t {
  kIdentity = 1u << 0,
  kPayload = 1u << 1,
  kNode = 1u << 2,
  kVehicle = 1u << 3,
  kConfiguredFlag = 1u << 4,
  kStopped = 1u << 5,
  kInstalled = 1u << 6,
  kOriginalRestored = 1u << 7,
  kResumed = 1u << 8,
  kOutput = 1u << 9,
};

#pragma pack(push, 1)
struct Report {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t action;
  std::uint32_t flags;
  std::uint64_t pid;
  std::uint64_t start_ticks;
  std::uint64_t game_base;
  std::uint64_t payload_base;
  std::uint64_t manager;
  std::uint64_t shape;
  std::uint64_t node;
  std::uint64_t node_vptr;
  std::uint64_t original_callback;
  std::uint64_t wrapper;
  std::uint64_t hud_visibility_wrapper;
  std::uint64_t control;
  std::uint64_t evidence_address;
  std::uint64_t embedded_vptr;
  std::uint64_t combined_function;
  std::uint64_t source;
  std::uint64_t source_vptr;
  std::uint64_t position_setter;
  std::uint64_t rotation_setter;
  std::uint64_t fov_setter;
  std::uint64_t vehicle_physics_base;
  std::uint64_t vehicle_backend_interface;
  std::uint64_t vehicle_velocity_interface;
  std::uint64_t vehicle_native_body;
  std::uint64_t vehicle_native_vptr;
  std::uint64_t payload_write_attempts;
  std::uint64_t game_write_attempts;
  std::uint64_t rollback_attempts;
  runtime_camera::Evidence evidence;
};
#pragma pack(pop)

static_assert(sizeof(Report) == 568);

struct PayloadLayout {
  std::uintptr_t base{};
  std::uintptr_t wrapper{};
  std::uintptr_t hud_visibility_wrapper{};
  std::uintptr_t control{};
  std::uintptr_t evidence{};
  std::string path;
};

bool ParseAction(const char* text, Action* output) {
  if (text == nullptr || output == nullptr) return false;
  if (std::strcmp(text, "install") == 0) *output = Action::kInstall;
  else if (std::strcmp(text, "status") == 0) *output = Action::kStatus;
  else if (std::strcmp(text, "deactivate") == 0)
    *output = Action::kDeactivate;
  else if (std::strcmp(text, "uninstall") == 0)
    *output = Action::kUninstall;
  else return false;
  return true;
}

bool ResolvePayload(int mem, const std::vector<Mapping>& mappings,
                    PayloadLayout* output) {
  std::uintptr_t base = UINTPTR_MAX;
  std::string path;
  std::uint32_t matches = 0;
  for (const auto& mapping : mappings) {
    if (mapping.path.find(kPayloadBasename) == std::string::npos) continue;
    if (mapping.path.find(" (deleted)") != std::string::npos) return false;
    if (path.empty()) path = mapping.path;
    if (mapping.path != path) return false;
    if (mapping.file_offset == 0) base = std::min(base, mapping.begin);
    ++matches;
  }
  if (matches < 2 || base == UINTPTR_MAX || path.empty()) return false;
  PayloadLayout layout{};
  layout.base = base;
  layout.path = path;
  layout.wrapper = base + kWrapperRva;
  layout.hud_visibility_wrapper = base + kHudVisibilityWrapperRva;
  std::uint8_t signature[sizeof(kWrapperSignature)]{};
  if (!ReadExact(mem, layout.wrapper, signature, sizeof(signature)) ||
      std::memcmp(signature, kWrapperSignature, sizeof(signature)) != 0 ||
      !ReadExact(mem, base + kControlPointerRva,
                 &layout.control, sizeof(layout.control)) ||
      !ReadExact(mem, base + kEvidencePointerRva,
                 &layout.evidence, sizeof(layout.evidence)))
    return false;
  const Mapping* wrapper_map = MappingAt(mappings, layout.wrapper, kWrapperSize);
  const Mapping* control_map = MappingAt(
      mappings, layout.control, sizeof(runtime_camera::Control));
  const Mapping* evidence_map = MappingAt(
      mappings, layout.evidence, sizeof(runtime_camera::Evidence));
  if (wrapper_map == nullptr || wrapper_map->path != path ||
      wrapper_map->permissions[0] != 'r' ||
      wrapper_map->permissions[1] == 'w' ||
      // LDPlayer NativeBridge exposes ARM guest code as host r--p rather than
      // host executable memory.  Exact path, payload hash/RVAs, full wrapper
      // extent and entry signature remain pinned by the host workflow.
      !PrivateObject(control_map) || !PrivateObject(evidence_map) ||
      control_map->permissions[1] != 'w' ||
      evidence_map->permissions[1] != 'w' ||
      control_map->path != path || evidence_map->path != path ||
      layout.control != base + kControlStorageRva ||
      layout.evidence != base + kEvidenceStorageRva ||
      (layout.control & 63u) != 0 || (layout.evidence & 63u) != 0)
    return false;
  *output = layout;
  return true;
}

bool EvidenceHeader(const runtime_camera::Evidence& evidence) {
  return std::memcmp(evidence.magic, runtime_camera::kEvidenceMagic, 8) == 0 &&
      evidence.version == runtime_camera::kVersion &&
      evidence.size == sizeof(runtime_camera::Evidence);
}

bool ConfiguredIdentity(const runtime_camera::Control& control,
                        std::uintptr_t manager, const NodeSnapshot& node,
                        std::uintptr_t shape, const Report& expected) {
  return std::memcmp(control.magic, runtime_camera::kControlMagic, 8) == 0 &&
      control.version == runtime_camera::kVersion &&
      control.size == sizeof(runtime_camera::Control) &&
      (control.flags == runtime_camera::kConfigured ||
       control.flags == (runtime_camera::kConfigured | runtime_camera::kActive)) &&
      control.expected_manager == manager && control.expected_node == node.node &&
      control.expected_shape == shape &&
      control.original_callback == expected.original_callback &&
      control.expected_node_vptr == node.vptr &&
      control.expected_embedded_vptr == expected.embedded_vptr &&
      control.expected_combined_function == expected.combined_function &&
      control.expected_source == expected.source &&
      control.expected_source_vptr == expected.source_vptr &&
      control.expected_position_setter == expected.position_setter &&
      control.expected_rotation_setter == expected.rotation_setter &&
      control.expected_fov_setter == expected.fov_setter &&
      control.expected_hud_visibility_wrapper ==
          expected.hud_visibility_wrapper &&
      control.vehicle_physics_base == expected.vehicle_physics_base &&
      control.vehicle_backend_interface == expected.vehicle_backend_interface &&
      control.vehicle_velocity_interface == expected.vehicle_velocity_interface &&
      control.vehicle_native_body == expected.vehicle_native_body &&
      control.vehicle_native_vptr == expected.vehicle_native_vptr;
}

bool WriteOutput(const char* path, Report* report) {
  if (path == nullptr || *path == '\0' || access(path, F_OK) == 0) return false;
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) return false;
  report->flags |= kOutput;
  const bool ok = std::fwrite(report, sizeof(*report), 1, file) == 1 &&
      std::fflush(file) == 0 && std::ferror(file) == 0;
  const bool close_ok = std::fclose(file) == 0;
  if (!ok || !close_ok) {
    std::remove(path);
    return false;
  }
  return true;
}

}  // namespace

int CameraToolRuntimeMain(int argc, char** argv) {
  if (argc != 10 || std::strcmp(argv[9], kAcknowledgement) != 0) {
    std::fprintf(stderr,
        "usage: %s ACTION PID START_TICKS GAME_BASE_HEX MANAGER_HEX "
        "SHAPE_HEX PROFILE OUTPUT "
        "I_ACCEPT_CAMERA_TOOL_PHASE_ALIGNED_SINGLE_SLOT_V2\n",
        argv[0]);
    return 2;
  }
  Action action{};
  std::uint64_t values[5]{};
  const int bases[5] = {10,10,16,16,16};
  if (!ParseAction(argv[1], &action)) return 2;
  for (int index = 0; index < 5; ++index)
    if (!ParseUnsigned(argv[index + 2], bases[index], &values[index])) return 2;
  if (values[0] == 0 || values[0] > INT32_MAX || values[1] == 0 ||
      values[2] == 0 || values[3] == 0 || values[4] == 0 ||
      access(argv[8], F_OK) == 0) return 2;
  camera_profile::Profile profile{};
  if (!camera_profile::Load(argv[7], &profile)) return 2;

  Report report{};
  std::memcpy(report.magic, "A9CTT2", 7);
  report.version = 2;
  report.size = sizeof(report);
  report.action = static_cast<std::uint32_t>(action);
  report.pid = values[0];
  report.start_ticks = values[1];
  report.game_base = values[2];
  report.manager = values[3];
  report.shape = values[4];

  const pid_t pid = static_cast<pid_t>(values[0]);
  std::uint64_t observed_ticks = 0;
  if (!ReadStartTicks(pid, &observed_ticks) || observed_ticks != values[1])
    return 3;
  report.flags |= kIdentity;
  std::vector<Mapping> mappings;
  if (!ReadMappings(pid, &mappings)) return 3;
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
  const int mem = open(mem_path,
      (action == Action::kStatus ? O_RDONLY : O_RDWR) | O_CLOEXEC);
  if (mem < 0) return 4;

  const auto game_base = static_cast<std::uintptr_t>(values[2]);
  const auto manager = static_cast<std::uintptr_t>(values[3]);
  const auto shape = static_cast<std::uintptr_t>(values[4]);
  PayloadLayout payload{};
  NodeSnapshot node{};
  if (!ResolvePayload(mem, mappings, &payload) ||
      !ReadNode(mem, mappings, game_base, manager, shape, &node,
                profile.callback_node_vptr_rva)) {
    close(mem); return 3;
  }
  report.flags |= kPayload | kNode;
  report.payload_base = payload.base;
  report.wrapper = payload.wrapper;
  report.hud_visibility_wrapper = payload.hud_visibility_wrapper;
  report.control = payload.control;
  report.evidence_address = payload.evidence;
  report.node = node.node;
  report.node_vptr = node.vptr;
  report.original_callback = game_base + profile.callback_rva;
  report.embedded_vptr = game_base + profile.embedded_vptr_rva;
  report.combined_function = game_base + profile.combined_function_rva;
  report.source_vptr = game_base + profile.source_vptr_rva;
  report.position_setter = game_base + profile.position_setter_rva;
  report.rotation_setter = game_base + profile.rotation_setter_rva;
  report.fov_setter = game_base + profile.fov_setter_rva;
  if (node.function != report.original_callback && node.function != payload.wrapper) {
    close(mem); return 3;
  }
  if (action == Action::kInstall && node.function != report.original_callback) {
    close(mem); return 5;
  }
  if (action != Action::kInstall && action != Action::kUninstall &&
      node.function != payload.wrapper) {
    close(mem); return 5;
  }

  std::uintptr_t live_embedded_vptr = 0;
  std::uintptr_t live_combined = 0;
  std::uintptr_t live_source = 0;
  std::uintptr_t live_source_vptr = 0;
  std::uintptr_t live_position_setter = 0;
  std::uintptr_t live_rotation_setter = 0;
  if (!ReadExact(mem, manager + 0x08, &live_embedded_vptr,
                 sizeof(live_embedded_vptr)) ||
      live_embedded_vptr != report.embedded_vptr ||
      !ReadExact(mem, live_embedded_vptr + 0x40, &live_combined,
                 sizeof(live_combined)) ||
      live_combined != report.combined_function ||
      !ReadExact(mem, manager + kManagerSourceOffset, &live_source,
                 sizeof(live_source)) || live_source == 0 ||
      !ReadExact(mem, live_source, &live_source_vptr,
                 sizeof(live_source_vptr)) ||
      live_source_vptr != report.source_vptr ||
      !ReadExact(mem, live_source_vptr + 0x88, &live_position_setter,
                 sizeof(live_position_setter)) ||
      live_position_setter != report.position_setter ||
      !ReadExact(mem, live_source_vptr + 0x90, &live_rotation_setter,
                 sizeof(live_rotation_setter)) ||
      live_rotation_setter != report.rotation_setter) {
    close(mem); return 3;
  }
  report.source = live_source;

  vehicle::Layout vehicle_layout{};
  vehicle::BackendLayout backend{};
  if (action == Action::kInstall) {
    if (!vehicle::Resolve(pid, mem, game_base, profile.vehicle,
                          &vehicle_layout) ||
        !vehicle::ResolveBackendLayout(mem, game_base, vehicle_layout,
                                       profile.vehicle, &backend)) {
      close(mem); return 3;
    }
    report.vehicle_physics_base = vehicle_layout.physics_base;
    report.vehicle_backend_interface = backend.interface;
    report.vehicle_velocity_interface = backend.physics_velocity_interface;
    report.vehicle_native_body = backend.native_body;
    report.vehicle_native_vptr = backend.native_body_vtable;
    report.flags |= kVehicle;
  }

  runtime_camera::Control control{};
  runtime_camera::Evidence evidence{};
  if (action == Action::kInstall) {
    std::memcpy(control.magic, runtime_camera::kControlMagic, 8);
    control.version = runtime_camera::kVersion;
    control.size = sizeof(control);
    control.flags = runtime_camera::kConfigured;
    control.expected_manager = manager;
    control.expected_node = node.node;
    control.expected_shape = shape;
    control.original_callback = report.original_callback;
    control.expected_node_vptr = node.vptr;
    control.expected_embedded_vptr = report.embedded_vptr;
    control.expected_combined_function = report.combined_function;
    control.expected_source = report.source;
    control.expected_source_vptr = report.source_vptr;
    control.expected_position_setter = report.position_setter;
    control.expected_rotation_setter = report.rotation_setter;
    control.expected_fov_setter = report.fov_setter;
    control.expected_hud_visibility_wrapper = report.hud_visibility_wrapper;
    control.published_command = 0;
    for (auto& command : control.commands) {
      command.mode = runtime_camera::Mode::kFreeFlight;
      command.move_speed = 100.0f;
      command.sensitivity = 0.2f;
      command.orbital_distance = 5.0f;
      command.orbital_zoom_speed = 1.0f;
      command.fov_radians = 0.959931076f;
      command.absolute_override_flags = runtime_camera::kAbsoluteOverrideMask;
      command.absolute_rotation[3] = 1.0f;
    }
    control.vehicle_physics_base = report.vehicle_physics_base;
    control.vehicle_backend_interface = report.vehicle_backend_interface;
    control.vehicle_velocity_interface = report.vehicle_velocity_interface;
    control.vehicle_native_body = report.vehicle_native_body;
    control.vehicle_native_vptr = report.vehicle_native_vptr;
    std::memcpy(evidence.magic, runtime_camera::kEvidenceMagic, 8);
    evidence.version = runtime_camera::kVersion;
    evidence.size = sizeof(evidence);
    evidence.last_status = runtime_camera::kPassive;
    evidence.last_mode = runtime_camera::Mode::kAbsolute;
    evidence.natural_transform[6] = 1.0f;
    evidence.applied_transform[6] = 1.0f;
    report.payload_write_attempts = 2;
    if (!WriteExactVerified(mem, payload.evidence, &evidence, sizeof(evidence)) ||
        !WriteExactVerified(mem, payload.control, &control, sizeof(control))) {
      close(mem); return 6;
    }
    report.flags |= kConfiguredFlag;
    if (!StopProcess(pid)) { (void)ResumeProcess(pid); close(mem); return 7; }
    report.flags |= kStopped;
    std::vector<Mapping> stopped_maps;
    NodeSnapshot stopped_node{};
    const bool pinned = ReadStartTicks(pid, &observed_ticks) &&
        observed_ticks == values[1] && ReadMappings(pid, &stopped_maps) &&
        ReadNode(mem, stopped_maps, game_base, manager, shape, &stopped_node,
                 profile.callback_node_vptr_rva) &&
        stopped_node.node == node.node && stopped_node.vptr == node.vptr &&
        stopped_node.function == report.original_callback;
    bool installed = false;
    if (pinned) {
      ++report.game_write_attempts;
      installed = WriteExactVerified(mem,
          node.node + callback::kCallbackOffset, &payload.wrapper,
          sizeof(payload.wrapper));
    }
    if (!installed) {
      (void)ResumeProcess(pid); close(mem); return 8;
    }
    report.flags |= kInstalled;
    if (!ResumeProcess(pid)) {
      std::uintptr_t current = 0;
      bool restored = ReadExact(mem,
          node.node + callback::kCallbackOffset, &current, sizeof(current));
      if (restored && current == payload.wrapper) {
        ++report.game_write_attempts;
        ++report.rollback_attempts;
        restored = WriteExactVerified(mem,
            node.node + callback::kCallbackOffset,
            &report.original_callback, sizeof(report.original_callback));
      } else if (restored) {
        restored = current == report.original_callback;
      }
      if (restored) report.flags |= kOriginalRestored;
      const bool recovery_resumed = ResumeProcess(pid);
      if (recovery_resumed) report.flags |= kResumed;
      close(mem);
      return restored && recovery_resumed ? 9 : 13;
    }
    report.flags |= kResumed;
  } else {
    if (!ReadExact(mem, payload.control, &control, sizeof(control)) ||
        !ReadExact(mem, payload.evidence, &evidence, sizeof(evidence)) ||
        !EvidenceHeader(evidence)) {
      close(mem); return 6;
    }
    report.vehicle_physics_base = control.vehicle_physics_base;
    report.vehicle_backend_interface = control.vehicle_backend_interface;
    report.vehicle_velocity_interface = control.vehicle_velocity_interface;
    report.vehicle_native_body = control.vehicle_native_body;
    report.vehicle_native_vptr = control.vehicle_native_vptr;
    if (report.vehicle_physics_base == 0 ||
        report.vehicle_backend_interface == 0 ||
        report.vehicle_velocity_interface == 0 ||
        report.vehicle_native_body == 0 || report.vehicle_native_vptr == 0 ||
        !ConfiguredIdentity(control, manager, node, shape, report)) {
      close(mem); return 6;
    }
    report.flags |= kVehicle;
    report.flags |= kConfiguredFlag;
    if (action == Action::kStatus) {
      report.evidence = evidence;
      const bool ok = WriteOutput(argv[8], &report);
      close(mem);
      std::printf("CAMERA_TOOL_RUNTIME_STATUS active=%u entries=%" PRIu64
                  " steps=%" PRIu64 " vehicle_reads=%" PRIu64
                  " failures=%" PRIu64 " payload_sha256=%s\n",
                  (control.flags & runtime_camera::kActive) != 0 ? 1u : 0u,
                  evidence.wrapper_entries, evidence.runtime_steps,
                  evidence.vehicle_reads, evidence.failures, kPayloadSha256);
      return ok ? 0 : 10;
    }
    if (action == Action::kDeactivate) {
      const std::uint32_t inactive = runtime_camera::kConfigured;
      ++report.payload_write_attempts;
      if (!WriteExactVerified(mem,
          payload.control + offsetof(runtime_camera::Control, flags),
          &inactive, sizeof(inactive))) {
        close(mem); return 6;
      }
    } else if (action == Action::kUninstall) {
      if (!StopProcess(pid)) { (void)ResumeProcess(pid); close(mem); return 7; }
      report.flags |= kStopped;
      std::vector<Mapping> stopped_maps;
      NodeSnapshot stopped_node{};
      bool restored = ReadMappings(pid, &stopped_maps) &&
          ReadNode(mem, stopped_maps, game_base, manager, shape, &stopped_node,
                   profile.callback_node_vptr_rva) &&
          stopped_node.node == node.node && stopped_node.vptr == node.vptr;
      if (restored && stopped_node.function == payload.wrapper) {
        ++report.game_write_attempts;
        ++report.rollback_attempts;
        restored = WriteExactVerified(mem,
            node.node + callback::kCallbackOffset,
            &report.original_callback, sizeof(report.original_callback));
      } else if (restored) {
        restored = stopped_node.function == report.original_callback;
      }
      const std::uint32_t disabled = 0;
      ++report.payload_write_attempts;
      restored = restored && WriteExactVerified(mem,
          payload.control + offsetof(runtime_camera::Control, flags),
          &disabled, sizeof(disabled));
      if (restored) report.flags |= kOriginalRestored;
      const bool resumed = ResumeProcess(pid);
      if (resumed) report.flags |= kResumed;
      if (!restored || !resumed) { close(mem); return 12; }
    }
  }

  (void)ReadExact(mem, payload.evidence, &report.evidence,
                  sizeof(report.evidence));
  const bool ok = WriteOutput(argv[8], &report);
  close(mem);
  std::printf("CAMERA_TOOL_RUNTIME_TRANSACTION action=%u flags=0x%x "
              "node=0x%" PRIx64 " wrapper=0x%" PRIx64
              " control=0x%" PRIx64 " evidence=0x%" PRIx64
              " payload_writes=%" PRIu64 " game_writes=%" PRIu64
              " payload_sha256=%s output=%s\n",
              report.action, report.flags, report.node, report.wrapper,
              report.control, report.evidence_address,
              report.payload_write_attempts, report.game_write_attempts,
              kPayloadSha256, argv[8]);
  return ok ? 0 : 10;
}

}  // namespace camera_tool_runtime_txn_v2

int main(int argc, char** argv) {
  return camera_tool_runtime_txn_v2::CameraToolRuntimeMain(argc, argv);
}
