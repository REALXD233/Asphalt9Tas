// Review-only, read-only preflight for the M1 controller-shadow executor.
// It proves every process/payload/object identity needed by the later
// stopped-thread transaction, but performs no ptrace attach and no write.

#include <cstdio>

#ifndef A9TAS_M1_PREFLIGHT_REVIEW
#define A9TAS_M1_PREFLIGHT_REVIEW 0
#endif

#if A9TAS_M1_PREFLIGHT_REVIEW != 0 && A9TAS_M1_PREFLIGHT_REVIEW != 1
#error "A9TAS_M1_PREFLIGHT_REVIEW must be 0 or 1"
#endif

#if A9TAS_M1_PREFLIGHT_REVIEW == 1

#define A9TAS_PIPELINE_ORDER_NO_MAIN
#include "hwbp_pipeline_order_observer_v1.cpp"
#define A9TAS_UNIFIED_TICK_CORE_NO_MAIN
#include "unified_tick_executor_core_v1.cpp"

#include "controller_shadow_coordinator_elf_resolver_v1.h"
#include "controller_shadow_transaction_core_v1.h"
#include "gameplay_input_controller_resolver_v1.h"
#include "gameplay_input_controller_mapping_adapter_v1.h"
#include "final_writer_replay_elf_resolver_v1.h"
#include "final_writer_target_blob_protocol_v1.h"
#include "final_writer_transaction_core_v1.h"
#define A9TAS_NAL_ACTION_PAYLOAD_REVIEW 2
#include "natural_action_lifecycle_elf_resolver_v1.h"
#include "natural_action_callback_mailbox_v1.h"
#include "natural_action_scheduler_report_v1.h"
#include "race_lifecycle_object_resolver_v1.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <vector>

