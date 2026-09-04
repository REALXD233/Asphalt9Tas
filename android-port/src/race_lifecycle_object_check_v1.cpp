// Explicitly gated, read-only runtime check for the Android race-phase owner.

#include "race_lifecycle_object_resolver_v1.h"

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_RACE_LIFECYCLE_READ_ONLY_V1";

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

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5 || std::strcmp(argv[4], kAcknowledgement) != 0) {
    std::fprintf(stderr,
                 "usage: %s PID LIB_BASE_HEX START_TICKS "
                 "I_ACCEPT_RACE_LIFECYCLE_READ_ONLY_V1\n",
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
    std::fprintf(stderr, "invalid race-lifecycle arguments; no process opened\n");
    return 2;
  }
  const pid_t pid = static_cast<pid_t>(pid_value);
  const auto base = static_cast<std::uintptr_t>(base_value);
  std::uint64_t observed_start_ticks = 0;
  if (!ReadProcessStartTicks(pid, &observed_start_ticks) ||
      observed_start_ticks != expected_start_ticks) {
    std::fprintf(stderr, "race-lifecycle PID/start-time mismatch; no mem open\n");
    return 3;
  }
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
  const int mem = open(path, O_RDONLY | O_CLOEXEC);
  if (mem < 0) return 4;
  a9tas::race_lifecycle_v1::Resolution resolution{};
  const bool resolved = a9tas::race_lifecycle_v1::ResolveCountdownObject(
      pid, mem, base, &resolution);
  close(mem);
  const auto& selected = resolution.selected;
  std::printf(
      "RACE_LIFECYCLE_OBJECT_CHECK_V1 resolved=%u pid=%d "
      "start_ticks=%" PRIu64 " scanned=%" PRIu64
      " structurally_valid=%u countdown_candidates=%u "
      "object=0x%" PRIxPTR " vptr=0x%" PRIxPTR
      " phase_enter=0x%" PRIxPTR " state_address=0x%" PRIxPTR
      " state=%u device_access=1 gameplay_writes=0 ptrace_calls=0\n",
      resolved ? 1u : 0u, static_cast<int>(pid), observed_start_ticks,
      resolution.scanned_bytes, resolution.structurally_valid,
      resolution.countdown_candidates, selected.object, selected.vptr,
      selected.phase_enter, selected.state_address, selected.state);
  return resolved ? 0 : 5;
}
