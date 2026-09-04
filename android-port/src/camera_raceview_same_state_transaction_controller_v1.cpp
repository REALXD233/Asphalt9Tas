// Same-state five-callback single-slot transaction. Reuse the already live-
// proven process/mapping/stop/restore primitives from the record transaction;
// this file supplies a distinct hash-pinned payload ABI and report.

#define main a9tas_record_transaction_embedded_main_v1
#include "camera_raceview_record_transaction_controller_v1.cpp"
#undef main

#include "camera_raceview_same_state_protocol_v1.h"

namespace same = a9tas::camera_raceview_same_state_v1;

namespace {

constexpr char kSameAcknowledgement[] =
    "I_ACCEPT_RACEVIEW_SAME_STATE_SINGLE_SLOT_V1";
constexpr char kSamePayloadBasename[] =
    "liba9tas_camera_raceview_same_state_v1_build_only.so";
constexpr char kSamePayloadSha256[] =
    "58e888e4fb606fe3ec1b497c97a2a64c0b4dfc7dbd647ad0c01fa0558efd6ddf";
constexpr std::uintptr_t kSameWrapperRva = 0x1AB0;
constexpr std::uintptr_t kSameControlPointerRva = 0x4DC0;
constexpr std::uintptr_t kSameEvidencePointerRva = 0x4E80;
constexpr std::uintptr_t kSameFramesPointerRva = 0x4E88;
constexpr std::uintptr_t kSameControlStorageRva = 0x4D40;
constexpr std::uintptr_t kSameEvidenceStorageRva = 0x4E00;
constexpr std::uintptr_t kSameFramesStorageRva = 0x4EC0;
constexpr std::uintptr_t kEmbeddedVptrRva = 0x7F0F8A8;
constexpr std::uintptr_t kCombinedFunctionRva = 0x4D10D90;
constexpr std::uint8_t kSameWrapperSignature[24] = {
    0xff,0xc3,0x01,0xd1,0xfd,0x7b,0x01,0xa9,
    0xfb,0x13,0x00,0xf9,0xfa,0x67,0x03,0xa9,
    0xf8,0x5f,0x04,0xa9,0xf6,0x57,0x05,0xa9,
};

enum class SameAction : std::uint32_t {
  kInstall = 1, kStatus = 2, kFinalize = 3, kRollback = 4,
};

#pragma pack(push, 1)
struct SameReport {
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
  std::uint64_t frames;
  std::uint64_t embedded_vptr;
  std::uint64_t combined_function;
  std::uint32_t requested_frames;
  std::uint32_t captured_frames;
  std::uint64_t payload_write_attempts;
  std::uint64_t game_write_attempts;
  std::uint64_t rollback_attempts;
  std::uint64_t read_errors;
  std::uint64_t semantic_errors;
  same::Evidence evidence;
};
#pragma pack(pop)

struct SamePayloadLayout {
  std::uintptr_t base{};
  std::uintptr_t wrapper{};
  std::uintptr_t control{};
  std::uintptr_t evidence{};
  std::uintptr_t frames{};
  std::string path;
};

bool ParseSameAction(const char* text, SameAction* output) {
  if (text == nullptr || output == nullptr) return false;
  if (std::strcmp(text, "install") == 0) *output = SameAction::kInstall;
  else if (std::strcmp(text, "status") == 0) *output = SameAction::kStatus;
  else if (std::strcmp(text, "finalize") == 0) *output = SameAction::kFinalize;
  else if (std::strcmp(text, "rollback") == 0) *output = SameAction::kRollback;
  else return false;
  return true;
}

bool ResolveSamePayload(int mem, const std::vector<Mapping>& mappings,
                        SamePayloadLayout* output) {
  std::uintptr_t base = UINTPTR_MAX;
  std::string path;
  std::uint32_t matches = 0;
  for (const auto& mapping : mappings) {
    if (mapping.path.find(kSamePayloadBasename) == std::string::npos) continue;
    if (mapping.path.find(" (deleted)") != std::string::npos) return false;
    if (path.empty()) path = mapping.path;
    if (mapping.path != path) return false;
    if (mapping.file_offset == 0) base = std::min(base, mapping.begin);
    ++matches;
  }
  if (matches < 2 || base == UINTPTR_MAX) return false;
  SamePayloadLayout layout{};
  layout.base = base;
  layout.path = path;
  layout.wrapper = base + kSameWrapperRva;
  const std::uintptr_t control_pointer = base + kSameControlPointerRva;
  const std::uintptr_t evidence_pointer = base + kSameEvidencePointerRva;
  const std::uintptr_t frames_pointer = base + kSameFramesPointerRva;
  std::uint8_t signature[sizeof(kSameWrapperSignature)]{};
  if (!ReadExact(mem, layout.wrapper, signature, sizeof(signature)) ||
      std::memcmp(signature, kSameWrapperSignature, sizeof(signature)) != 0 ||
      !ReadExact(mem, control_pointer, &layout.control, sizeof(layout.control)) ||
      !ReadExact(mem, evidence_pointer, &layout.evidence, sizeof(layout.evidence)) ||
      !ReadExact(mem, frames_pointer, &layout.frames, sizeof(layout.frames)))
    return false;
  const Mapping* wrapper_map = MappingAt(mappings, layout.wrapper, sizeof(signature));
  const Mapping* control_map = MappingAt(mappings, layout.control, sizeof(same::Control));
  const Mapping* evidence_map = MappingAt(mappings, layout.evidence, sizeof(same::Evidence));
  const bool frame_range = PrivateWritableRange(
      mappings, layout.frames, sizeof(same::Frame) * same::kMaximumFrames, path);
  if (wrapper_map == nullptr || wrapper_map->path != path ||
      wrapper_map->permissions[0] != 'r' || wrapper_map->permissions[1] == 'w' ||
      !PrivateObject(control_map) || !PrivateObject(evidence_map) || !frame_range ||
      control_map->permissions[1] != 'w' || evidence_map->permissions[1] != 'w' ||
      control_map->path != path || evidence_map->path != path ||
      layout.control != base + kSameControlStorageRva ||
      layout.evidence != base + kSameEvidenceStorageRva ||
      layout.frames != base + kSameFramesStorageRva ||
      (layout.control & 63u) != 0 || (layout.evidence & 63u) != 0 ||
      (layout.frames & 63u) != 0)
    return false;
  *output = layout;
  return true;
}

bool SameEvidenceHeader(const same::Evidence& evidence) {
  return std::memcmp(evidence.magic, same::kEvidenceMagic, 8) == 0 &&
      evidence.version == same::kVersion && evidence.size == sizeof(evidence);
}

bool WriteSameOutput(const char* path, SameReport* report, int mem,
                     std::uintptr_t frames, bool include_frames) {
  if (path == nullptr || *path == '\0' || access(path, F_OK) == 0) return false;
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) return false;
  report->flags |= kOutputWritten;
  bool ok = std::fwrite(report, sizeof(*report), 1, file) == 1;
  std::array<same::Frame, same::kMaximumFrames> local_frames{};
  if (ok && include_frames) {
    ok = ReadExact(mem, frames, local_frames.data(), sizeof(local_frames)) &&
        std::fwrite(local_frames.data(), sizeof(local_frames), 1, file) == 1;
  }
  ok = ok && std::fflush(file) == 0 && std::ferror(file) == 0;
  const bool close_ok = std::fclose(file) == 0;
  if (!ok || !close_ok) {
    report->flags &= ~kOutputWritten;
    std::remove(path);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 9 || std::strcmp(argv[8], kSameAcknowledgement) != 0) {
    std::fprintf(stderr,
        "usage: %s ACTION PID START_TICKS GAME_BASE_HEX MANAGER_HEX "
        "SHAPE_HEX OUTPUT I_ACCEPT_RACEVIEW_SAME_STATE_SINGLE_SLOT_V1\n",
        argv[0]);
    return 2;
  }
  SameAction action{};
  std::uint64_t values[5]{};
  const int bases[5] = {10,10,16,16,16};
  if (!ParseSameAction(argv[1], &action)) return 2;
  for (int i = 0; i < 5; ++i)
    if (!ParseUnsigned(argv[i + 2], bases[i], &values[i])) return 2;
  if (values[0] == 0 || values[0] > INT32_MAX || values[1] == 0 ||
      values[2] == 0 || values[3] == 0 || values[4] == 0 ||
      access(argv[7], F_OK) == 0) return 2;

