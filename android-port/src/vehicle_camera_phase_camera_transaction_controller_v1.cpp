// Host-side transaction for the camera half of the dual-callback observer.
// It reuses the live-proven RaceView node/process primitives, but resolves the
// separate hash-pinned combined payload.  The only game-owned write is the
// callback slot original <-> camera_wrapper transition.

#define main a9tas_record_transaction_embedded_main_v1
#include "camera_raceview_record_transaction_controller_v1.cpp"
#undef main

#include "vehicle_camera_phase_observer_elf_resolver_v1.h"
#include "vehicle_camera_phase_observer_protocol_v1.h"

namespace phase = a9tas::vehicle_camera_phase_observer_v1;
namespace phase_elf = a9tas::vehicle_camera_phase_observer_elf_v1;

namespace {

constexpr char kPhaseAcknowledgement[] =
    "I_ACCEPT_VEHICLE_CAMERA_PHASE_SINGLE_SLOT_V1";

#pragma pack(push, 1)
struct PhaseTransactionReport {
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
  std::uint64_t camera_wrapper;
  std::uint64_t vehicle_wrapper;
  std::uint64_t phase_control;
  std::uint64_t phase_evidence;
  std::uint64_t phase_events;
  std::uint32_t maximum_events;
  std::uint32_t copied_events;
  std::uint64_t payload_write_attempts;
  std::uint64_t game_write_attempts;
  std::uint64_t rollback_attempts;
  std::uint64_t read_errors;
  std::uint64_t semantic_errors;
  phase::Evidence evidence;
};
#pragma pack(pop)

static_assert(sizeof(phase::Control) == 128);
static_assert(sizeof(phase::Evidence) == 192);
static_assert(sizeof(phase::Event) == 80);

bool PhaseControlIdentity(const phase::Control& control,
                          std::uint32_t maximum_events,
                          std::uintptr_t manager, std::uintptr_t node,
                          std::uintptr_t shape,
                          std::uintptr_t original_callback,
                          std::uintptr_t node_vptr) {
  return std::memcmp(control.magic, phase::kControlMagic, 8) == 0 &&
         control.version == phase::kVersion &&
         control.size == sizeof(control) &&
         control.flags == (phase::kConfigured | phase::kObserveVehicle |
                           phase::kObserveCamera) &&
         control.maximum_events == maximum_events &&
         control.expected_manager == manager &&
         control.expected_node == node && control.expected_shape == shape &&
         control.original_camera_callback == original_callback &&
         control.expected_node_vptr == node_vptr;
}

bool PhaseEvidenceHeader(const phase::Evidence& evidence) {
  return std::memcmp(evidence.magic, phase::kEvidenceMagic, 8) == 0 &&
         evidence.version == phase::kVersion &&
         evidence.size == sizeof(evidence);
}

bool WritePhaseOutput(const char* path, PhaseTransactionReport* report,
                      const std::vector<phase::Event>& events) {
  if (path == nullptr || *path == '\0' || report == nullptr ||
      access(path, F_OK) == 0)
    return false;
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) return false;
  report->flags |= kOutputWritten;
  bool ok = std::fwrite(report, sizeof(*report), 1, file) == 1;
  if (ok && !events.empty())
    ok = std::fwrite(events.data(), sizeof(phase::Event), events.size(), file) ==
         events.size();
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
  if (argc != 10 || std::strcmp(argv[9], kPhaseAcknowledgement) != 0) {
    std::fprintf(stderr,
        "usage: %s ACTION PID START_TICKS GAME_BASE_HEX MANAGER_HEX "
        "SHAPE_HEX MAX_EVENTS OUTPUT "
        "I_ACCEPT_VEHICLE_CAMERA_PHASE_SINGLE_SLOT_V1\n", argv[0]);
    return 2;
  }
  Action action{};
  std::uint64_t values[6]{};
  const int bases[6] = {10, 10, 16, 16, 16, 10};
  if (!ParseAction(argv[1], &action)) return 2;
  for (int index = 0; index < 6; ++index)
    if (!ParseUnsigned(argv[index + 2], bases[index], &values[index])) return 2;
  if (values[0] == 0 || values[0] > INT32_MAX || values[1] == 0 ||
      values[2] == 0 || values[3] == 0 || values[4] == 0 ||
      values[5] == 0 || values[5] > phase::kMaximumEvents ||
      access(argv[8], F_OK) == 0)
    return 2;

  const pid_t pid = static_cast<pid_t>(values[0]);
  const std::uint64_t expected_ticks = values[1];
  const auto game_base = static_cast<std::uintptr_t>(values[2]);
  const auto manager = static_cast<std::uintptr_t>(values[3]);
  const auto shape = static_cast<std::uintptr_t>(values[4]);
  const auto maximum_events = static_cast<std::uint32_t>(values[5]);

