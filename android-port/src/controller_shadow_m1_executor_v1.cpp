// M1 controller-shadow executor candidate.
//
// Runtime ownership is deliberately narrow:
//   * prearm: freeze a stable process thread set, configure/install the
//     hash-pinned final writer, then install the controller-shadow session;
//   * running: observe one x86_64 HWBP address (MainTimeSource fixed delta)
//     and write only that same 8-byte value;
//   * completion/failure: freeze, validate, conditionally roll back both
//     vptr transactions, and detach.
//
// Frame selection, steering, brake, natural actions, writer permits and
// conditional 64+12 physics correction are all owned by game-thread payloads.

#include <cstdio>

#ifndef A9TAS_M1_EXECUTOR_REVIEW
#define A9TAS_M1_EXECUTOR_REVIEW 0
#endif

#ifndef A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1
#define A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 0
#endif

#if A9TAS_M1_EXECUTOR_REVIEW != 0 && A9TAS_M1_EXECUTOR_REVIEW != 1
#error "A9TAS_M1_EXECUTOR_REVIEW must be 0 or 1"
#endif

#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 != 0 && \
    A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 != 1
#error "A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 must be 0 or 1"
#endif

#if A9TAS_M1_EXECUTOR_REVIEW == 1

#define A9TAS_PIPELINE_ORDER_NO_MAIN
#include "hwbp_pipeline_order_observer_v1.cpp"
#define A9TAS_UNIFIED_TICK_CORE_NO_MAIN
#include "unified_tick_executor_core_v1.cpp"

#define A9TAS_NAL_ACTION_PAYLOAD_REVIEW 2
#define A9TAS_FINAL_WRITER_NATURAL_ACTION_V1 1
#include "final_writer_unified_integration_v1.h"

#include "controller_shadow_coordinator_elf_resolver_v1.h"
#include "controller_shadow_host_session_v1.h"
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
#include "controller_shadow_phase_paced_core_v1.h"
#else
#include "gameplay_input_controller_resolver_v1.h"
#include "gameplay_input_controller_mapping_adapter_v1.h"
#include "m1_delta_only_executor_core_v1.h"
#endif
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
#include "gameplay_input_controller_resolver_v1.h"
#include "gameplay_input_controller_mapping_adapter_v1.h"
#endif
#include "natural_action_scheduler_report_v1.h"
#include "race_lifecycle_object_resolver_v1.h"

#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <vector>

const char* g_a9tas_final_writer_target_blob_path_v1 = nullptr;
const char* g_a9tas_final_writer_payload_report_path_v1 = nullptr;

namespace {

namespace controller_elf =
    a9tas::controller_shadow_coordinator_elf_v1;
namespace controller_resolver =
    a9tas::gameplay_input_controller_resolver_v1;
namespace host_session = a9tas::controller_shadow_host_session_v1;
namespace transaction = a9tas::controller_shadow_transaction_core_v1;
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
namespace phase_core = a9tas::controller_shadow_phase_paced_v1;
#else
namespace delta_core = a9tas::m1_delta_only_executor_core_v1;
#endif
namespace final_writer = a9tas::final_writer_unified_v1;
namespace natural_report = a9tas::natural_action_scheduler_report_v1;
namespace protocol = a9tas::controller_shadow_coordinator_v1;
namespace lifecycle = a9tas::race_lifecycle_v1;

#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
constexpr char kAcknowledgement[] =
    "I_ACCEPT_PHASE_PACED_EXECUTOR_REVIEW_V1";
constexpr char kReportMagic[8] = {'A', '9', 'P', 'H', 'E', 'X', '1', 0};
constexpr char kReadyPathPrefix[] = "/data/local/tmp/a9tas_phase_ready_";
#else
constexpr char kAcknowledgement[] = "I_ACCEPT_M1_EXECUTOR_REVIEW_V1";
constexpr char kReportMagic[8] = {'A', '9', 'M', '1', 'E', 'X', '1', 0};
constexpr char kReadyPathPrefix[] = "/data/local/tmp/a9tas_m1_ready_";
#endif
constexpr std::uint64_t kReadyGateTimeoutMs = 30000;

enum ReportFlag : std::uint32_t {
  kIdentityReady = 1u << 0,
  kThreadsFrozen = 1u << 1,
  kFinalWriterInstalled = 1u << 2,
  kControllerInstalled = 1u << 3,
  kCompletionValidated = 1u << 4,
  kControllerClean = 1u << 5,
  kFinalWriterClean = 1u << 6,
  kCleanDetach = 1u << 7,
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
  kWorldCommitCompletion = 1u << 8,
#else
  kPreNextTickCompletion = 1u << 8,
#endif
};

#pragma pack(push, 1)
struct ReportV1 {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t flags;
  std::uint32_t frame_count;
  std::uint64_t pid;
  std::uint64_t process_generation;
  std::uint64_t game_base;
  std::uint64_t main_object;
  std::uint64_t delta_address;
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
  std::uint64_t final_owner;
  std::uint64_t c98_address;
  std::uint64_t c9c_address;
  std::uint64_t world_accumulator;
#endif
  std::uint64_t controller;
  std::uint64_t source;
  std::uint64_t vehicle_owner;
  std::uint64_t event_count;
  std::uint64_t delta_writes;
  std::uint64_t thread_additions;
  std::uint64_t read_errors;
  std::uint64_t ptrace_errors;
  std::uint64_t semantic_errors;
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
  std::uint64_t phase_delta_events;
  std::uint64_t phase_c98_events;
  std::uint64_t phase_c9c_events;
  std::uint64_t phase_world_commit_events;
  std::uint64_t duplicate_delta_events;
  std::uint64_t committed_frames;
#endif
  std::uint32_t initial_threads;
  std::uint32_t final_threads;
  std::int32_t session_install_result;
  std::int32_t session_completion_result;
  std::int32_t session_cleanup_result;
  std::int32_t delta_result;
  std::uint64_t controller_completed_frames;
  std::uint64_t writer_processed_frames;
  std::uint64_t action_completed_sequence;
  std::uint8_t recording_sha256[32];
  std::uint8_t controller_payload_sha256[32];
};
#pragma pack(pop)

#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
static_assert(sizeof(ReportV1) == 328);
#else
static_assert(sizeof(ReportV1) == 248);
#endif

bool ReadProcessStartTicks(pid_t pid, std::uint64_t* output) {
  if (pid <= 0 || output == nullptr) return false;
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char buffer[4096]{};
  const ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);
  if (count <= 0) return false;
  buffer[count] = 0;
  char* cursor = std::strrchr(buffer, ')');
  if (cursor == nullptr) return false;
  ++cursor;
  for (int field = 3; field <= 22; ++field) {
    while (*cursor == ' ') ++cursor;
    if (*cursor == 0 || *cursor == '\n') return false;
    char* end = cursor;
    while (*end != 0 && *end != '\n' && *end != ' ') ++end;
    if (field == 22) {
      char* parsed_end = nullptr;
      errno = 0;
      const unsigned long long value = std::strtoull(cursor, &parsed_end, 10);
      if (errno != 0 || parsed_end != end || value == 0) return false;
      *output = static_cast<std::uint64_t>(value);
      return true;
    }
    cursor = end;
  }
  return false;
}

