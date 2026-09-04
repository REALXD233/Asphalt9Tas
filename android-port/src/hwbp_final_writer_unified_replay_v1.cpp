// Guarded composition of the proven unified fixed-delta/input executor with
// the AluTasV2-order final-writer callback payload.  The default artifact is
// passive.  The complete path is emitted only as an unlinked review object.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/types.h>

#ifdef A9TAS_RACE_LIFECYCLE_START_V1
#include "race_lifecycle_object_resolver_v1.h"
#endif

#ifndef A9TAS_FINAL_WRITER_LIVE_CANDIDATE
#define A9TAS_FINAL_WRITER_LIVE_CANDIDATE 0
#endif

#if A9TAS_FINAL_WRITER_LIVE_CANDIDATE != 0 && \
    A9TAS_FINAL_WRITER_LIVE_CANDIDATE != 1
#error "A9TAS_FINAL_WRITER_LIVE_CANDIDATE must be 0 or 1"
#endif

#if A9TAS_FINAL_WRITER_LIVE_CANDIDATE == 1

const char* g_a9tas_final_writer_target_blob_path_v1 = nullptr;
const char* g_a9tas_final_writer_payload_report_path_v1 = nullptr;
bool g_a9tas_final_writer_arm_before_resume_v1 = false;
std::uint64_t g_a9tas_final_writer_expected_start_ticks_v1 = 0;
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
std::uintptr_t g_a9tas_race_lifecycle_object_v1 = 0;
std::uintptr_t g_a9tas_race_lifecycle_state_address_v1 = 0;
#endif

namespace {
bool ReadProcessStartTicks(pid_t pid, std::uint64_t* output);
bool CreateFinalWriterArmedMarker(const char* path, pid_t pid,
                                  std::uint64_t start_ticks,
                                  std::uintptr_t delta_address,
                                  std::size_t attached_threads);
bool WaitForFinalWriterMarkerRemoval(const char* path,
                                     std::uint64_t timeout_ms);
}  // namespace

#define A9TAS_UNIFIED_BRAKE_V1 1
#define A9TAS_REPLAY_ALIGNMENT_GUARD_V1 1
#define A9TAS_FINAL_WRITER_REPLAY_V1 1
#define A9TAS_UNIFIED_TICK_EXECUTOR_MAIN_NAME \
  a9tas_final_writer_unified_embedded_main_v1
#include "hwbp_unified_tick_executor_v1.cpp"
#undef A9TAS_UNIFIED_TICK_EXECUTOR_MAIN_NAME

#define A9TAS_STARTLINE_PREARM_PROTOCOL_NO_MAIN 1
#include "startline_prearm_protocol_v1.cpp"
#undef A9TAS_STARTLINE_PREARM_PROTOCOL_NO_MAIN

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_FINAL_WRITER_UNIFIED_REVIEW_ONLY_V1";

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

bool CreateFinalWriterReadyMarker(const char* path, pid_t pid,
                                  std::uint64_t start_ticks,
                                  std::uintptr_t delta_address) {
  const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  char payload[224]{};
  const int length = std::snprintf(
      payload, sizeof(payload),
      "READY_NO_ATTACH_FINAL_WRITER_V1 pid=%d start_ticks=%" PRIu64
      " delta=0x%" PRIxPTR
      " target_threads_attached=0 gameplay_writes=0"
      " payload_mapped=1 host_resume_gate=marker_removal\n",
      static_cast<int>(pid), start_ticks, delta_address);
  bool ok = length > 0 && static_cast<std::size_t>(length) < sizeof(payload);
  std::size_t written = 0;
  while (ok && written < static_cast<std::size_t>(length)) {
    const ssize_t amount =
        write(fd, payload + written,
              static_cast<std::size_t>(length) - written);
    if (amount <= 0)
      ok = false;
    else
      written += static_cast<std::size_t>(amount);
  }
  if (ok && fsync(fd) != 0) ok = false;
  if (close(fd) != 0) ok = false;
  if (!ok) unlink(path);
  return ok;
}