  const pid_t process_id = static_cast<pid_t>(values[0]);
  const std::uint64_t start_ticks = values[1];
  const auto game_base = static_cast<std::uintptr_t>(values[2]);
  const auto manager = static_cast<std::uintptr_t>(values[3]);
  const auto shape = static_cast<std::uintptr_t>(values[4]);
  SameReport report{};
  std::memcpy(report.magic, "A9CSTR1", 8);
  report.version = 1;
  report.size = sizeof(report);
  report.action = static_cast<std::uint32_t>(action);
  report.pid = values[0];
  report.start_ticks = start_ticks;
  report.game_base = game_base;
  report.manager = manager;
  report.shape = shape;
  report.requested_frames = same::kMaximumFrames;

  std::uint64_t observed_ticks = 0;
  if (!ReadStartTicks(process_id, &observed_ticks) || observed_ticks != start_ticks)
    return 3;
  report.flags |= kProcessIdentity;
  std::vector<Mapping> mappings;
  if (!ReadMappings(process_id, &mappings)) return 3;
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", process_id);
  const int mem = open(mem_path,
      (action == SameAction::kStatus ? O_RDONLY : O_RDWR) | O_CLOEXEC);
  if (mem < 0) return 4;
  SamePayloadLayout payload{};
  NodeSnapshot node{};
  if (!ResolveSamePayload(mem, mappings, &payload) ||
      !ReadNode(mem, mappings, game_base, manager, shape, &node)) {
    close(mem);
    return 3;
  }
  report.flags |= kPayloadResolved | kNodeIdentity;
  report.payload_base = payload.base;
  report.wrapper = payload.wrapper;
  report.control = payload.control;
  report.evidence_address = payload.evidence;
  report.frames = payload.frames;
  report.node = node.node;
  report.node_vptr = node.vptr;
  report.original_callback = game_base + callback::kExpectedCallbackRva;
  report.embedded_vptr = game_base + kEmbeddedVptrRva;
  report.combined_function = game_base + kCombinedFunctionRva;
  if (node.function != report.original_callback && node.function != payload.wrapper) {
    close(mem);
    return 3;
  }
  std::uintptr_t live_embedded_vptr = 0;
  std::uintptr_t live_combined_function = 0;
  if (!ReadExact(mem, manager + 0x08, &live_embedded_vptr,
                 sizeof(live_embedded_vptr)) ||
      live_embedded_vptr != report.embedded_vptr ||
      !ReadExact(mem, live_embedded_vptr + 0x40, &live_combined_function,
                 sizeof(live_combined_function)) ||
      live_combined_function != report.combined_function) {
    close(mem);
    return 3;
  }