bool RecordingSha256(const char* path, std::uint8_t output[32]) {
  if (path == nullptr || output == nullptr) return false;
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok =
      a9tas::final_writer_replay_elf_v1::detail::HashFile(fd, output);
  close(fd);
  return ok;
}

bool ConvertMappings(const std::vector<Mapping>& source,
                     std::vector<controller_resolver::Mapping>* output) {
  return a9tas::gameplay_input_controller_mapping_adapter_v1::
      ConvertObjectMappings(source, output);
}

bool ReadBackend(void* context, std::uintptr_t address, void* output,
                 std::size_t size) {
  return context != nullptr && output != nullptr && address != 0 && size != 0 &&
         ReadExact(*static_cast<const int*>(context), address, output, size);
}

bool VehicleBackendIdentityAlive(
    int mem, const a9tas::vehicle_state_v1::Layout& vehicle,
    const a9tas::vehicle_state_v1::BackendLayout& backend,
    std::uintptr_t base) {
  std::uintptr_t interface_pointer = 0;
  std::uintptr_t native_pointer = 0;
  std::uintptr_t native_vtable = 0;
  return ReadExact(mem, vehicle.physics_base + 0x30, &interface_pointer,
                   sizeof(interface_pointer)) &&
         interface_pointer == backend.interface &&
         ReadExact(mem, backend.physics_velocity_interface + 0x90,
                   &native_pointer, sizeof(native_pointer)) &&
         native_pointer == backend.native_body &&
         ReadExact(mem, backend.native_body, &native_vtable,
                   sizeof(native_vtable)) &&
         native_vtable ==
             base + a9tas::vehicle_state_v1::kNativePhysicsBodyVtableRva;
}

bool WriteBackend(void* context, std::uintptr_t address, const void* input,
                  std::size_t size) {
  return final_writer::WriteArbitraryVerified(context, address, input, size);
}

bool ReadNaturalIdentity(int mem,
                         const a9tas::natural_action_lifecycle_elf_v1::Layout&
                             payload,
                         std::uintptr_t vehicle_owner,
                         std::uint32_t* session_id,
                         std::uint32_t* producer_tid) {
  if (session_id == nullptr || producer_tid == nullptr) return false;
  natural_report::PayloadControl control{};
  natural_report::PayloadEvidence evidence{};
  if (!ReadExact(mem, payload.control, &control, sizeof(control)) ||
      !ReadExact(mem, payload.evidence, &evidence, sizeof(evidence)))
    return false;
  const char control_magic[8] = {'A', '9', 'N', 'A', 'L', '1', 0, 0};
  const char evidence_magic[8] = {'A', '9', 'N', 'A', 'X', '1', 0, 0};
  if (std::memcmp(control.magic, control_magic, 8) != 0 ||
      control.version != 1 || control.size != sizeof(control) ||
      control.expected_car != vehicle_owner ||
      control.vehicle_owner != vehicle_owner || control.session_id == 0 ||
      control.expected_producer_tid == 0 || control.remove_requested != 0 ||
      std::memcmp(evidence.magic, evidence_magic, 8) != 0 ||
      evidence.version != 1 || evidence.size != sizeof(evidence) ||
      evidence.failures != 0 || evidence.protocol_state != 1)
    return false;
  *session_id = control.session_id;
  *producer_tid = control.expected_producer_tid;
  return true;
}

bool FreezeStableDeltaThreadSet(pid_t pid, std::uintptr_t delta_address,
                                std::vector<TracedThread>* threads,
                                std::uint64_t* additions,
                                std::uint64_t* ptrace_errors) {
  if (threads == nullptr || additions == nullptr || ptrace_errors == nullptr ||
      delta_address == 0)
    return false;
  for (int pass = 0; pass < 6; ++pass) {
    for (auto& thread : *threads) {
      if (!thread.live || thread.stopped) continue;
      if (!StopThread(thread.tid)) {
        ++*ptrace_errors;
        return false;
      }
      thread.stopped = true;
    }
    std::uint64_t failures = 0;
    const std::size_t added = AttachNewThreadsStopped(
        pid, delta_address, 0, 0, 0, threads, &failures,
        AccumulatorOnlyDr7());
    *additions += added;
    *ptrace_errors += failures;
    if (failures != 0) return false;
    if (added == 0) return !threads->empty();
  }
  return false;
}