bool CreateFinalWriterArmedMarker(const char* path, pid_t pid,
                                  std::uint64_t start_ticks,
                                  std::uintptr_t delta_address,
                                  std::size_t attached_threads) {
  if (attached_threads == 0) return false;
  const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  // Keep the lifecycle proof explicit without perturbing the established
  // macro-disabled FW-2 binary.
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
  char payload[384]{};
#else
  char payload[288]{};
#endif
  const int length = std::snprintf(
      payload, sizeof(payload),
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
      "READY_ARMED_RACE_LIFECYCLE_FINAL_WRITER_V1 pid=%d start_ticks=%" PRIu64
      " controller_pid=%d delta=0x%" PRIxPTR
      " state_address=0x%" PRIxPTR " state=2"
      " target_threads_attached=%zu vptr_writes=1 gameplay_state_writes=0"
      " payload_mapped=1 all_target_threads_frozen=1"
      " host_resume_gate=marker_removal\n",
      static_cast<int>(pid), start_ticks, static_cast<int>(getpid()),
      delta_address, g_a9tas_race_lifecycle_state_address_v1,
      attached_threads);
#else
      "READY_ARMED_FINAL_WRITER_V1 pid=%d start_ticks=%" PRIu64
      " controller_pid=%d delta=0x%" PRIxPTR
      " target_threads_attached=%zu vptr_writes=1 gameplay_state_writes=0"
      " payload_mapped=1 all_target_threads_frozen=1"
      " host_resume_gate=marker_removal\n",
      static_cast<int>(pid), start_ticks, static_cast<int>(getpid()),
      delta_address, attached_threads);
#endif
  bool ok = length > 0 && static_cast<std::size_t>(length) < sizeof(payload);
  std::size_t written = 0;
  while (ok && written < static_cast<std::size_t>(length)) {
    const ssize_t amount =
        write(fd, payload + written,
              static_cast<std::size_t>(length) - written);
    if (amount <= 0)
      ok = false;
    else
      written += static_cast<std::size_t>(amount);
  }
  if (ok && fsync(fd) != 0) ok = false;
  if (close(fd) != 0) ok = false;
  if (!ok) unlink(path);
  return ok;
}

