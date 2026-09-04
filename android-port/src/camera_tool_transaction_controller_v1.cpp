// Host transaction for the standalone Camera Tool.  It reuses the already
// live-proven RaceView resolver, process identity, stop/resume, exact write and
// conditional callback-slot restoration primitives.

#define main a9tas_record_transaction_embedded_main_v1
#include "camera_raceview_record_transaction_controller_v1.cpp"
#undef main

#include "camera_tool_protocol_v1.h"

#include <cmath>

namespace camera = a9tas::camera_tool_v1;

namespace camera_tool_txn {
namespace {

constexpr char kAcknowledgement[] = "I_ACCEPT_CAMERA_TOOL_SINGLE_SLOT_V1";
constexpr char kPayloadBasename[] =
    "liba9tas_camera_tool_v1_build_only.so";
constexpr char kPayloadSha256[] =
    "7b2fad97e599943ff6c41d67611caeeb82d517fae5545c1e3f228371b3bdf5c8";
constexpr std::uintptr_t kWrapperRva = 0x1A50;
constexpr std::uintptr_t kControlPointerRva = 0x5000;
constexpr std::uintptr_t kEvidencePointerRva = 0x5140;
constexpr std::uintptr_t kControlStorageRva = 0x4F40;
constexpr std::uintptr_t kEvidenceStorageRva = 0x5040;
constexpr std::uintptr_t kEmbeddedVptrRva = 0x7F0F8A8;
constexpr std::uintptr_t kCombinedFunctionRva = 0x4D10D90;
constexpr std::uintptr_t kSourceVptrRva = 0x9D5B768;
constexpr std::uintptr_t kPositionSetterRva = 0x4BF4D28;
constexpr std::uintptr_t kRotationSetterRva = 0x4BF4D7C;
constexpr std::uintptr_t kFovSetterRva = 0x4C711F8;
constexpr std::uintptr_t kManagerSourceOffset = 0xD8;
constexpr std::uint8_t kWrapperSignature[24] = {
    0xff,0xc3,0x03,0xd1,0xe8,0x43,0x00,0xfd,
    0xfd,0x7b,0x09,0xa9,0xfc,0x6f,0x0a,0xa9,
    0xfa,0x67,0x0b,0xa9,0xf8,0x5f,0x0c,0xa9,
};

enum class Action : std::uint32_t {
  kInstall = 1,
  kStatus = 2,
  kActivate = 3,
  kDeactivate = 4,
  kUninstall = 5,
};

enum ReportFlag : std::uint32_t {
  kIdentity = 1u << 0,
  kPayload = 1u << 1,
  kNode = 1u << 2,
  kConfiguredFlag = 1u << 3,
  kStopped = 1u << 4,
  kInstalled = 1u << 5,
  kCommandPublished = 1u << 6,
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
  std::uint64_t control;
  std::uint64_t evidence_address;
  std::uint64_t embedded_vptr;
  std::uint64_t combined_function;
  std::uint64_t source;
  std::uint64_t source_vptr;
  std::uint64_t position_setter;
  std::uint64_t rotation_setter;
  std::uint64_t fov_setter;
  std::uint32_t override_flags;
  std::uint32_t reserved0;
  std::uint64_t command_sequence;
  float position[3];
  float rotation[4];
  float fov_radians;
  std::uint64_t payload_write_attempts;
  std::uint64_t game_write_attempts;
  std::uint64_t rollback_attempts;
  camera::Evidence evidence;
};
#pragma pack(pop)

static_assert(sizeof(Report) == 504);

struct PayloadLayout {
  std::uintptr_t base{};
  std::uintptr_t wrapper{};
  std::uintptr_t control{};
  std::uintptr_t evidence{};
  std::string path;
};

bool ParseAction(const char* text, Action* output) {
  if (text == nullptr || output == nullptr) return false;
  if (std::strcmp(text, "install") == 0) *output = Action::kInstall;
  else if (std::strcmp(text, "status") == 0) *output = Action::kStatus;
  else if (std::strcmp(text, "activate") == 0) *output = Action::kActivate;
  else if (std::strcmp(text, "deactivate") == 0) *output = Action::kDeactivate;
  else if (std::strcmp(text, "uninstall") == 0) *output = Action::kUninstall;
  else return false;
  return true;
}

bool ParseFloat(const char* text, float* output) {
  if (text == nullptr || output == nullptr || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const float value = std::strtof(text, &end);
  if (errno != 0 || end == text || *end != '\0' || !std::isfinite(value))
    return false;
  *output = value;
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
  const std::uintptr_t control_pointer = base + kControlPointerRva;
  const std::uintptr_t evidence_pointer = base + kEvidencePointerRva;
  std::uint8_t signature[sizeof(kWrapperSignature)]{};
  if (!ReadExact(mem, layout.wrapper, signature, sizeof(signature)) ||
      std::memcmp(signature, kWrapperSignature, sizeof(signature)) != 0 ||
      !ReadExact(mem, control_pointer, &layout.control, sizeof(layout.control)) ||
      !ReadExact(mem, evidence_pointer, &layout.evidence, sizeof(layout.evidence)))
    return false;
  const Mapping* wrapper_map = MappingAt(mappings, layout.wrapper,
                                         sizeof(signature));
  const Mapping* control_map = MappingAt(mappings, layout.control,
                                         sizeof(camera::Control));
  const Mapping* evidence_map = MappingAt(mappings, layout.evidence,
                                          sizeof(camera::Evidence));
  if (wrapper_map == nullptr || wrapper_map->path != path ||
      wrapper_map->permissions[0] != 'r' || wrapper_map->permissions[1] == 'w' ||
      !PrivateObject(control_map) || !PrivateObject(evidence_map) ||
      control_map->permissions[1] != 'w' || evidence_map->permissions[1] != 'w' ||
      control_map->path != path || evidence_map->path != path ||
      layout.control != base + kControlStorageRva ||
      layout.evidence != base + kEvidenceStorageRva ||
      (layout.control & 63u) != 0 || (layout.evidence & 63u) != 0)
    return false;
  *output = layout;
  return true;
}

bool EvidenceHeader(const camera::Evidence& evidence) {
  return std::memcmp(evidence.magic, camera::kEvidenceMagic, 8) == 0 &&
      evidence.version == camera::kVersion &&
      evidence.size == sizeof(camera::Evidence);
}

bool ConfiguredIdentity(const camera::Control& control,
                        std::uintptr_t manager, const NodeSnapshot& node,
                        std::uintptr_t shape, std::uintptr_t original,
                        std::uintptr_t embedded_vptr,
                        std::uintptr_t combined_function,
                        std::uintptr_t source,
                        std::uintptr_t source_vptr,
                        std::uintptr_t position_setter,
                        std::uintptr_t rotation_setter,
                        std::uintptr_t fov_setter) {
  return std::memcmp(control.magic, camera::kControlMagic, 8) == 0 &&
      control.version == camera::kVersion &&
      control.size == sizeof(camera::Control) &&
      (control.flags == camera::kConfigured ||
       control.flags == (camera::kConfigured | camera::kActive)) &&
      control.expected_manager == manager &&
      control.expected_node == node.node &&
      control.expected_shape == shape &&
      control.original_callback == original &&
      control.expected_node_vptr == node.vptr &&
      control.expected_embedded_vptr == embedded_vptr &&
      control.expected_combined_function == combined_function &&
      control.expected_source == source &&
      control.expected_source_vptr == source_vptr &&
      control.expected_position_setter == position_setter &&
      control.expected_rotation_setter == rotation_setter &&
      control.expected_fov_setter == fov_setter &&
      (control.command_sequence & 1u) == 0;
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

int CameraToolMain(int argc, char** argv) {
  const bool base_argv = argc == 9;
  const bool activate_argv = argc == 18;
  if ((!base_argv && !activate_argv) ||
      std::strcmp(argv[argc - 1], kAcknowledgement) != 0) {
    std::fprintf(stderr,
        "usage: %s ACTION PID START_TICKS GAME_BASE_HEX MANAGER_HEX "
        "SHAPE_HEX OUTPUT [FLAGS PX PY PZ QX QY QZ QW FOV] "
        "I_ACCEPT_CAMERA_TOOL_SINGLE_SLOT_V1\n", argv[0]);
    return 2;
  }
  Action action{};
  std::uint64_t values[5]{};
  const int bases[5] = {10,10,16,16,16};
  if (!ParseAction(argv[1], &action) ||
      (action == Action::kActivate) != activate_argv) return 2;
  for (int index = 0; index < 5; ++index)
    if (!ParseUnsigned(argv[index + 2], bases[index], &values[index])) return 2;
  if (values[0] == 0 || values[0] > INT32_MAX || values[1] == 0 ||
      values[2] == 0 || values[3] == 0 || values[4] == 0 ||
      access(argv[7], F_OK) == 0) return 2;

  Report report{};
  std::memcpy(report.magic, "A9CTTR1", 8);
  report.version = 1;
  report.size = sizeof(report);
  report.action = static_cast<std::uint32_t>(action);
  report.pid = values[0];
  report.start_ticks = values[1];
  report.game_base = values[2];
  report.manager = values[3];
  report.shape = values[4];
  if (action == Action::kActivate) {
    std::uint64_t parsed_flags = 0;
    if (!ParseUnsigned(argv[8], 0, &parsed_flags) || parsed_flags == 0 ||
        parsed_flags > camera::kAbsoluteOverrideMask) return 2;
    report.override_flags = static_cast<std::uint32_t>(parsed_flags);
    for (int index = 0; index < 3; ++index)
      if (!ParseFloat(argv[9 + index], &report.position[index])) return 2;
    for (int index = 0; index < 4; ++index)
      if (!ParseFloat(argv[12 + index], &report.rotation[index])) return 2;
    if (!ParseFloat(argv[16], &report.fov_radians)) return 2;
  }

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
      !ReadNode(mem, mappings, game_base, manager, shape, &node)) {
    close(mem);
    return 3;
  }
  report.flags |= kPayload | kNode;
  report.payload_base = payload.base;
  report.wrapper = payload.wrapper;
  report.control = payload.control;
  report.evidence_address = payload.evidence;
  report.node = node.node;
  report.node_vptr = node.vptr;
  report.original_callback = game_base + callback::kExpectedCallbackRva;
  report.embedded_vptr = game_base + kEmbeddedVptrRva;
  report.combined_function = game_base + kCombinedFunctionRva;
  report.source_vptr = game_base + kSourceVptrRva;
  report.position_setter = game_base + kPositionSetterRva;
  report.rotation_setter = game_base + kRotationSetterRva;
  report.fov_setter = game_base + kFovSetterRva;
  if (node.function != report.original_callback && node.function != payload.wrapper) {
    close(mem);
    return 3;
  }
  if (action == Action::kInstall && node.function != report.original_callback) {
    close(mem);
    return 5;
  }
  if (action != Action::kInstall && action != Action::kUninstall &&
      node.function != payload.wrapper) {
    close(mem);
    return 5;
  }
  std::uintptr_t live_embedded_vptr = 0;
  std::uintptr_t live_combined_function = 0;
  std::uintptr_t live_source = 0;
  std::uintptr_t live_source_vptr = 0;
  std::uintptr_t live_position_setter = 0;
  std::uintptr_t live_rotation_setter = 0;
  if (!ReadExact(mem, manager + 0x08, &live_embedded_vptr,
                 sizeof(live_embedded_vptr)) ||
      live_embedded_vptr != report.embedded_vptr ||
      !ReadExact(mem, live_embedded_vptr + 0x40, &live_combined_function,
                 sizeof(live_combined_function)) ||
      live_combined_function != report.combined_function ||
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
    close(mem);
    return 3;
  }
  report.source = live_source;

  camera::Control control{};
  camera::Evidence evidence{};
  if (action == Action::kInstall) {
    std::memcpy(control.magic, camera::kControlMagic, 8);
    control.version = camera::kVersion;
    control.size = sizeof(control);
    control.flags = camera::kConfigured;
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
    control.command_sequence = 2;
    control.rotation[3] = 1.0f;
    control.fov_radians = 1.0f;
    std::memcpy(evidence.magic, camera::kEvidenceMagic, 8);
    evidence.version = camera::kVersion;
    evidence.size = sizeof(evidence);
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
        ReadNode(mem, stopped_maps, game_base, manager, shape, &stopped_node) &&
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
      std::uintptr_t current_function = 0;
      bool restored = ReadExact(mem,
          node.node + callback::kCallbackOffset,
          &current_function, sizeof(current_function));
      if (restored && current_function == payload.wrapper) {
        ++report.game_write_attempts;
        ++report.rollback_attempts;
        restored = WriteExactVerified(mem,
            node.node + callback::kCallbackOffset,
            &report.original_callback, sizeof(report.original_callback));
      } else if (restored) {
        restored = current_function == report.original_callback;
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
        !EvidenceHeader(evidence) ||
        !ConfiguredIdentity(control, manager, node, shape,
                            report.original_callback, report.embedded_vptr,
                            report.combined_function, report.source,
                            report.source_vptr, report.position_setter,
                            report.rotation_setter, report.fov_setter)) {
      close(mem); return 6;
    }
    report.flags |= kConfiguredFlag;
    report.command_sequence = control.command_sequence;

    if (action == Action::kStatus) {
      report.evidence = evidence;
      const bool ok = WriteOutput(argv[7], &report);
      close(mem);
      std::printf("CAMERA_TOOL_STATUS active=%u sequence=%" PRIu64
                  " entries=%" PRIu64 " failures=%" PRIu64
                  " payload_sha256=%s\n",
                  (control.flags & camera::kActive) != 0 ? 1u : 0u,
                  control.command_sequence, evidence.wrapper_entries,
                  evidence.failures, kPayloadSha256);
      return ok ? 0 : 10;
    }

    if (action == Action::kActivate) {
      const std::uint64_t odd = control.command_sequence + 1;
      const std::uint64_t even = control.command_sequence + 2;
      if (even <= control.command_sequence || (odd & 1u) == 0 ||
          (even & 1u) != 0) { close(mem); return 6; }
      ++report.payload_write_attempts;
      bool updated = WriteExactVerified(mem,
          payload.control + offsetof(camera::Control, command_sequence),
          &odd, sizeof(odd));
      ++report.payload_write_attempts;
      updated = updated && WriteExactVerified(mem,
          payload.control + offsetof(camera::Control, override_flags),
          &report.override_flags, sizeof(report.override_flags));
      struct TargetBytes {
        float position[3];
        float rotation[4];
        float fov;
      } target{};
      static_assert(sizeof(TargetBytes) == 32);
      std::memcpy(target.position, report.position, sizeof(target.position));
      std::memcpy(target.rotation, report.rotation, sizeof(target.rotation));
      target.fov = report.fov_radians;
      ++report.payload_write_attempts;
      updated = updated && WriteExactVerified(mem,
          payload.control + offsetof(camera::Control, position),
          &target, sizeof(target));
      ++report.payload_write_attempts;
      updated = updated && WriteExactVerified(mem,
          payload.control + offsetof(camera::Control, command_sequence),
          &even, sizeof(even));
      const std::uint32_t active_flags = camera::kConfigured | camera::kActive;
      ++report.payload_write_attempts;
      updated = updated && WriteExactVerified(mem,
          payload.control + offsetof(camera::Control, flags),
          &active_flags, sizeof(active_flags));
      if (!updated) { close(mem); return 6; }
      report.flags |= kCommandPublished;
      report.command_sequence = even;
    } else if (action == Action::kDeactivate) {
      const std::uint32_t inactive_flags = camera::kConfigured;
      ++report.payload_write_attempts;
      if (!WriteExactVerified(mem,
          payload.control + offsetof(camera::Control, flags),
          &inactive_flags, sizeof(inactive_flags))) {
        close(mem); return 6;
      }
      report.flags |= kCommandPublished;
    } else if (action == Action::kUninstall) {
      if (!StopProcess(pid)) { (void)ResumeProcess(pid); close(mem); return 7; }
      report.flags |= kStopped;
      std::vector<Mapping> stopped_maps;
      NodeSnapshot stopped_node{};
      bool restored = ReadMappings(pid, &stopped_maps) &&
          ReadNode(mem, stopped_maps, game_base, manager, shape, &stopped_node) &&
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
      const std::uint32_t disabled_flags = 0;
      ++report.payload_write_attempts;
      restored = restored && WriteExactVerified(mem,
          payload.control + offsetof(camera::Control, flags),
          &disabled_flags, sizeof(disabled_flags));
      if (restored) report.flags |= kOriginalRestored;
      const bool resumed = ResumeProcess(pid);
      if (resumed) report.flags |= kResumed;
      if (!restored || !resumed) { close(mem); return 12; }
    }
  }

  (void)ReadExact(mem, payload.evidence, &report.evidence,
                  sizeof(report.evidence));
  const bool ok = WriteOutput(argv[7], &report);
  close(mem);
  std::printf("CAMERA_TOOL_TRANSACTION action=%u flags=0x%x node=0x%" PRIx64
              " original=0x%" PRIx64 " wrapper=0x%" PRIx64
              " sequence=%" PRIu64 " payload_writes=%" PRIu64
              " game_writes=%" PRIu64 " payload_sha256=%s output=%s\n",
              report.action, report.flags, report.node,
              report.original_callback, report.wrapper,
              report.command_sequence, report.payload_write_attempts,
              report.game_write_attempts, kPayloadSha256, argv[7]);
  return ok ? 0 : 10;
}

}  // namespace camera_tool_txn

#ifndef A9TAS_CAMERA_TOOL_TRANSACTION_NO_MAIN
int main(int argc, char** argv) {
  return camera_tool_txn::CameraToolMain(argc, argv);
}
#endif
