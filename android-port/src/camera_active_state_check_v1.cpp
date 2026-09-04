// Explicitly gated, read-only live check for RaceView's blended camera state.

#include "camera_active_state_resolver_v1.h"

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

namespace resolver = a9tas::camera_active_state_resolver_v1;

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_CAMERA_ACTIVE_STATE_READ_ONLY_V1";

struct ProcessMemory {
  int fd{-1};
};

bool ReadRemote(void* context, std::uintptr_t address, void* output,
                std::size_t size) {
  auto* memory = static_cast<ProcessMemory*>(context);
  if (memory == nullptr || memory->fd < 0 || output == nullptr) return false;
  auto* cursor = static_cast<std::uint8_t*>(output);
  std::size_t done = 0;
  while (done < size) {
    const ssize_t amount =
        pread(memory->fd, cursor + done, size - done,
              static_cast<off_t>(address + done));
    if (amount <= 0) return false;
    done += static_cast<std::size_t>(amount);
  }
  return true;
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

bool ParseUnsigned(const char* text, int base, std::uint64_t* output) {
  if (text == nullptr || output == nullptr || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, base);
  if (errno != 0 || end == text || *end != '\0') return false;
  *output = static_cast<std::uint64_t>(value);
  return true;
}

bool ReadMappings(pid_t pid, std::vector<resolver::Mapping>* output) {
  if (pid <= 0 || output == nullptr) return false;
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
  FILE* file = std::fopen(path, "re");
  if (file == nullptr) return false;
  std::vector<resolver::Mapping> mappings;
  char line[2048]{};
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    unsigned long long begin = 0;
    unsigned long long end = 0;
    unsigned long long offset = 0;
    char perms[5]{};
    const int fields = std::sscanf(line, "%llx-%llx %4s %llx", &begin,
                                   &end, perms, &offset);
    if (fields != 4 || begin >= end) continue;
    resolver::Mapping mapping{};
    mapping.begin = static_cast<std::uintptr_t>(begin);
    mapping.end = static_cast<std::uintptr_t>(end);
    mapping.readable = perms[0] == 'r';
    mapping.writable = perms[1] == 'w';
    mapping.executable = perms[2] == 'x';
    mapping.private_mapping = perms[3] == 'p';
    mappings.push_back(mapping);
  }
  std::fclose(file);
  *output = std::move(mappings);
  return !output->empty();
}

void PrintState(const char* label, const resolver::CameraState& state) {
  std::printf(
      " %s_fov=%.9g %s_aspect=%.9g %s_pos=%.9g,%.9g,%.9g "
      "%s_rot=%.9g,%.9g,%.9g,%.9g",
      label, state.fov, label, state.aspect, label, state.position[0],
      state.position[1], state.position[2], label, state.rotation_xyzw[0],
      state.rotation_xyzw[1], state.rotation_xyzw[2],
      state.rotation_xyzw[3]);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5 || std::strcmp(argv[4], kAcknowledgement) != 0) {
    std::fprintf(stderr,
                 "usage: %s PID LIB_BASE_HEX START_TICKS "
                 "I_ACCEPT_CAMERA_ACTIVE_STATE_READ_ONLY_V1\n",
                 argv[0]);
    return 2;
  }
  std::uint64_t pid_value = 0;
  std::uint64_t base_value = 0;
  std::uint64_t expected_start_ticks = 0;
  if (!ParseUnsigned(argv[1], 10, &pid_value) ||
      !ParseUnsigned(argv[2], 16, &base_value) ||
      !ParseUnsigned(argv[3], 10, &expected_start_ticks) ||
      pid_value == 0 || pid_value > static_cast<std::uint64_t>(INT32_MAX) ||
      base_value == 0 || expected_start_ticks == 0) {
    std::fprintf(stderr, "invalid camera-check arguments; no process opened\n");
    return 2;
  }
  const pid_t pid = static_cast<pid_t>(pid_value);
  const auto game_base = static_cast<std::uintptr_t>(base_value);
  std::uint64_t observed_start_ticks = 0;
  if (!ReadProcessStartTicks(pid, &observed_start_ticks) ||
      observed_start_ticks != expected_start_ticks) {
    std::fprintf(stderr, "camera-check PID/start-time mismatch; no mem open\n");
    return 3;
  }
  std::vector<resolver::Mapping> mappings;
  if (!ReadMappings(pid, &mappings)) return 4;
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
  ProcessMemory memory{};
  memory.fd = open(path, O_RDONLY | O_CLOEXEC);
  if (memory.fd < 0) return 5;
  resolver::Resolution resolution{};
  const resolver::Result resolve_result = resolver::Resolve(
      {&memory, &ReadRemote}, mappings.data(), mappings.size(), game_base,
      &resolution);
  resolver::StateSamples samples{};
  resolver::Result sample_result = resolver::Result::kStateUnavailable;
  if (resolve_result == resolver::Result::kOk) {
    sample_result = resolver::ReadStateSamples(
        {&memory, &ReadRemote}, mappings.data(), mappings.size(), resolution,
        &samples);
  }
  close(memory.fd);

  std::printf(
      "CAMERA_ACTIVE_STATE_CHECK_V1 result=%d sample_result=%d pid=%d "
      "start_ticks=%" PRIu64 " mapping_count=%zu scanned=%" PRIu64
      " candidates=%u race_view=0x%" PRIxPTR
      " manager=0x%" PRIxPTR " camera=0x%" PRIxPTR
      " camera_vptr=0x%" PRIxPTR " actual_base=0x%" PRIxPTR
      " valid_mask=0x%x plausible_mask=0x%x"
      " failure_mapping_index=%" PRIu64 " failure_address=0x%" PRIxPTR
      " failure_size=%" PRIu64,
      static_cast<int>(resolve_result), static_cast<int>(sample_result),
      static_cast<int>(pid), observed_start_ticks, mappings.size(),
      resolution.bytes_scanned,
      resolution.structural_candidates, resolution.race_view,
      resolution.camera_manager, resolution.blended_camera,
      resolution.camera_vptr, resolution.actual_base_candidate,
      samples.valid_mask, samples.plausible_mask,
      resolution.failure_mapping_index, resolution.failure_address,
      resolution.failure_size);
  if ((samples.valid_mask & resolver::kControllerRelativeValid) != 0)
    PrintState("controller", samples.controller_relative);
  if ((samples.valid_mask & resolver::kActualBaseCompactValid) != 0)
    PrintState("compact", samples.actual_base_compact);
  if ((samples.valid_mask & resolver::kActualBaseUpstreamValid) != 0)
    PrintState("upstream", samples.actual_base_upstream);
  std::printf(" device_access=1 gameplay_writes=0 ptrace_calls=0\n");
  return resolve_result == resolver::Result::kOk &&
                 sample_result == resolver::Result::kOk
             ? 0
             : 6;
}