bool WaitForFinalWriterMarkerRemoval(const char* path,
                                     std::uint64_t timeout_ms) {
  const std::uint64_t deadline = MonotonicNs() + timeout_ms * 1000000ULL;
  while (MonotonicNs() < deadline) {
    if (access(path, F_OK) != 0) return errno == ENOENT;
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return false;
}

bool PrearmFinalWriterUntilResume(int argc, char** argv,
                                   bool* read_only_complete) {
  if (read_only_complete == nullptr) return false;
  *read_only_complete = false;
  const char* read_only_value =
      std::getenv("A9TAS_FINAL_WRITER_READ_ONLY_GATE_V1");
  const bool read_only =
      read_only_value != nullptr && std::strcmp(read_only_value, "1") == 0;
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
  constexpr int kMinimumArgc = 12;
  constexpr int kMaximumArgc = 15;
  constexpr int kOptionalAddressArg = 12;
#else
  constexpr int kMinimumArgc = 10;
  constexpr int kMaximumArgc = 13;
  constexpr int kOptionalAddressArg = 10;
#endif
  if (argc < kMinimumArgc || argc > kMaximumArgc ||
      std::strcmp(argv[9], kAcknowledgement) != 0 ||
      access(argv[7], F_OK) == 0 || access(argv[8], F_OK) == 0 ||
      std::strcmp(argv[7], argv[8]) == 0) {
    std::fprintf(stderr,
                 "final-writer authorization/report gate failed; no process opened\n");
    return false;
  }
  RecordingHeaderV1 recording{};
  std::vector<RecordingFrameV1> frames;
  std::size_t unsupported = 0;
  if (!LoadRecording(argv[5], &recording, &frames) ||
      !RuntimeCapabilitiesSupported(frames, &unsupported)) {
    std::fprintf(stderr,
                 "final-writer recording/capability validation failed; no process opened\n");
    return false;
  }
  std::vector<std::uint8_t> recording_bytes;
  std::vector<std::uint8_t> target_bytes;
  std::uint32_t recording_size = 0, target_size = 0;
  std::uint8_t recording_sha256[32]{}, target_sha256[32]{};
  a9tas::final_writer_target_blob_v1::View target_view{};
  if (!a9tas::final_writer_unified_v1::ReadWholeFile(
          argv[5], &recording_bytes, &recording_size, recording_sha256) ||
      !a9tas::final_writer_unified_v1::ReadWholeFile(
          argv[6], &target_bytes, &target_size, target_sha256) ||
      !a9tas::final_writer_target_blob_v1::Decode(
          target_bytes.data(), target_bytes.size(), recording_sha256,
          recording_size, &target_view) ||
      target_view.header.frame_count != frames.size()) {
    std::fprintf(stderr,
                 "final-writer source/target binding failed; no process opened\n");
    return false;
  }
  (void)target_size;
  (void)target_sha256;

  std::uint64_t pid_value = 0, base_value = 0, timeout_value = 0;
  std::uint64_t expected_start_ticks = 0;
  std::uint64_t context_value = 0, main_value = 0, owner_value = 0;
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
  std::uint64_t lifecycle_object_value = 0, lifecycle_state_value = 0;
#endif
  if (!ParseUnsigned(argv[1], 10, &pid_value) ||
      !ParseUnsigned(argv[2], 16, &base_value) ||
      !ParseUnsigned(argv[3], 10, &timeout_value) ||
      !ParseUnsigned(argv[4], 10, &expected_start_ticks) ||
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
      !ParseUnsigned(argv[10], 16, &lifecycle_object_value) ||
      !ParseUnsigned(argv[11], 16, &lifecycle_state_value) ||
#endif
      (argc > kOptionalAddressArg &&
       !ParseUnsigned(argv[kOptionalAddressArg], 16, &context_value)) ||
      (argc > kOptionalAddressArg + 1 &&
       !ParseUnsigned(argv[kOptionalAddressArg + 1], 16, &main_value)) ||
      (argc > kOptionalAddressArg + 2 &&
       !ParseUnsigned(argv[kOptionalAddressArg + 2], 16, &owner_value)) ||
      pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
      base_value == 0 || timeout_value < 1000 || timeout_value > 300000 ||
      expected_start_ticks == 0) {
    std::fprintf(stderr, "invalid final-writer arguments; no process opened\n");
    return false;
  }
  const pid_t pid = static_cast<pid_t>(pid_value);
  const auto base = static_cast<std::uintptr_t>(base_value);
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
  const auto lifecycle_object =
      static_cast<std::uintptr_t>(lifecycle_object_value);
  const auto lifecycle_state =
      static_cast<std::uintptr_t>(lifecycle_state_value);
  if (lifecycle_object == 0 || lifecycle_state == 0 ||
      lifecycle_object >
          UINTPTR_MAX - a9tas::race_lifecycle_v1::kPhaseStateOffset ||
      lifecycle_state !=
          lifecycle_object + a9tas::race_lifecycle_v1::kPhaseStateOffset ||
      (lifecycle_state & 3u) != 0) {
    std::fprintf(stderr,
                 "final-writer lifecycle object/state binding invalid; no attach\n");
    return false;
  }
#endif
  std::uint64_t observed_start_ticks = 0;
  if (!ReadProcessStartTicks(pid, &observed_start_ticks) ||
      observed_start_ticks != expected_start_ticks) {
    std::fprintf(stderr, "final-writer PID/start-time mismatch; no attach\n");
    return false;
  }
  std::uintptr_t main_object = 0, final_owner = 0;
  if (!VerifyTargetBuild(pid, base) ||
      !ResolveMainObject(pid, base, static_cast<std::uintptr_t>(main_value),
                         &main_object) ||
      !ResolveFinalOwner(pid, base, static_cast<std::uintptr_t>(owner_value),
                         &final_owner)) {
    std::fprintf(stderr,
                 "final-writer scheduler resolution failed; no attach\n");
    return false;
  }
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                static_cast<int>(pid));
  const int mem = open(mem_path, O_RDONLY | O_CLOEXEC);
  if (mem < 0) return false;

#ifdef A9TAS_RACE_LIFECYCLE_START_V1
  std::uintptr_t lifecycle_vptr = 0;
  a9tas::race_lifecycle_v1::Candidate lifecycle_candidate{};
  if (!a9tas::race_lifecycle_v1::VerifyTargetBuild(mem, base) ||
      !a9tas::race_lifecycle_v1::ReadExact(
          mem, lifecycle_object, &lifecycle_vptr, sizeof(lifecycle_vptr)) ||
      !a9tas::race_lifecycle_v1::ValidateCandidate(
          mem, base, lifecycle_object, lifecycle_vptr,
          &lifecycle_candidate) ||
      lifecycle_candidate.state_address != lifecycle_state ||
      lifecycle_candidate.state !=
          a9tas::race_lifecycle_v1::kCountdownState) {
    std::fprintf(stderr,
                 "final-writer authoritative countdown lifecycle invalid;"
                 " no attach\n");
    close(mem);
    return false;
  }
  g_a9tas_race_lifecycle_object_v1 = lifecycle_object;
  g_a9tas_race_lifecycle_state_address_v1 = lifecycle_state;
#endif

  std::uintptr_t context = 0, adapter = 0, world = 0;
  a9tas::vehicle_state_v1::Layout vehicle{};
  a9tas::vehicle_state_v1::BackendLayout backend{};
  a9tas::final_writer_replay_elf_v1::Layout payload_layout{};
  const bool resolved =
      ResolvePhysicsContext(pid, mem, base,
                            static_cast<std::uintptr_t>(context_value),
                            &context, &adapter, &world) &&
      a9tas::vehicle_state_v1::Resolve(pid, mem, base, &vehicle) &&
      a9tas::vehicle_state_v1::ResolveBackendLayout(mem, base, vehicle,
                                                     &backend) &&
      ExecutorIdentityAlive(mem, vehicle, backend, base) &&
      a9tas::final_writer_replay_elf_v1::Resolve(pid, mem, &payload_layout);
  const std::uintptr_t delta = main_object + kAccumulatorOffset;
  const std::uintptr_t c98 = final_owner + kC98Offset;
  const std::uintptr_t c9c = final_owner + kC9COffset;
  const std::uintptr_t completion = context + kContextCompletionTokenOffset;
  const std::uintptr_t callback_flags = context + kContextCallbackFlagsOffset;
  const std::uintptr_t f64 = vehicle.physics_base + kCarPhysicsF64Offset;
  const std::uintptr_t world_accumulator = world + kWorldAccumulatorOffset;
  std::vector<Mapping> maps;
  if (!resolved || (delta & 7u) != 0 || (c98 & 7u) != 0 || c9c != c98 + 4 ||
      !ReadMaps(pid, &maps) || !PipelineWritable(maps, delta, 8) ||
      !PipelineWritable(maps, c98, 8) ||
      !PipelineWritable(maps, completion, 8) ||
      !PipelineWritable(maps, callback_flags, 2) ||
      !PipelineWritable(maps, f64, 4) ||
      !PipelineWritable(maps, world_accumulator, 4) ||
      !PipelineWritable(maps, backend.native_body, kNativeBodyAuditSize) ||
      !PipelineWritable(maps, backend.base + kWrapperAuditOffset,
                        kWrapperAuditSize)) {
    std::fprintf(stderr,
                 "final-writer full read-only validation failed; no attach\n");
    close(mem);
    return false;
  }

  a9tas::startline_prearm_v1::ProtocolV1 protocol{};
  std::int64_t delta_us = 0;
  if (!ReadExact(mem, delta, &delta_us, sizeof(delta_us)) ||
      !a9tas::startline_prearm_v1::Initialize(&protocol, delta_us).accepted) {
    std::fprintf(stderr,
                 "final-writer paused-zero baseline missing; no attach\n");
    close(mem);
    return false;
  }
  for (int sample = 0; sample < 7; ++sample) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!ReadExact(mem, delta, &delta_us, sizeof(delta_us))) {
      std::fprintf(stderr,
                   "final-writer paused baseline read failed sample=%d;"
                   " no attach\n",
                   sample + 1);
      close(mem);
      return false;
    }
    const auto observation =
        a9tas::startline_prearm_v1::ObserveBeforeAttach(&protocol, delta_us);
    if (!observation.accepted ||
        protocol.phase !=
            a9tas::startline_prearm_v1::PhaseV1::kReadyNoAttach) {
      std::fprintf(stderr,
                   "final-writer paused baseline changed sample=%d"
                   " delta_us=%" PRId64 " phase=%u; no attach\n",
                   sample + 1, delta_us,
                   static_cast<unsigned>(protocol.phase));
      close(mem);
      return false;
    }
  }
  if (!read_only) {
    g_a9tas_final_writer_arm_before_resume_v1 = true;
    g_a9tas_final_writer_expected_start_ticks_v1 = expected_start_ticks;
    close(mem);
    std::printf(
        "FINAL_WRITER_PAUSED_BASELINE pid=%d start_ticks=%" PRIu64
        " frames=%zu paused_zero_samples=%u attach_attempts=0"
        " gameplay_state_writes=0\n",
        static_cast<int>(pid), expected_start_ticks, frames.size(),
        protocol.paused_zero_observations);
    std::fflush(stdout);
    return true;
  }
  char ready_path[112]{};
  std::snprintf(ready_path, sizeof(ready_path),
                "/data/local/tmp/a9tas_final_writer_ready_%d",
                static_cast<int>(pid));
  if (access(ready_path, F_OK) == 0 ||
      !CreateFinalWriterReadyMarker(ready_path, pid, expected_start_ticks,
                                    delta)) {
    std::fprintf(stderr, "stale/unwritable final-writer ready marker\n");
    close(mem);
    return false;
  }
  std::printf(
      "FINAL_WRITER_READY_NO_ATTACH pid=%d start_ticks=%" PRIu64
      " ready=%s frames=%zu paused_zero_samples=%u"
      " target_threads_attached=0 gameplay_writes=0 payload_mapped=1\n",
      static_cast<int>(pid), expected_start_ticks, ready_path, frames.size(),
      protocol.paused_zero_observations);
  std::fflush(stdout);
  const std::uint64_t deadline = MonotonicNs() + timeout_value * 1000000ULL;
  bool host_acknowledged = false;
  while (MonotonicNs() < deadline) {
    if (access(ready_path, F_OK) != 0) {
      host_acknowledged = errno == ENOENT;
      break;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  if (!host_acknowledged) {
    unlink(ready_path);
    close(mem);
    std::fprintf(stderr,
                 "final-writer host resume acknowledgement missing; no attach\n");
    return false;
  }
  close(mem);
  observed_start_ticks = 0;
  if (!ReadProcessStartTicks(pid, &observed_start_ticks) ||
      observed_start_ticks != expected_start_ticks) {
    std::fprintf(stderr,
                 "final-writer read-only PID lifetime mismatch; no attach\n");
    return false;
  }
  *read_only_complete = true;
  std::printf(
      "FINAL_WRITER_READ_ONLY_COMPLETE pid=%d start_ticks=%" PRIu64
      " attached=0 gameplay_writes=0 reports=0\n",
      static_cast<int>(pid), expected_start_ticks);
  std::fflush(stdout);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
  constexpr int kMainMinimumArgc = 12;
  constexpr int kMainMaximumArgc = 15;
#else
  constexpr int kMainMinimumArgc = 10;
  constexpr int kMainMaximumArgc = 13;
#endif
  if (argc < kMainMinimumArgc || argc > kMainMaximumArgc) {
    std::fprintf(
        stderr,
        "usage: %s PID LIB_BASE_HEX TIMEOUT_MS PROC_START_TICKS "
        "A9UTK1_PATH A9FWT1_PATH "
        "REPORT_PATH PAYLOAD_REPORT_PATH ACKNOWLEDGEMENT "
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
        "LIFECYCLE_OBJECT_HEX LIFECYCLE_STATE_HEX "
#endif
        "[PHYSICS_CONTEXT_HEX] [MAIN_OBJECT_HEX] [FINAL_OWNER_HEX]\n",
        argv[0]);
    return 2;
  }
  g_a9tas_final_writer_target_blob_path_v1 = argv[6];
  g_a9tas_final_writer_payload_report_path_v1 = argv[8];
  bool read_only_complete = false;
  if (!PrearmFinalWriterUntilResume(argc, argv, &read_only_complete)) return 5;
  if (read_only_complete) return 0;
  char* shifted[11]{};
  shifted[0] = argv[0];
  shifted[1] = argv[1];
  shifted[2] = argv[2];
  shifted[3] = argv[3];
  shifted[4] = argv[5];
  shifted[5] = argv[7];
  shifted[6] = const_cast<char*>(kUnifiedAcknowledgement);
#ifdef A9TAS_RACE_LIFECYCLE_START_V1
  for (int index = 12; index < argc; ++index)
    shifted[index - 5] = argv[index];
  return a9tas_final_writer_unified_embedded_main_v1(argc - 5, shifted);
#else
  for (int index = 10; index < argc; ++index)
    shifted[index - 3] = argv[index];
  return a9tas_final_writer_unified_embedded_main_v1(argc - 3, shifted);
#endif
}

#else

int main() {
  std::puts(
      "FINAL_WRITER_UNIFIED_BUILD_ONLY runtime=disabled return=-100 "
      "device_access=0 attach=0 game_writes=0");
  return 100;
}

#endif