  PhaseTransactionReport report{};
  std::memcpy(report.magic, "A9VCPTR1", 8);
  report.version = 1;
  report.size = sizeof(report);
  report.action = static_cast<std::uint32_t>(action);
  report.pid = values[0];
  report.start_ticks = expected_ticks;
  report.game_base = game_base;
  report.manager = manager;
  report.shape = shape;
  report.maximum_events = maximum_events;

  std::uint64_t current_ticks = 0;
  if (!ReadStartTicks(pid, &current_ticks) || current_ticks != expected_ticks)
    return 3;
  report.flags |= kProcessIdentity;
  std::vector<Mapping> mappings;
  if (!ReadMappings(pid, &mappings)) return 3;
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                static_cast<int>(pid));
  const int mem = open(mem_path,
      (action == Action::kStatus ? O_RDONLY : O_RDWR) | O_CLOEXEC);
  if (mem < 0) return 4;

  phase_elf::Layout payload{};
  NodeSnapshot node{};
  if (!phase_elf::Resolve(pid, mem, &payload) ||
      !ReadNode(mem, mappings, game_base, manager, shape, &node)) {
    std::fprintf(stderr,
                 "phase payload or RaceView node resolution failed\n");
    close(mem);
    return 3;
  }
  report.flags |= kPayloadResolved | kNodeIdentity;
  report.payload_base = payload.load_bias;
  report.node = node.node;
  report.node_vptr = node.vptr;
  report.original_callback = game_base + callback::kExpectedCallbackRva;
  report.camera_wrapper = payload.camera_wrapper;
  report.vehicle_wrapper = payload.wrapper;
  report.phase_control = payload.phase_control;
  report.phase_evidence = payload.phase_evidence;
  report.phase_events = payload.phase_events;
  if (node.function != report.original_callback &&
      node.function != payload.camera_wrapper) {
    close(mem);
    return 3;
  }

  phase::Control control{};
  phase::Evidence evidence{};
  std::vector<phase::Event> copied_events;
  if (action == Action::kInstall) {
    phase::Control initial_control{};
    phase::Evidence initial_evidence{};
    if (node.function != report.original_callback ||
        !ReadExact(mem, payload.phase_control, &initial_control,
                   sizeof(initial_control)) ||
        !ReadExact(mem, payload.phase_evidence, &initial_evidence,
                   sizeof(initial_evidence)) ||
        std::memcmp(initial_control.magic, phase::kControlMagic, 8) != 0 ||
        initial_control.version != phase::kVersion ||
        initial_control.size != sizeof(initial_control) ||
        initial_control.flags != 0 ||
        !PhaseEvidenceHeader(initial_evidence) ||
        initial_evidence.next_sequence != 0 ||
        initial_evidence.committed_events != 0) {
      std::fprintf(stderr,
                   "phase payload is not in pristine install state\n");
      close(mem);
      return 5;
    }
    std::memcpy(control.magic, phase::kControlMagic, 8);
    control.version = phase::kVersion;
    control.size = sizeof(control);
    control.flags = phase::kConfigured | phase::kObserveVehicle |
                    phase::kObserveCamera;
    control.maximum_events = maximum_events;
    control.expected_manager = manager;
    control.expected_node = node.node;
    control.expected_shape = shape;
    control.original_camera_callback = report.original_callback;
    control.expected_node_vptr = node.vptr;
    std::memcpy(evidence.magic, phase::kEvidenceMagic, 8);
    evidence.version = phase::kVersion;
    evidence.size = sizeof(evidence);
    evidence.last_status = phase::kPassive;
    report.payload_write_attempts = 2;
    if (!WriteExactVerified(mem, payload.phase_evidence, &evidence,
                            sizeof(evidence)) ||
        !WriteExactVerified(mem, payload.phase_control, &control,
                            sizeof(control))) {
      close(mem);
      return 6;
    }
    report.flags |= kPayloadConfigured;
    if (!StopProcess(pid)) {
      (void)ResumeProcess(pid);
      close(mem);
      return 7;
    }
    report.flags |= kProcessStopped;
    std::vector<Mapping> stopped_maps;
    NodeSnapshot stopped_node{};
    const bool pinned = ReadStartTicks(pid, &current_ticks) &&
        current_ticks == expected_ticks && ReadMappings(pid, &stopped_maps) &&
        ReadNode(mem, stopped_maps, game_base, manager, shape, &stopped_node) &&
        stopped_node.node == node.node && stopped_node.vptr == node.vptr &&
        stopped_node.function == report.original_callback;
    bool installed = false;
    if (pinned) {
      ++report.game_write_attempts;
      installed = WriteExactVerified(
          mem, node.node + callback::kCallbackOffset,
          &payload.camera_wrapper, sizeof(payload.camera_wrapper));
    }
    if (!installed) {
      std::uintptr_t current_function = 0;
      if (ReadExact(mem, node.node + callback::kCallbackOffset,
                    &current_function, sizeof(current_function)) &&
          current_function == payload.camera_wrapper) {
        ++report.rollback_attempts;
        (void)WriteExactVerified(
            mem, node.node + callback::kCallbackOffset,
            &report.original_callback, sizeof(report.original_callback));
      }
      (void)ResumeProcess(pid);
      close(mem);
      return 8;
    }
    report.flags |= kSingleSlotInstalled;
    if (!ResumeProcess(pid)) {
      ++report.rollback_attempts;
      (void)WriteExactVerified(mem, node.node + callback::kCallbackOffset,
                               &report.original_callback,
                               sizeof(report.original_callback));
      (void)ResumeProcess(pid);
      close(mem);
      return 9;
    }
    report.flags |= kProcessResumed;
    (void)ReadExact(mem, payload.phase_evidence, &report.evidence,
                    sizeof(report.evidence));
  } else {
    if (!ReadExact(mem, payload.phase_control, &control, sizeof(control)) ||
        !ReadExact(mem, payload.phase_evidence, &evidence, sizeof(evidence)) ||
        !PhaseEvidenceHeader(evidence) ||
        !PhaseControlIdentity(control, maximum_events, manager, node.node,
                              shape, report.original_callback, node.vptr)) {
      close(mem);
      return 6;
    }
    report.flags |= kPayloadConfigured;
    report.evidence = evidence;
    if (evidence.camera_failures == 0 &&
        evidence.camera_recursive_entries == 0 &&
        evidence.camera_original_calls == evidence.camera_entries &&
        evidence.camera_original_returns == evidence.camera_original_calls)
      report.flags |= kEvidenceComplete;

    if (action != Action::kStatus) {
      if (!StopProcess(pid)) {
        (void)ResumeProcess(pid);
        close(mem);
        return 7;
      }
      report.flags |= kProcessStopped;
      std::vector<Mapping> stopped_maps;
      NodeSnapshot stopped_node{};
      bool restored = ReadMappings(pid, &stopped_maps) &&
          ReadNode(mem, stopped_maps, game_base, manager, shape,
                   &stopped_node) && stopped_node.node == node.node &&
          stopped_node.vptr == node.vptr;
      if (restored && stopped_node.function == payload.camera_wrapper) {
        ++report.game_write_attempts;
        ++report.rollback_attempts;
        restored = WriteExactVerified(
            mem, node.node + callback::kCallbackOffset,
            &report.original_callback, sizeof(report.original_callback));
      } else if (restored) {
        restored = stopped_node.function == report.original_callback;
      }
      if (restored) report.flags |= kOriginalSlotFinal;
      if (!ReadExact(mem, payload.phase_evidence, &report.evidence,
                     sizeof(report.evidence))) {
        ++report.read_errors;
      } else if (action == Action::kFinalize) {
        const std::uint64_t available = std::min<std::uint64_t>(
            report.evidence.committed_events, maximum_events);
        copied_events.resize(static_cast<std::size_t>(available));
        if (!copied_events.empty() &&
            !ReadExact(mem, payload.phase_events, copied_events.data(),
                       copied_events.size() * sizeof(phase::Event))) {
          copied_events.clear();
          ++report.read_errors;
        }
        report.copied_events = static_cast<std::uint32_t>(copied_events.size());
        if (report.evidence.committed_events != copied_events.size() ||
            report.evidence.committed_events > report.evidence.next_sequence)
          ++report.semantic_errors;
      }
      const bool resumed = ResumeProcess(pid);
      if (resumed) report.flags |= kProcessResumed;
      if (!restored || !resumed) {
        close(mem);
        return 12;
      }
    }
  }

  const bool written = WritePhaseOutput(argv[8], &report, copied_events);
  close(mem);
  std::printf(
      "VEHICLE_CAMERA_PHASE_TRANSACTION action=%u flags=0x%x "
      "camera_entries=%" PRIu64 " vehicle_before=%" PRIu64
      " vehicle_after=%" PRIu64 " copied_events=%u game_writes=%" PRIu64
      " output=%s\n",
      report.action, report.flags, report.evidence.camera_entries,
      report.evidence.vehicle_before_events,
      report.evidence.vehicle_after_events, report.copied_events,
      report.game_write_attempts, argv[8]);
  return written ? 0 : 10;
}