#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
bool FreezeStablePhaseThreadSet(pid_t pid, std::uintptr_t delta_address,
                                std::uintptr_t c98_address,
                                std::uintptr_t c9c_address,
                                std::uintptr_t world_accumulator,
                                std::vector<TracedThread>* threads,
                                std::uint64_t* additions,
                                std::uint64_t* ptrace_errors) {
  if (threads == nullptr || additions == nullptr || ptrace_errors == nullptr ||
      delta_address == 0 || c98_address == 0 || c9c_address == 0 ||
      world_accumulator == 0)
    return false;
  for (int pass = 0; pass < 6; ++pass) {
    for (auto& thread : *threads) {
      if (!thread.live || thread.stopped) continue;
      if (!StopThread(thread.tid)) {
        ++*ptrace_errors;
        return false;
      }
      thread.stopped = true;
    }
    std::uint64_t failures = 0;
    const std::size_t added = AttachNewThreadsStopped(
        pid, delta_address, c98_address, c9c_address, world_accumulator,
        threads, &failures, BoundaryDr7(true));
    *additions += added;
    *ptrace_errors += failures;
    if (failures != 0) return false;
    if (added == 0) return !threads->empty();
  }
  return false;
}
#endif

bool ResumeAll(std::vector<TracedThread>* threads,
               std::uint64_t* ptrace_errors) {
  if (threads == nullptr || ptrace_errors == nullptr) return false;
  bool ok = true;
  for (auto& thread : *threads) {
    if (!thread.live || !thread.stopped) continue;
    if (!ContinueThread(thread.tid)) {
      ++*ptrace_errors;
      ok = false;
    } else {
      thread.stopped = false;
    }
  }
  return ok;
}

bool InstallFinalWriterFrozen(int mem, final_writer::Runtime* runtime) {
  if (runtime == nullptr || !runtime->configured || runtime->installed ||
      !runtime->natural_runtime.has_value())
    return false;
  const auto& control = runtime->prepared.unpublished_control;
  std::uintptr_t current = 0;
  if (!ReadExact(mem, control.expected_object, &current, sizeof(current)) ||
      current != control.original_vptr ||
      !WriteBackend(&mem, control.expected_object,
                    &runtime->prepared.shadow_vptr,
                    sizeof(runtime->prepared.shadow_vptr)))
    return false;
  runtime->installed = true;
  return true;
}

bool WriteReport(const char* path, const ReportV1& report) {
  if (path == nullptr || access(path, F_OK) == 0) return false;
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) return false;
  const bool ok = std::fwrite(&report, sizeof(report), 1, file) == 1 &&
                  std::fflush(file) == 0 && std::ferror(file) == 0;
  const bool close_ok = std::fclose(file) == 0;
  if (!ok || !close_ok) {
    std::remove(path);
    return false;
  }
  return true;
}

bool ReadyPathValid(const char* path) {
  if (path == nullptr || std::strncmp(path, kReadyPathPrefix,
                                     sizeof(kReadyPathPrefix) - 1) != 0)
    return false;
  const std::size_t length = std::strlen(path);
  if (length <= sizeof(kReadyPathPrefix) - 1 || length >= 192) return false;
  for (const char* cursor = path + sizeof(kReadyPathPrefix) - 1;
       *cursor != '\0'; ++cursor) {
    const char value = *cursor;
    if (!((value >= 'a' && value <= 'z') ||
          (value >= 'A' && value <= 'Z') ||
          (value >= '0' && value <= '9') || value == '_' || value == '-'))
      return false;
  }
  return true;
}

bool WriteReadyMarker(const char* path, pid_t pid,
                      std::uint64_t process_generation,
                      const lifecycle::Candidate& race_lifecycle,
                      std::uintptr_t delta_address,
                      std::uintptr_t controller,
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
                      std::uintptr_t c98_address,
                      std::uintptr_t c9c_address,
                      std::uintptr_t world_accumulator,
#endif
                      std::uint32_t frame_count) {
  if (!ReadyPathValid(path) || access(path, F_OK) == 0) return false;
  const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  char marker[512]{};
  const int length = std::snprintf(
      marker, sizeof(marker),
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
      "PHASE_PACED_READY_ARMED pid=%d start_ticks=%" PRIu64
      " state=%u lifecycle=0x%" PRIxPTR " delta=0x%" PRIxPTR
      " c98=0x%" PRIxPTR " c9c=0x%" PRIxPTR " world=0x%" PRIxPTR
      " controller=0x%" PRIxPTR
      " frames=%u writer_installed=1 controller_installed=1"
      " all_target_threads_frozen=1 delta_writes=0"
      " host_resume_gate=marker_removal controller_pid=%d\n",
      static_cast<int>(pid), process_generation, race_lifecycle.state,
      race_lifecycle.object, delta_address, c98_address, c9c_address,
      world_accumulator, controller, frame_count, static_cast<int>(getpid()));
#else
      "M1_READY_ARMED pid=%d start_ticks=%" PRIu64
      " state=%u lifecycle=0x%" PRIxPTR " delta=0x%" PRIxPTR
      " controller=0x%" PRIxPTR
      " frames=%u writer_installed=1 controller_installed=1"
      " all_target_threads_frozen=1 delta_writes=0"
      " host_resume_gate=marker_removal controller_pid=%d\n",
      static_cast<int>(pid), process_generation, race_lifecycle.state,
      race_lifecycle.object, delta_address, controller, frame_count,
      static_cast<int>(getpid()));
#endif
  bool ok = length > 0 && static_cast<std::size_t>(length) < sizeof(marker);
  if (ok) {
    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(length)) {
      const ssize_t written =
          write(fd, marker + offset, static_cast<std::size_t>(length) - offset);
      if (written <= 0) {
        ok = false;
        break;
      }
      offset += static_cast<std::size_t>(written);
    }
  }
  if (ok) ok = fsync(fd) == 0;
  const bool close_ok = close(fd) == 0;
  if (!ok || !close_ok) {
    std::remove(path);
    return false;
  }
  return true;
}

