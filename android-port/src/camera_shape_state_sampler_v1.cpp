// Explicitly gated, bounded read-only sampler for the exact 0xF0 camera-manager
// transform object. The object address must come from a freshly passed manager
// graph. No process scan, game write, ptrace call, or interface method call.

#include "camera_shape_state_v1.h"

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <time.h>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

namespace active = a9tas::camera_active_state_resolver_v1;
namespace shape = a9tas::camera_shape_state_v1;

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_CAMERA_SHAPE_STATE_READ_ONLY_V1";
constexpr std::uint64_t kMaximumSamples = 3000;
constexpr std::uint64_t kMaximumDurationMs = 60000;

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

bool ParseUnsigned(const char* text, int base, std::uint64_t* output) {
  if (text == nullptr || output == nullptr || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, base);
  if (errno != 0 || end == text || *end != '\0') return false;
  *output = static_cast<std::uint64_t>(value);
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

bool ReadMappings(pid_t pid, std::uintptr_t expected_game_base,
                  std::vector<active::Mapping>* output) {
  if (pid <= 0 || expected_game_base == 0 || output == nullptr) return false;
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
  FILE* file = std::fopen(path, "re");
  if (file == nullptr) return false;
  std::vector<active::Mapping> mappings;
  bool saw_game_base = false;
  char line[2048]{};
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    unsigned long long begin = 0;
    unsigned long long end = 0;
    unsigned long long offset = 0;
    char perms[5]{};
    const int fields = std::sscanf(line, "%llx-%llx %4s %llx", &begin,
                                   &end, perms, &offset);
    if (fields != 4 || begin >= end) continue;
    active::Mapping mapping{};
    mapping.begin = static_cast<std::uintptr_t>(begin);
    mapping.end = static_cast<std::uintptr_t>(end);
    mapping.readable = perms[0] == 'r';
    mapping.writable = perms[1] == 'w';
    mapping.executable = perms[2] == 'x';
    mapping.private_mapping = perms[3] == 'p';
    mappings.push_back(mapping);
    if (std::strstr(line, "libAsphalt9.so") != nullptr &&
        mapping.begin == expected_game_base && offset == 0)
      saw_game_base = true;
  }
  std::fclose(file);
  if (mappings.empty() || !saw_game_base) return false;
  *output = std::move(mappings);
  return true;
}

std::uint64_t MonotonicMilliseconds() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<std::uint64_t>(now.tv_sec) * 1000u +
         static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
}

void SleepMilliseconds(std::uint64_t milliseconds) {
  timespec request{};
  request.tv_sec = static_cast<time_t>(milliseconds / 1000u);
  request.tv_nsec = static_cast<long>((milliseconds % 1000u) * 1000000u);
  while (nanosleep(&request, &request) != 0 && errno == EINTR) {
  }
}

void PrintTransform(const char* name, const shape::Transform28& state) {
  std::printf(
      " %s_pos=%.9g,%.9g,%.9g %s_q=%.9g,%.9g,%.9g,%.9g",
      name, static_cast<double>(state.position[0]),
      static_cast<double>(state.position[1]),
      static_cast<double>(state.position[2]), name,
      static_cast<double>(state.quaternion_xyzw[0]),
      static_cast<double>(state.quaternion_xyzw[1]),
      static_cast<double>(state.quaternion_xyzw[2]),
      static_cast<double>(state.quaternion_xyzw[3]));
}