namespace {

namespace controller_elf =
    a9tas::controller_shadow_coordinator_elf_v1;
namespace controller_resolver =
    a9tas::gameplay_input_controller_resolver_v1;
namespace transaction = a9tas::controller_shadow_transaction_core_v1;
namespace final_elf = a9tas::final_writer_replay_elf_v1;
namespace final_protocol = a9tas::final_writer_replay_v1;
namespace final_target = a9tas::final_writer_target_blob_v1;
namespace final_transaction = a9tas::final_writer_transaction_core_v1;
namespace natural_elf = a9tas::natural_action_lifecycle_elf_v1;
namespace natural_report = a9tas::natural_action_scheduler_report_v1;
namespace natural_mailbox = a9tas::natural_action_callback_v1;
namespace protocol = a9tas::controller_shadow_coordinator_v1;
namespace lifecycle = a9tas::race_lifecycle_v1;

constexpr char kAcknowledgement[] = "I_ACCEPT_M1_READ_ONLY_PREFLIGHT_V1";
constexpr char kBaseAcknowledgement[] =
    "I_ACCEPT_M1_BASE_READ_ONLY_PREFLIGHT_V1";

bool ReadBackend(void* context, std::uintptr_t address, void* output,
                 std::size_t size) {
  return context != nullptr &&
         ReadExact(*static_cast<const int*>(context), address, output, size);
}

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
  buffer[count] = '\0';
  char* cursor = std::strrchr(buffer, ')');
  if (cursor == nullptr) return false;
  ++cursor;
  for (int field = 3; field <= 22; ++field) {
    while (*cursor == ' ') ++cursor;
    if (*cursor == '\0' || *cursor == '\n') return false;
    char* end = cursor;
    while (*end != '\0' && *end != '\n' && *end != ' ') ++end;
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
  const bool ok = final_elf::detail::HashFile(fd, output);
  close(fd);
  return ok;
}

bool ReadFile(const char* path, std::vector<std::uint8_t>* output,
              std::uint32_t* size) {
  if (path == nullptr || output == nullptr || size == nullptr) return false;
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat info {};
  bool ok = fstat(fd, &info) == 0 && info.st_size > 0 &&
            info.st_size <= 16 * 1024 * 1024 &&
            static_cast<std::uint64_t>(info.st_size) <= UINT32_MAX;
  std::vector<std::uint8_t> bytes;
  if (ok) {
    bytes.resize(static_cast<std::size_t>(info.st_size));
    ok = final_elf::detail::ReadAt(fd, 0, bytes.data(), bytes.size());
  }
  close(fd);
  if (!ok) return false;
  *size = static_cast<std::uint32_t>(bytes.size());
  *output = std::move(bytes);
  return true;
}

bool ConvertMappings(const std::vector<Mapping>& source,
                     std::vector<controller_resolver::Mapping>* output) {
  return a9tas::gameplay_input_controller_mapping_adapter_v1::
      ConvertObjectMappings(source, output);
}

bool NaturalRuntimeReady(int mem, const natural_elf::Layout& payload,
                         std::uintptr_t vehicle_owner,
                         std::uint32_t* session_id,
                         std::uint32_t* producer_tid) {
  using Control = natural_report::PayloadControl;
  using Evidence = natural_report::PayloadEvidence;
  if (session_id == nullptr || producer_tid == nullptr) return false;
  Control control{};
  Evidence evidence{};
  natural_mailbox::Mailbox mailbox{};
  if (!ReadExact(mem, payload.control, &control, sizeof(control)) ||
      !ReadExact(mem, payload.evidence, &evidence, sizeof(evidence)) ||
      !ReadExact(mem, payload.mailbox, &mailbox, sizeof(mailbox)))
    return false;
  const char control_magic[8] = {'A', '9', 'N', 'A', 'L', '1', 0, 0};
  const char evidence_magic[8] = {'A', '9', 'N', 'A', 'X', '1', 0, 0};
  const std::uint64_t session =
      natural_mailbox::LoadAcquire(&mailbox.session_control);
  const std::uint64_t claimed =
      natural_mailbox::LoadAcquire(&mailbox.claimed_sequence);
  const std::uint64_t completed =
      natural_mailbox::LoadAcquire(&mailbox.completed_sequence);
  if (std::memcmp(control.magic, control_magic, 8) != 0 ||
      control.version != 1 || control.size != sizeof(control) ||
      control.expected_car != vehicle_owner ||
      control.vehicle_owner != vehicle_owner ||
      control.dedicated_object != payload.dedicated_object ||
      control.dedicated_vptr != payload.dedicated_vptr ||
      control.session_id == 0 || control.expected_producer_tid == 0 ||
      control.remove_requested != 0 || control.reserved0 != 0 ||
      control.reserved[0] == 0 ||
      std::memcmp(evidence.magic, evidence_magic, 8) != 0 ||
      evidence.version != 1 || evidence.size != sizeof(evidence) ||
      evidence.bootstrap_entries != 1 || evidence.original_calls != 1 ||
      evidence.original_returns != 1 || evidence.registration_attempts != 1 ||
      evidence.registration_returns != 1 || evidence.failures != 0 ||
      evidence.protocol_state != 1 || evidence.last_status != 1 ||
      evidence.removal_attempts != 0 || evidence.removal_returns != 0 ||
      !natural_mailbox::MailboxValid(mailbox) ||
      !natural_mailbox::Armed(session) ||
      natural_mailbox::SessionId(session) != control.session_id ||
      claimed != 0 || completed != 0 ||
      natural_mailbox::LoadAcquire(&mailbox.published_selector) != 0)
    return false;
  *session_id = control.session_id;
  *producer_tid = control.expected_producer_tid;
  return true;
}

bool FinalWriterFactoryReady(int mem, const final_elf::Layout& payload,
                             std::uintptr_t game_base,
                             std::uintptr_t vehicle_owner) {
  final_protocol::Control control{};
  final_protocol::Evidence evidence{};
  std::uintptr_t observed_vptr = 0;
  if (!ReadExact(mem, payload.control, &control, sizeof(control)) ||
      !ReadExact(mem, payload.evidence, &evidence, sizeof(evidence)) ||
      !ReadExact(mem, vehicle_owner, &observed_vptr,
                 sizeof(observed_vptr)))
    return false;
  return final_transaction::IsFresh(control, evidence) &&
         observed_vptr ==
             game_base + final_protocol::kPrimaryAddressPointRva;
}

bool ControllerPayloadFactoryReady(int mem,
                                   const controller_elf::Layout& payload) {
  protocol::Control control{};
  protocol::Evidence evidence{};
  std::array<std::uint8_t, protocol::kControllerShadowSize> shadow{};
  if (!ReadExact(mem, payload.control, &control, sizeof(control)) ||
      !ReadExact(mem, payload.evidence, &evidence, sizeof(evidence)) ||
      !ReadExact(mem, payload.shadow, shadow.data(), shadow.size()) ||
      !transaction::FactoryControl(control) ||
      !transaction::FactoryEvidence(evidence) || shadow[0] != 0xA9)
    return false;
  for (std::size_t index = 1; index < shadow.size(); ++index)
    if (shadow[index] != 0) return false;
  return true;
}

int RunPreflight(int argc, char** argv) {
  if (argc != 7 ||
      (std::strcmp(argv[6], kAcknowledgement) != 0 &&
       std::strcmp(argv[6], kBaseAcknowledgement) != 0)) {
    std::fprintf(stderr,
                 "usage: %s PID LIB_BASE_HEX START_TICKS A9UTK1_PATH "
                 "TARGET_BLOB_PATH ACK\n"
                 "ACK is I_ACCEPT_M1_BASE_READ_ONLY_PREFLIGHT_V1 or "
                 "I_ACCEPT_M1_READ_ONLY_PREFLIGHT_V1\n",
                 argv[0]);
    return 2;
  }
  const bool base_only =
      std::strcmp(argv[6], kBaseAcknowledgement) == 0;
  std::uint64_t pid_value = 0;
  std::uint64_t base_value = 0;
  std::uint64_t expected_ticks = 0;
  if (!ParseUnsigned(argv[1], 10, &pid_value) ||
      !ParseUnsigned(argv[2], 16, &base_value) ||
      !ParseUnsigned(argv[3], 10, &expected_ticks) || pid_value == 0 ||
      pid_value > INT32_MAX || base_value == 0 || expected_ticks == 0)
    return 2;
  const pid_t pid = static_cast<pid_t>(pid_value);
  const std::uintptr_t base = static_cast<std::uintptr_t>(base_value);
  std::uint64_t observed_ticks = 0;
  if (!ReadProcessStartTicks(pid, &observed_ticks) ||
      observed_ticks != expected_ticks || !VerifyTargetBuild(pid, base)) {
    std::fprintf(stderr, "M1_PREFLIGHT_FAIL stage=process_identity\n");
    return 3;
  }

  RecordingHeaderV1 recording_header{};
  std::vector<RecordingFrameV1> frames;
  std::uint8_t recording_sha256[32]{};
  std::vector<std::uint8_t> recording_bytes;
  std::vector<std::uint8_t> target_bytes;
  std::uint32_t recording_size = 0;
  std::uint32_t target_size = 0;
  final_target::View target_view{};
  if (!LoadRecording(argv[4], &recording_header, &frames) || frames.empty() ||
      frames.size() > protocol::kMaximumFrames ||
      !RecordingSha256(argv[4], recording_sha256) ||
      !ReadFile(argv[4], &recording_bytes, &recording_size) ||
      !ReadFile(argv[5], &target_bytes, &target_size) || target_size == 0 ||
      !final_target::Decode(target_bytes.data(), target_bytes.size(),
                            recording_sha256, recording_size, &target_view) ||
      target_view.header.frame_count != frames.size()) {
    std::fprintf(stderr, "M1_PREFLIGHT_FAIL stage=recording\n");
    return 3;
  }
  for (std::uint32_t index = 0;
       index < static_cast<std::uint32_t>(frames.size()); ++index)
    if (!protocol::FrameInputSupported(frames[index], index)) {
      std::fprintf(stderr,
                   "M1_PREFLIGHT_FAIL stage=recording_capability frame=%u\n",
                   index);
      return 3;
    }

  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                static_cast<int>(pid));
  int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
  if (mem < 0) return 4;
  std::vector<Mapping> mappings;
  std::vector<controller_resolver::Mapping> controller_mappings;
  controller_resolver::Resolution controller{};
  controller_elf::Layout controller_payload{};
  final_elf::Layout final_payload{};
  natural_elf::Layout natural_payload{};
  lifecycle::Resolution race_lifecycle{};
  const char* natural_failure = nullptr;
  a9tas::vehicle_state_v1::Layout vehicle{};
  a9tas::vehicle_state_v1::BackendLayout backend{};
  std::uint32_t session_id = 0;
  std::uint32_t producer_tid = 0;
  auto FailStage = [&](const char* stage, const char* detail = nullptr) {
    close(mem);
    std::fprintf(stderr, "M1_PREFLIGHT_FAIL stage=%s detail=%s\n", stage,
                 detail == nullptr ? "none" : detail);
    return 3;
  };
  if (!ReadMaps(pid, &mappings)) return FailStage("process_maps");
  if (!ConvertMappings(mappings, &controller_mappings))
    return FailStage("controller_mappings");
  const controller_resolver::Result controller_result =
      controller_resolver::Resolve(
          {&mem, &ReadBackend}, controller_mappings.data(),
          controller_mappings.size(), base, &controller);
  if (controller_result != controller_resolver::Result::kOk) {
    const char* failure_path = "none";
    if (controller.failure_address != 0) {
      for (const auto& mapping : mappings) {
        if (controller.failure_address >= mapping.begin &&
            controller.failure_address < mapping.end) {
          failure_path = mapping.path.empty() ? "anonymous" :
                                                mapping.path.c_str();
          break;
        }
      }
    }
    char detail[512]{};
    std::snprintf(
        detail, sizeof(detail),
        "result_%d map=%" PRIu64 " address=0x%" PRIxPTR
        " size=%" PRIu64 " scanned=%" PRIu64 " path=%s",
        static_cast<int>(controller_result),
        controller.failure_mapping_index, controller.failure_address,
        controller.failure_size, controller.bytes_scanned, failure_path);
    return FailStage("gameplay_input_controller", detail);
  }
  if (!controller_elf::Resolve(pid, mem, &controller_payload))
    return FailStage("controller_payload");
  if (!final_elf::Resolve(pid, mem, &final_payload))
    return FailStage("final_writer_payload");
  if (!natural_elf::Resolve(pid, mem, &natural_payload, &natural_failure))
    return FailStage("natural_payload", natural_failure);
  if (!lifecycle::ResolveCountdownObject(pid, mem, base, &race_lifecycle))
    return FailStage("race_lifecycle");
  if (!a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle))
    return FailStage("vehicle_state");
  if (!a9tas::vehicle_state_v1::ResolveBackendLayout(mem, base, vehicle,
                                                      &backend))
    return FailStage("vehicle_backend");
  if (!FinalWriterFactoryReady(mem, final_payload, base,
                               vehicle.physics_base))
    return FailStage("final_writer_factory");
  if (!ControllerPayloadFactoryReady(mem, controller_payload))
    return FailStage("controller_factory");
  if (base_only) {
    close(mem);
    std::printf(
        "M1_BASE_PREFLIGHT_OK pid=%d start_ticks=%" PRIu64
        " frames=%zu fixed_interval_us=%u controller=0x%" PRIxPTR
        " source=0x%" PRIxPTR " vehicle_owner=0x%" PRIxPTR
        " controller_payload=0x%" PRIxPTR
        " final_payload=0x%" PRIxPTR " natural_payload=0x%" PRIxPTR
        " lifecycle=0x%" PRIxPTR " lifecycle_state=%u"
        " writer_state=factory controller_state=factory"
        " natural_state=resolved target_blob_bound=1"
        " attach_attempts=0 process_writes=0 device_mutations=0\n",
        static_cast<int>(pid), expected_ticks, frames.size(),
        recording_header.fixed_interval_us, controller.controller,
        controller.source, vehicle.physics_base, controller_payload.load_bias,
        final_payload.load_bias, natural_payload.load_bias,
        race_lifecycle.selected.object, race_lifecycle.selected.state);
    return 0;
  }
  if (!NaturalRuntimeReady(mem, natural_payload, vehicle.physics_base,
                           &session_id, &producer_tid))
    return FailStage("natural_runtime_registered");
  close(mem);
  std::printf(
      "M1_PREFLIGHT_OK pid=%d start_ticks=%" PRIu64
      " frames=%zu fixed_interval_us=%u controller=0x%" PRIxPTR
      " source=0x%" PRIxPTR " producer_tid=%u session=%u"
      " controller_payload=0x%" PRIxPTR " final_payload=0x%" PRIxPTR
      " natural_payload=0x%" PRIxPTR
      " lifecycle=0x%" PRIxPTR " lifecycle_state=%u"
      " writer_state=factory controller_state=factory"
      " target_blob_bound=1 attach_attempts=0 process_writes=0"
      " device_mutations=0\n",
      static_cast<int>(pid), expected_ticks, frames.size(),
      recording_header.fixed_interval_us, controller.controller,
      controller.source, producer_tid, session_id, controller_payload.load_bias,
      final_payload.load_bias, natural_payload.load_bias,
      race_lifecycle.selected.object, race_lifecycle.selected.state);
  return 0;
}

}  // namespace

int main(int argc, char** argv) { return RunPreflight(argc, argv); }

#else

int main() {
  std::puts("M1_CONTROLLER_PREFLIGHT_BUILD_ONLY runtime=disabled return=-100 "
            "device_access=0 attach=0 process_writes=0");
  return 100;
}

#endif