bool WaitForReadyRemoval(const char* path, pid_t pid,
                         std::uint64_t expected_ticks) {
  if (!ReadyPathValid(path)) return false;
  const std::uint64_t deadline_ns =
      MonotonicNs() + kReadyGateTimeoutMs * 1000000ULL;
  while (MonotonicNs() < deadline_ns) {
    if (access(path, F_OK) != 0) {
      if (errno != ENOENT) return false;
      std::uint64_t observed_ticks = 0;
      return ReadProcessStartTicks(pid, &observed_ticks) &&
             observed_ticks == expected_ticks;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

int RunExecutor(int argc, char** argv) {
  if (argc != 10 || std::strcmp(argv[9], kAcknowledgement) != 0) {
    std::fprintf(stderr,
                 "usage: %s PID LIB_BASE_HEX START_TICKS TIMEOUT_MS "
                 "A9UTK1_PATH TARGET_BLOB_PATH REPORT_PATH "
                 "READY_MARKER_PATH ACK\n",
                 argv[0]);
    return 2;
  }
  if (access(argv[7], F_OK) == 0 || !ReadyPathValid(argv[8]) ||
      access(argv[8], F_OK) == 0)
    return 2;
  std::uint64_t pid_value = 0;
  std::uint64_t base_value = 0;
  std::uint64_t expected_ticks = 0;
  std::uint64_t timeout_ms = 0;
  if (!ParseUnsigned(argv[1], 10, &pid_value) ||
      !ParseUnsigned(argv[2], 16, &base_value) ||
      !ParseUnsigned(argv[3], 10, &expected_ticks) ||
      !ParseUnsigned(argv[4], 10, &timeout_ms) || pid_value == 0 ||
      pid_value > INT32_MAX || base_value == 0 || expected_ticks == 0 ||
      timeout_ms < 1000 || timeout_ms > 300000)
    return 2;

  const pid_t pid = static_cast<pid_t>(pid_value);
  const std::uintptr_t base = static_cast<std::uintptr_t>(base_value);
  ReportV1 report{};
  std::memcpy(report.magic, kReportMagic, sizeof(kReportMagic));
  report.version = 1;
  report.size = sizeof(report);
  report.pid = pid_value;
  report.process_generation = expected_ticks;
  report.game_base = base;

  RecordingHeaderV1 recording_header{};
  std::vector<RecordingFrameV1> frames;
  if (!LoadRecording(argv[5], &recording_header, &frames) ||
      frames.size() < 2 || frames.size() > protocol::kMaximumFrames ||
      !RecordingSha256(argv[5], report.recording_sha256))
    return 3;
  report.frame_count = static_cast<std::uint32_t>(frames.size());
  for (std::uint32_t index = 0; index < report.frame_count; ++index)
    if (!protocol::FrameInputSupported(frames[index], index)) return 3;

  std::uint64_t observed_ticks = 0;
  if (!ReadProcessStartTicks(pid, &observed_ticks) ||
      observed_ticks != expected_ticks || !VerifyTargetBuild(pid, base))
    return 3;

  std::uintptr_t main_object = 0;
  if (!ResolveMainObject(pid, base, 0, &main_object)) return 3;
  const std::uintptr_t delta_address = main_object + kAccumulatorOffset;
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
  std::uintptr_t final_owner = 0;
  [[maybe_unused]] std::uintptr_t physics_context = 0;
  [[maybe_unused]] std::uintptr_t backend_adapter = 0;
  std::uintptr_t inner_world = 0;
  std::uintptr_t world_accumulator = 0;
  if (!ResolveFinalOwner(pid, base, 0, &final_owner)) return 3;
  const std::uintptr_t c98_address = final_owner + kC98Offset;
  const std::uintptr_t c9c_address = final_owner + kC9COffset;
#endif
  report.main_object = main_object;
  report.delta_address = delta_address;
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
  report.final_owner = final_owner;
  report.c98_address = c98_address;
  report.c9c_address = c9c_address;
#endif

  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                static_cast<int>(pid));
  int mem = open(mem_path, O_RDWR | O_CLOEXEC);
  if (mem < 0) return 4;

  int exit_code = 7;
  std::vector<TracedThread> threads;
  host_session::Session session{};
  final_writer::Runtime writer_runtime{};
  bool writer_configured = false;
  bool writer_installed = false;
  bool controller_session_started = false;
  bool completion_validated = false;
  bool ready_marker_created = false;
  std::uint32_t session_id = 0;
  std::uint32_t producer_tid = 0;
  const transaction::Backend process_backend{&mem, &ReadBackend,
                                              &WriteBackend};
  const transaction::Guard guard{true, true, true, true, true};
  transaction::RuntimeLayout runtime_layout{};
  host_session::Result install = host_session::Result::kInvalidArgument;

  std::vector<Mapping> mappings;
  std::vector<controller_resolver::Mapping> controller_mappings;
  controller_resolver::Resolution controller{};
  controller_elf::Layout controller_payload{};
  lifecycle::Resolution race_lifecycle{};
  a9tas::vehicle_state_v1::Layout vehicle{};
  a9tas::vehicle_state_v1::BackendLayout physics_backend{};
  if (!ReadMaps(pid, &mappings) ||
      !PipelineWritable(mappings, delta_address, sizeof(std::int64_t)) ||
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
      !ResolvePhysicsContext(pid, mem, base, 0, &physics_context,
                             &backend_adapter, &inner_world) ||
      !PipelineWritable(mappings, c98_address, sizeof(std::uint32_t)) ||
      !PipelineWritable(mappings, c9c_address, sizeof(std::uint32_t)) ||
      !PipelineWritable(mappings, inner_world + kWorldAccumulatorOffset,
                        sizeof(std::uint32_t)) ||
#endif
      !ConvertMappings(mappings, &controller_mappings) ||
      controller_resolver::Resolve(
          {&mem, &ReadBackend}, controller_mappings.data(),
          controller_mappings.size(), base, &controller) !=
          controller_resolver::Result::kOk ||
      !controller_elf::Resolve(pid, mem, &controller_payload) ||
      !lifecycle::ResolveCountdownObject(pid, mem, base, &race_lifecycle) ||
      !a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle) ||
      !a9tas::vehicle_state_v1::ResolveBackendLayout(
          mem, base, vehicle, &physics_backend) ||
      !VehicleBackendIdentityAlive(mem, vehicle, physics_backend, base)) {
    ++report.read_errors;
    goto cleanup;
  }
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
  world_accumulator = inner_world + kWorldAccumulatorOffset;
  report.world_accumulator = world_accumulator;
#endif
  report.flags |= kIdentityReady;
  report.controller = controller.controller;
  report.source = controller.source;
  report.vehicle_owner = vehicle.physics_base;
  std::memcpy(report.controller_payload_sha256,
              controller_payload.file_sha256,
              sizeof(report.controller_payload_sha256));

#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
  if (!FreezeStablePhaseThreadSet(
          pid, delta_address, c98_address, c9c_address, world_accumulator,
          &threads, &report.thread_additions, &report.ptrace_errors))
#else
  if (!FreezeStableDeltaThreadSet(pid, delta_address, &threads,
                                  &report.thread_additions,
                                  &report.ptrace_errors))
#endif
    goto cleanup;
  report.initial_threads = static_cast<std::uint32_t>(threads.size());
  report.flags |= kThreadsFrozen;
  if (!ReadProcessStartTicks(pid, &observed_ticks) ||
      observed_ticks != expected_ticks) {
    ++report.read_errors;
    goto cleanup;
  }

  g_a9tas_final_writer_target_blob_path_v1 = argv[6];
  if (!final_writer::Setup(
          pid, mem, base, vehicle.physics_base,
          physics_backend.native_pose_address,
          physics_backend.native_linear_address, argv[5], report.frame_count,
          &writer_runtime)) {
    ++report.semantic_errors;
    goto cleanup;
  }
  writer_configured = true;
  if (!InstallFinalWriterFrozen(mem, &writer_runtime)) {
    ++report.semantic_errors;
    goto cleanup;
  }
  writer_installed = true;
  report.flags |= kFinalWriterInstalled;

  if (!ReadNaturalIdentity(mem, writer_runtime.natural_payload,
                           vehicle.physics_base, &session_id,
                           &producer_tid)) {
    ++report.read_errors;
    goto cleanup;
  }
  {
    TracedThread* producer =
        FindThread(&threads, static_cast<pid_t>(producer_tid));
    if (producer == nullptr || !producer->live || !producer->stopped) {
      ++report.semantic_errors;
      goto cleanup;
    }
  }
  runtime_layout = transaction::RuntimeLayout{
      base,
      controller.controller,
      writer_runtime.payload.control,
      writer_runtime.payload.evidence,
      writer_runtime.natural_payload.mailbox,
      vehicle.physics_base,
      session_id,
      producer_tid,
  };
  install = host_session::Install(
      process_backend, guard, controller_payload, runtime_layout,
      frames.data(), report.frame_count, report.recording_sha256, &session);
  controller_session_started = true;
  report.session_install_result = static_cast<std::int32_t>(install);
  if (install != host_session::Result::kOk) {
    ++report.semantic_errors;
    goto cleanup;
  }
  report.flags |= kControllerInstalled;

  {
    lifecycle::Candidate armed_lifecycle{};
    if (!lifecycle::ValidateCandidate(
            mem, base, race_lifecycle.selected.object,
            race_lifecycle.selected.vptr, &armed_lifecycle) ||
        armed_lifecycle.object != race_lifecycle.selected.object ||
        armed_lifecycle.vptr != race_lifecycle.selected.vptr ||
        armed_lifecycle.state_address !=
            race_lifecycle.selected.state_address ||
        armed_lifecycle.state != 2 ||
       !WriteReadyMarker(argv[8], pid, expected_ticks, armed_lifecycle,
                          delta_address, controller.controller,
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
                          c98_address, c9c_address, world_accumulator,
#endif
                          report.frame_count)) {
      ++report.semantic_errors;
      goto cleanup;
    }
    ready_marker_created = true;
  }
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
  std::printf(
      "PHASE_PACED_EXECUTOR_READY pid=%d frames=%u state=2"
      " all_target_threads_frozen=1 delta_writes=0"
      " host_resume_gate=marker_removal controller_pid=%d\n",
      static_cast<int>(pid), report.frame_count, static_cast<int>(getpid()));
#else
  std::printf(
      "M1_EXECUTOR_READY pid=%d frames=%u state=2"
      " all_target_threads_frozen=1 delta_writes=0"
      " host_resume_gate=marker_removal controller_pid=%d\n",
      static_cast<int>(pid), report.frame_count, static_cast<int>(getpid()));
#endif
  std::fflush(stdout);
  if (!WaitForReadyRemoval(argv[8], pid, expected_ticks)) {
    ++report.semantic_errors;
    goto cleanup;
  }
  ready_marker_created = false;

  if (!ResumeAll(&threads, &report.ptrace_errors)) goto cleanup;
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
  std::printf(
      "PHASE_PACED_EXECUTOR_RUNNING pid=%d frames=%u delta=0x%" PRIxPTR
      " c98=0x%" PRIxPTR " c9c=0x%" PRIxPTR " world=0x%" PRIxPTR
      " controller=0x%" PRIxPTR
      " hwbp_addresses=4 per_frame_control_writes=0\n",
      static_cast<int>(pid), report.frame_count, delta_address, c98_address,
      c9c_address, world_accumulator, controller.controller);
#else
  std::printf(
      "M1_EXECUTOR_RUNNING pid=%d frames=%u delta=0x%" PRIxPTR
      " controller=0x%" PRIxPTR
      " hwbp_addresses=1 per_frame_control_writes=0\n",
      static_cast<int>(pid), report.frame_count, delta_address,
      controller.controller);
#endif
  std::fflush(stdout);

  {
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
    phase_core::Config config{report.frame_count,
                              recording_header.fixed_interval_us};
    phase_core::State state{};
    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + timeout_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 250000000ULL;
    bool stop_loop = false;
    while (!stop_loop && MonotonicNs() < deadline_ns &&
           report.read_errors == 0 && report.ptrace_errors == 0 &&
           report.semantic_errors == 0) {
      const std::uint64_t now_ns = MonotonicNs();
      if (now_ns >= next_rescan_ns) {
        std::uint64_t failures = 0;
        const std::size_t added = AttachNewThreads(
            pid, delta_address, c98_address, c9c_address, world_accumulator,
            &threads, &failures, BoundaryDr7(true));
        report.thread_additions += added;
        report.ptrace_errors += failures;
        next_rescan_ns = now_ns + 250000000ULL;
      }
      int status = 0;
      const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
      if (tid == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }
      if (tid < 0) {
        if (errno == EINTR) continue;
        if (errno != ECHILD) ++report.ptrace_errors;
        continue;
      }
      TracedThread* tracked = FindThread(&threads, tid);
      if (WIFEXITED(status) || WIFSIGNALED(status)) {
        if (tracked != nullptr) {
          tracked->live = false;
          tracked->stopped = false;
        }
        if (WIFSIGNALED(status) ||
            (WIFEXITED(status) && WEXITSTATUS(status) != 0))
          ++report.semantic_errors;
        continue;
      }
      if (!WIFSTOPPED(status) || tracked == nullptr) {
        ++report.semantic_errors;
        continue;
      }
      tracked->stopped = true;
      unsigned long dr6 = 0;
      if (WSTOPSIG(status) != SIGTRAP || !PeekDebug(tid, 6, &dr6) ||
          (dr6 & 15UL) == 0 || ((dr6 & 15UL) & ((dr6 & 15UL) - 1UL)) != 0) {
        ++report.semantic_errors;
        continue;
      }
      ++report.event_count;

      phase_core::Boundary boundary = phase_core::Boundary::kDelta;
      if ((dr6 & 2UL) != 0)
        boundary = phase_core::Boundary::kC98;
      else if ((dr6 & 4UL) != 0)
        boundary = phase_core::Boundary::kC9C;
      else if ((dr6 & 8UL) != 0)
        boundary = phase_core::Boundary::kWorldCommit;

      std::int64_t observed_delta = 0;
      phase_core::Receipts receipts{};
      a9tas::final_writer_replay_v1::Evidence writer_evidence{};
      a9tas::natural_action_callback_v1::Mailbox action_mailbox{};
      lifecycle::Candidate live_lifecycle{};
      const bool delta_ok =
          boundary != phase_core::Boundary::kDelta ||
          ReadExact(mem, delta_address, &observed_delta,
                    sizeof(observed_delta));
      if (!delta_ok ||
          !ReadExact(mem, controller_payload.evidence,
                     &receipts.controller, sizeof(receipts.controller)) ||
          !ReadExact(mem, writer_runtime.payload.evidence, &writer_evidence,
                     sizeof(writer_evidence)) ||
          !ReadExact(mem, writer_runtime.natural_payload.mailbox,
                     &action_mailbox, sizeof(action_mailbox)) ||
          !lifecycle::ValidateCandidate(
              mem, base, race_lifecycle.selected.object,
              race_lifecycle.selected.vptr, &live_lifecycle) ||
          live_lifecycle.object != race_lifecycle.selected.object ||
          live_lifecycle.vptr != race_lifecycle.selected.vptr ||
          live_lifecycle.state_address !=
              race_lifecycle.selected.state_address) {
        ++report.read_errors;
        continue;
      }
      receipts.writer_processed_frames = writer_evidence.processed_frames;
      receipts.action_completed_sequence =
          a9tas::natural_action_callback_v1::LoadAcquire(
              &action_mailbox.completed_sequence);

      phase_core::Decision decision{};
      const phase_core::Result phase_result = phase_core::Observe(
          config, boundary, observed_delta, live_lifecycle.state,
          static_cast<std::int32_t>(tid), receipts, &state, &decision);
      report.phase_delta_events = state.delta_events;
      report.phase_c98_events = state.c98_events;
      report.phase_c9c_events = state.c9c_events;
      report.phase_world_commit_events = state.world_commit_events;
      report.duplicate_delta_events = state.duplicate_delta_events;
      report.committed_frames = state.committed_frames;
      report.delta_result = static_cast<std::int32_t>(phase_result);
      if (phase_result != phase_core::Result::kOk) {
        std::fprintf(
            stderr,
            "PHASE_PACED_REJECT result=%d boundary=%u delta=%" PRId64
            " lifecycle=%u stage=%u requests=%" PRIu64
            " committed=%" PRIu64 " owner=%d tid=%d selected=%" PRIu64
            " completed=%" PRIu64 " writer=%" PRIu64
            " action=%" PRIu64 "\n",
            static_cast<std::int32_t>(phase_result),
            static_cast<std::uint32_t>(boundary), observed_delta,
            live_lifecycle.state, static_cast<std::uint32_t>(state.stage),
            state.fixed_delta_requests, state.committed_frames,
            state.cycle_owner_tid, static_cast<int>(tid),
            receipts.controller.selected_frames,
            receipts.controller.completed_frames,
            receipts.writer_processed_frames,
            receipts.action_completed_sequence);
        ++report.semantic_errors;
        stop_loop = true;
      } else if (decision.action == phase_core::Action::kWriteFixedDelta) {
        if (!WriteBackend(&mem, delta_address, &decision.write_value,
                          sizeof(decision.write_value))) {
          ++report.read_errors;
          stop_loop = true;
        } else {
          ++report.delta_writes;
        }
      } else if (decision.action ==
                 phase_core::Action::kFreezeAndValidateFinalInFlight) {
        if (!FreezeStablePhaseThreadSet(
                pid, delta_address, c98_address, c9c_address,
                world_accumulator, &threads, &report.thread_additions,
                &report.ptrace_errors)) {
          ++report.semantic_errors;
        } else {
          host_session::Completion completion{};
          const host_session::Result complete =
              host_session::ValidateFinalInFlight(
                  process_backend, frames.data(), session, &completion);
          report.session_completion_result =
              static_cast<std::int32_t>(complete);
          report.controller_completed_frames =
              completion.controller.completed_frames;
          report.writer_processed_frames =
              completion.final_writer.processed_frames;
          report.action_completed_sequence =
              a9tas::natural_action_callback_v1::LoadAcquire(
                  &completion.action_mailbox.completed_sequence);
          if (complete == host_session::Result::kOk &&
              state.fixed_delta_requests == report.frame_count &&
              state.committed_frames == report.frame_count) {
            completion_validated = true;
            report.flags |= kCompletionValidated | kWorldCommitCompletion;
            report.controller_completed_frames = report.frame_count;
          } else {
            ++report.semantic_errors;
          }
        }
        stop_loop = true;
      }
      if (!stop_loop) {
        if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
          ++report.ptrace_errors;
        else
          tracked->stopped = false;
      }
    }
    if (!stop_loop) ++report.semantic_errors;
#else
    delta_core::Config config{report.frame_count,
                              recording_header.fixed_interval_us};
    delta_core::State state{};
    const std::uint64_t start_ns = MonotonicNs();
    const std::uint64_t deadline_ns = start_ns + timeout_ms * 1000000ULL;
    std::uint64_t next_rescan_ns = start_ns + 250000000ULL;
    bool stop_loop = false;
    while (!stop_loop && MonotonicNs() < deadline_ns &&
           report.read_errors == 0 && report.ptrace_errors == 0 &&
           report.semantic_errors == 0) {
      const std::uint64_t now_ns = MonotonicNs();
      if (now_ns >= next_rescan_ns) {
        std::uint64_t failures = 0;
        const std::size_t added = AttachNewThreads(
            pid, delta_address, 0, 0, 0, &threads, &failures,
            AccumulatorOnlyDr7());
        report.thread_additions += added;
        report.ptrace_errors += failures;
        next_rescan_ns = now_ns + 250000000ULL;
      }
      int status = 0;
      const pid_t tid = waitpid(-1, &status, __WALL | WNOHANG);
      if (tid == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }
      if (tid < 0) {
        if (errno == EINTR) continue;
        if (errno != ECHILD) ++report.ptrace_errors;
        continue;
      }
      TracedThread* tracked = FindThread(&threads, tid);
      if (WIFEXITED(status) || WIFSIGNALED(status)) {
        if (tracked != nullptr) {
          tracked->live = false;
          tracked->stopped = false;
        }
        if (WIFSIGNALED(status) ||
            (WIFEXITED(status) && WEXITSTATUS(status) != 0))
          ++report.semantic_errors;
        continue;
      }
      if (!WIFSTOPPED(status) || tracked == nullptr) {
        ++report.semantic_errors;
        continue;
      }
      tracked->stopped = true;
      unsigned long dr6 = 0;
      if (WSTOPSIG(status) != SIGTRAP || !PeekDebug(tid, 6, &dr6) ||
          (dr6 & 1UL) == 0 || (dr6 & 14UL) != 0) {
        ++report.semantic_errors;
        continue;
      }
      ++report.event_count;
      std::int64_t observed_delta = 0;
      protocol::Evidence evidence{};
      lifecycle::Candidate live_lifecycle{};
      if (!ReadExact(mem, delta_address, &observed_delta,
                     sizeof(observed_delta)) ||
          !ReadExact(mem, controller_payload.evidence, &evidence,
                     sizeof(evidence)) ||
          !lifecycle::ValidateCandidate(
              mem, base, race_lifecycle.selected.object,
              race_lifecycle.selected.vptr, &live_lifecycle) ||
          live_lifecycle.object != race_lifecycle.selected.object ||
          live_lifecycle.vptr != race_lifecycle.selected.vptr ||
          live_lifecycle.state_address !=
              race_lifecycle.selected.state_address) {
        ++report.read_errors;
        continue;
      }
      delta_core::Decision decision{};
      const delta_core::Result delta_result = delta_core::ObserveDelta(
          config, observed_delta, live_lifecycle.state, evidence, &state,
          &decision);
      report.delta_result = static_cast<std::int32_t>(delta_result);
      if (delta_result != delta_core::Result::kOk) {
        std::fprintf(
            stderr,
            "M1_DELTA_REJECT result=%d observed_delta=%" PRId64
            " lifecycle=%u fixed_requests=%" PRIu64
            " closed_requests=%" PRIu64 " wrapper=%" PRIu64
            " selected=%" PRIu64 " completed=%" PRIu64
            " next=%u coordinator_phase=%u status=%d\n",
            static_cast<std::int32_t>(delta_result), observed_delta,
            live_lifecycle.state, state.fixed_delta_requests,
            state.closed_delta_requests, evidence.wrapper_entries,
            evidence.selected_frames, evidence.completed_frames,
            evidence.next_frame, evidence.coordinator_phase,
            evidence.last_status);
        ++report.semantic_errors;
        stop_loop = true;
      } else if (decision.action == delta_core::Action::kWriteFixedDelta) {
        if (!WriteBackend(&mem, delta_address, &decision.write_value,
                          sizeof(decision.write_value))) {
          ++report.read_errors;
          stop_loop = true;
        } else {
          ++report.delta_writes;
        }
      } else if (decision.action ==
                     delta_core::Action::kFreezeAndValidateCompletion ||
                 decision.action ==
                     delta_core::Action::kFreezeAndValidateFinalInFlight) {
        if (!FreezeStableDeltaThreadSet(pid, delta_address, &threads,
                                        &report.thread_additions,
                                        &report.ptrace_errors)) {
          ++report.semantic_errors;
        } else {
          host_session::Completion completion{};
          const bool final_in_flight =
              decision.action ==
              delta_core::Action::kFreezeAndValidateFinalInFlight;
          const host_session::Result complete =
              final_in_flight
                  ? host_session::ValidateFinalInFlight(
                        process_backend, frames.data(), session, &completion)
                  : host_session::ValidateComplete(
                        process_backend, frames.data(), session, &completion);
          report.session_completion_result =
              static_cast<std::int32_t>(complete);
          report.controller_completed_frames =
              completion.controller.completed_frames;
          report.writer_processed_frames =
              completion.final_writer.processed_frames;
          report.action_completed_sequence =
              a9tas::natural_action_callback_v1::LoadAcquire(
                  &completion.action_mailbox.completed_sequence);
          if (complete == host_session::Result::kOk) {
            completion_validated = true;
            report.flags |= kCompletionValidated;
            // FinalInFlight deliberately closes the final frame without an
            // extra controller tick; report its semantic completion as N.
            if (final_in_flight) {
              report.controller_completed_frames = report.frame_count;
              if (observed_delta > 0 &&
                  state.fixed_delta_requests == report.frame_count)
                report.flags |= kPreNextTickCompletion;
            }
          } else {
            ++report.semantic_errors;
          }
        }
        stop_loop = true;
      }
      if (!stop_loop) {
        if (!PokeDebug(tid, 6, 0) || !ContinueThread(tid))
          ++report.ptrace_errors;
        else
          tracked->stopped = false;
      }
    }
    if (!stop_loop) ++report.semantic_errors;
#endif
  }

cleanup:
  if (ready_marker_created) std::remove(argv[8]);
  if (!threads.empty())
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
    (void)FreezeStablePhaseThreadSet(
        pid, delta_address, c98_address, c9c_address, world_accumulator,
        &threads, &report.thread_additions, &report.ptrace_errors);
#else
    (void)FreezeStableDeltaThreadSet(pid, delta_address, &threads,
                                     &report.thread_additions,
                                     &report.ptrace_errors);
#endif
  if (controller_session_started &&
      session.phase != host_session::Phase::kClean &&
      session.phase != host_session::Phase::kEmpty) {
    const transaction::Result rollback =
        host_session::Cleanup({&mem, &ReadBackend, &WriteBackend},
                              {true, true, true, true, true}, &session);
    report.session_cleanup_result = static_cast<std::int32_t>(rollback);
    if (rollback == transaction::Result::kOk)
      report.flags |= kControllerClean;
    else
      ++report.semantic_errors;
  } else if (controller_session_started &&
             session.phase == host_session::Phase::kClean) {
    report.session_cleanup_result =
        static_cast<std::int32_t>(transaction::Result::kOk);
    report.flags |= kControllerClean;
  }
  if (writer_installed) {
    if (final_writer::ConditionalRollback(mem, &writer_runtime))
      report.flags |= kFinalWriterClean;
    else
      ++report.semantic_errors;
  } else if (writer_configured) {
    if (final_writer::ResetUninstalled(mem, &writer_runtime))
      report.flags |= kFinalWriterClean;
    else
      ++report.semantic_errors;
  }
  bool detach_ok = true;
  for (auto& thread : threads) {
    if (!thread.live) continue;
    if (!ClearAndDetach(thread.tid, thread.stopped)) {
      detach_ok = false;
      ++report.ptrace_errors;
    }
  }
  report.final_threads = static_cast<std::uint32_t>(threads.size());
  if (detach_ok) report.flags |= kCleanDetach;
  close(mem);

  const bool success = completion_validated && report.read_errors == 0 &&
                       report.ptrace_errors == 0 &&
                       report.semantic_errors == 0 &&
                        (report.flags &
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
                          (kWorldCommitCompletion | kControllerClean |
                           kFinalWriterClean | kCleanDetach)) ==
                             (kWorldCommitCompletion | kControllerClean |
                              kFinalWriterClean | kCleanDetach);
#else
                          (kPreNextTickCompletion | kControllerClean |
                          kFinalWriterClean | kCleanDetach)) ==
                             (kPreNextTickCompletion | kControllerClean |
                              kFinalWriterClean | kCleanDetach);
#endif
  if (!WriteReport(argv[7], report)) return 8;
#if A9TAS_CONTROLLER_SHADOW_PHASE_PACED_V1 == 1
  std::printf(
      "PHASE_PACED_EXECUTOR_DONE success=%u frames=%u delta_events=%" PRIu64
      " delta_writes=%" PRIu64 " flags=0x%x read=%" PRIu64
      " ptrace=%" PRIu64 " semantic=%" PRIu64
      " completion_result=%d controller_receipt=%" PRIu64
      " writer_receipt=%" PRIu64 " action_receipt=%" PRIu64 "\n",
      success ? 1u : 0u, report.frame_count, report.event_count,
      report.delta_writes, report.flags, report.read_errors,
      report.ptrace_errors, report.semantic_errors,
      report.session_completion_result, report.controller_completed_frames,
      report.writer_processed_frames, report.action_completed_sequence);
#else
  std::printf(
      "M1_EXECUTOR_DONE success=%u frames=%u delta_events=%" PRIu64
      " delta_writes=%" PRIu64 " flags=0x%x read=%" PRIu64
      " ptrace=%" PRIu64 " semantic=%" PRIu64
      " completion_result=%d controller_receipt=%" PRIu64
      " writer_receipt=%" PRIu64 " action_receipt=%" PRIu64 "\n",
      success ? 1u : 0u, report.frame_count, report.event_count,
      report.delta_writes, report.flags, report.read_errors,
      report.ptrace_errors, report.semantic_errors,
      report.session_completion_result, report.controller_completed_frames,
      report.writer_processed_frames, report.action_completed_sequence);
#endif
  return success ? 0 : exit_code;
}

}  // namespace

int main(int argc, char** argv) { return RunExecutor(argc, argv); }

#else

int main() {
  std::puts("M1_CONTROLLER_EXECUTOR_BUILD_ONLY runtime=disabled return=-100 "
            "device_access=0 deployed=0");
  return 100;
}

#endif