void PrintSample(std::uint64_t index, std::uint64_t elapsed_ms,
                 std::uint32_t changed_mask,
                 const shape::Snapshot& snapshot) {
  std::printf(
      "CAMERA_SHAPE_SAMPLE index=%" PRIu64 " elapsed_ms=%" PRIu64
      " changed_mask=0x%x finite_mask=0x%x quaternion_mask=0x%x",
      index, elapsed_ms, changed_mask, snapshot.finite_mask,
      snapshot.quaternion_plausible_mask);
  PrintTransform("a", snapshot.state_a);
  PrintTransform("b", snapshot.state_b);
  std::printf(" motion=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
              static_cast<double>(snapshot.motion_pair.first[0]),
              static_cast<double>(snapshot.motion_pair.first[1]),
              static_cast<double>(snapshot.motion_pair.first[2]),
              static_cast<double>(snapshot.motion_pair.second[0]),
              static_cast<double>(snapshot.motion_pair.second[1]),
              static_cast<double>(snapshot.motion_pair.second[2]));
  PrintTransform("c", snapshot.state_c);
  std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 8 || std::strcmp(argv[7], kAcknowledgement) != 0) {
    std::fprintf(stderr,
                 "usage: %s PID LIB_BASE_HEX START_TICKS OBJECT_HEX "
                 "SAMPLES INTERVAL_MS "
                 "I_ACCEPT_CAMERA_SHAPE_STATE_READ_ONLY_V1\n",
                 argv[0]);
    return 2;
  }
  std::uint64_t pid_value = 0;
  std::uint64_t base_value = 0;
  std::uint64_t expected_start_ticks = 0;
  std::uint64_t object_value = 0;
  std::uint64_t sample_count = 0;
  std::uint64_t interval_ms = 0;
  if (!ParseUnsigned(argv[1], 10, &pid_value) ||
      !ParseUnsigned(argv[2], 16, &base_value) ||
      !ParseUnsigned(argv[3], 10, &expected_start_ticks) ||
      !ParseUnsigned(argv[4], 16, &object_value) ||
      !ParseUnsigned(argv[5], 10, &sample_count) ||
      !ParseUnsigned(argv[6], 10, &interval_ms) || pid_value == 0 ||
      pid_value > static_cast<std::uint64_t>(INT32_MAX) || base_value == 0 ||
      object_value == 0 || expected_start_ticks == 0 || sample_count == 0 ||
      sample_count > kMaximumSamples || interval_ms == 0 ||
      interval_ms > 1000 || sample_count > kMaximumDurationMs / interval_ms) {
    std::fprintf(stderr, "invalid bounded shape-sampler arguments\n");
    return 2;
  }
  const pid_t pid = static_cast<pid_t>(pid_value);
  const auto game_base = static_cast<std::uintptr_t>(base_value);
  const auto object = static_cast<std::uintptr_t>(object_value);
  std::uint64_t observed_start_ticks = 0;
  if (!ReadProcessStartTicks(pid, &observed_start_ticks) ||
      observed_start_ticks != expected_start_ticks) {
    std::fprintf(stderr, "shape-sampler PID/start-time mismatch\n");
    return 3;
  }
  std::vector<active::Mapping> mappings;
  if (!ReadMappings(pid, game_base, &mappings)) return 4;
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
  ProcessMemory memory{};
  memory.fd = open(path, O_RDONLY | O_CLOEXEC);
  if (memory.fd < 0) return 5;

  shape::Snapshot previous{};
  std::uint32_t changed_a = 0;
  std::uint32_t changed_b = 0;
  std::uint32_t changed_motion = 0;
  std::uint32_t changed_c = 0;
  const std::uint64_t started = MonotonicMilliseconds();
  shape::Result result = shape::Result::kInvalidArgument;
  for (std::uint64_t index = 0; index < sample_count; ++index) {
    shape::Snapshot current{};
    result = shape::ReadSnapshot({&memory, &ReadRemote}, mappings.data(),
                                 mappings.size(), game_base, object, &current);
    if (result != shape::Result::kOk) break;
    std::uint32_t changed_mask = 0;
    if (index != 0) {
      if (shape::Different(previous.state_a, current.state_a)) {
        changed_mask |= shape::kStateAValid;
        ++changed_a;
      }
      if (shape::Different(previous.state_b, current.state_b)) {
        changed_mask |= shape::kStateBValid;
        ++changed_b;
      }
      if (shape::Different(previous.motion_pair, current.motion_pair)) {
        changed_mask |= shape::kMotionPairValid;
        ++changed_motion;
      }
      if (shape::Different(previous.state_c, current.state_c)) {
        changed_mask |= shape::kStateCValid;
        ++changed_c;
      }
    }
    const std::uint64_t elapsed = MonotonicMilliseconds() - started;
    if (index == 0 || changed_mask != 0 || index + 1 == sample_count)
      PrintSample(index, elapsed, changed_mask, current);
    previous = current;
    if (index + 1 != sample_count) SleepMilliseconds(interval_ms);
  }
  close(memory.fd);
  std::uint64_t final_start_ticks = 0;
  const bool identity_stable =
      ReadProcessStartTicks(pid, &final_start_ticks) &&
      final_start_ticks == expected_start_ticks;
  std::printf(
      "CAMERA_SHAPE_STATE_END result=%d pid=%d start_ticks=%" PRIu64
      " object=0x%" PRIxPTR " samples_requested=%" PRIu64
      " interval_ms=%" PRIu64 " changed_a=%u changed_b=%u"
      " changed_motion=%u changed_c=%u identity_stable=%u"
      " device_access=1 gameplay_writes=0 ptrace_calls=0 invoked_methods=0"
      " object_bytes=%zu\n",
      static_cast<int>(result), static_cast<int>(pid), observed_start_ticks,
      object, sample_count, interval_ms, changed_a, changed_b, changed_motion,
      changed_c, identity_stable ? 1u : 0u, shape::kObjectSize);
  return result == shape::Result::kOk && identity_stable ? 0 : 6;
}