  same::Control control{};
  same::Evidence evidence{};
  if (action == SameAction::kInstall) {
    if (node.function != report.original_callback) { close(mem); return 5; }
    std::memcpy(control.magic, same::kControlMagic, 8);
    control.version = same::kVersion;
    control.size = sizeof(control);
    control.flags = same::kConfigured;
    control.frame_count = same::kMaximumFrames;
    control.expected_manager = manager;
    control.expected_node = node.node;
    control.expected_shape = shape;
    control.original_callback = report.original_callback;
    control.expected_node_vptr = node.vptr;
    control.expected_embedded_vptr = report.embedded_vptr;
    control.expected_combined_function = report.combined_function;
    std::memcpy(evidence.magic, same::kEvidenceMagic, 8);
    evidence.version = same::kVersion;
    evidence.size = sizeof(evidence);
    report.payload_write_attempts = 2;
    if (!WriteExactVerified(mem, payload.evidence, &evidence, sizeof(evidence)) ||
        !WriteExactVerified(mem, payload.control, &control, sizeof(control))) {
      close(mem); return 6;
    }
    report.flags |= kPayloadConfigured;
    if (!StopProcess(process_id)) { (void)ResumeProcess(process_id); close(mem); return 7; }
    report.flags |= kProcessStopped;
    std::vector<Mapping> stopped_maps;
    NodeSnapshot stopped_node{};
    const bool pinned = ReadStartTicks(process_id, &observed_ticks) &&
        observed_ticks == start_ticks && ReadMappings(process_id, &stopped_maps) &&
        ReadNode(mem, stopped_maps, game_base, manager, shape, &stopped_node) &&
        stopped_node.node == node.node && stopped_node.vptr == node.vptr &&
        stopped_node.function == report.original_callback;
    bool installed = false;
    if (pinned) {
      ++report.game_write_attempts;
      installed = WriteExactVerified(mem, node.node + callback::kCallbackOffset,
                                     &payload.wrapper, sizeof(payload.wrapper));
    }
    if (!installed) {
      std::uintptr_t current = 0;
      if (ReadExact(mem, node.node + callback::kCallbackOffset, &current, sizeof(current)) &&
          current == payload.wrapper) {
        ++report.rollback_attempts;
        (void)WriteExactVerified(mem, node.node + callback::kCallbackOffset,
                                 &report.original_callback, sizeof(report.original_callback));
      }
      (void)ResumeProcess(process_id); close(mem); return 8;
    }
    report.flags |= kSingleSlotInstalled;
    if (!ResumeProcess(process_id)) {
      std::uintptr_t current = 0;
      bool resume_failure_restored =
          ReadExact(mem, node.node + callback::kCallbackOffset,
                    &current, sizeof(current));
      if (resume_failure_restored && current == payload.wrapper) {
        ++report.game_write_attempts;
        ++report.rollback_attempts;
        resume_failure_restored = WriteExactVerified(
            mem, node.node + callback::kCallbackOffset,
            &report.original_callback, sizeof(report.original_callback));
      } else if (resume_failure_restored) {
        resume_failure_restored = current == report.original_callback;
      }
      if (resume_failure_restored) report.flags |= kOriginalSlotFinal;
      const bool recovery_resumed = ResumeProcess(process_id);
      if (recovery_resumed) report.flags |= kProcessResumed;
      close(mem);
      return resume_failure_restored && recovery_resumed ? 9 : 13;
    }
    report.flags |= kProcessResumed;
  } else {
    if (!ReadExact(mem, payload.control, &control, sizeof(control)) ||
        !ReadExact(mem, payload.evidence, &evidence, sizeof(evidence)) ||
        !SameEvidenceHeader(evidence)) { close(mem); return 6; }
    const bool configured =
        std::memcmp(control.magic, same::kControlMagic, 8) == 0 &&
        control.version == same::kVersion && control.size == sizeof(control) &&
        control.flags == same::kConfigured &&
        control.frame_count == same::kMaximumFrames &&
        control.expected_manager == manager && control.expected_node == node.node &&
        control.expected_shape == shape &&
        control.original_callback == report.original_callback &&
        control.expected_node_vptr == node.vptr &&
        control.expected_embedded_vptr == report.embedded_vptr &&
        control.expected_combined_function == report.combined_function;
    if (!configured) { close(mem); return 6; }
    report.flags |= kPayloadConfigured;
    report.evidence = evidence;
    report.captured_frames = evidence.recorded_frames;
    const bool complete = evidence.processed_frames == same::kMaximumFrames &&
        evidence.recorded_frames == same::kMaximumFrames &&
        evidence.combined_calls == same::kMaximumFrames &&
        evidence.combined_returns == same::kMaximumFrames &&
        evidence.fov_same_writes == same::kMaximumFrames &&
        evidence.failures == 0 && evidence.recursive_entries == 0 &&
        evidence.original_calls == evidence.wrapper_entries &&
        evidence.original_returns == evidence.original_calls &&
        evidence.last_status == same::kComplete;
    if (complete) report.flags |= kEvidenceComplete;
    if (action == SameAction::kStatus) {
      const bool ok = WriteSameOutput(argv[7], &report, mem, payload.frames, false);
      close(mem);
      std::printf("RACEVIEW_SAME_STATE_STATUS complete=%u captured=%u entries=%" PRIu64
                  " failures=%" PRIu64 " payload_sha256=%s\n",
                  complete ? 1u : 0u, evidence.processed_frames,
                  evidence.wrapper_entries, evidence.failures, kSamePayloadSha256);
      return ok ? 0 : 10;
    }
    if (action == SameAction::kFinalize && !complete) { close(mem); return 11; }
    if (!StopProcess(process_id)) { (void)ResumeProcess(process_id); close(mem); return 7; }
    report.flags |= kProcessStopped;
    std::vector<Mapping> stopped_maps;
    NodeSnapshot stopped_node{};
    bool restored = ReadMappings(process_id, &stopped_maps) &&
        ReadNode(mem, stopped_maps, game_base, manager, shape, &stopped_node) &&
        stopped_node.node == node.node && stopped_node.vptr == node.vptr;
    if (restored && stopped_node.function == payload.wrapper) {
      ++report.game_write_attempts;
      ++report.rollback_attempts;
      restored = WriteExactVerified(mem, node.node + callback::kCallbackOffset,
                                     &report.original_callback, sizeof(report.original_callback));
    } else if (restored) {
      restored = stopped_node.function == report.original_callback;
    }
    if (restored) report.flags |= kOriginalSlotFinal;
    const bool resumed = ResumeProcess(process_id);
    if (resumed) report.flags |= kProcessResumed;
    if (!restored || !resumed) { close(mem); return 12; }
  }
  report.evidence = evidence;
  if (action == SameAction::kInstall)
    (void)ReadExact(mem, payload.evidence, &report.evidence, sizeof(report.evidence));
  const bool ok = WriteSameOutput(argv[7], &report, mem, payload.frames,
                                  action == SameAction::kFinalize);
  close(mem);
  std::printf("RACEVIEW_SAME_STATE_TRANSACTION action=%u flags=0x%x node=0x%" PRIx64
              " original=0x%" PRIx64 " wrapper=0x%" PRIx64
              " game_writes=%" PRIu64 " payload_sha256=%s output=%s\n",
              report.action, report.flags, report.node, report.original_callback,
              report.wrapper, report.game_write_attempts, kSamePayloadSha256, argv[7]);
  return ok ? 0 : 10;
}
