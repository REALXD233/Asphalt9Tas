// Bounded read-only RaceView manager/shape consistency sampler. Addresses must
// come from one freshly passed manager graph. No scan, game write, ptrace call,
// interface invocation or input event is present.

#include "camera_raceview_state_v1.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
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
namespace raceview = a9tas::camera_raceview_state_v1;
namespace shape = a9tas::camera_shape_state_v1;

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_RACEVIEW_STATE_READ_ONLY_V1";
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
    if (std::sscanf(line, "%llx-%llx %4s %llx", &begin, &end, perms,
                    &offset) != 4 ||
        begin >= end)
      continue;
    mappings.push_back({static_cast<std::uintptr_t>(begin),
                        static_cast<std::uintptr_t>(end), perms[0] == 'r',
                        perms[1] == 'w', perms[2] == 'x', perms[3] == 'p'});
    if (std::strstr(line, "libAsphalt9.so") != nullptr &&
        begin == expected_game_base && offset == 0)
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

float MaximumDifference(const shape::Transform28& left,
                        const shape::Transform28& right) {
  float maximum = 0.0f;
  for (std::size_t i = 0; i < 3; ++i)
    maximum = std::max(maximum,
                       std::fabs(left.position[i] - right.position[i]));
  for (std::size_t i = 0; i < 4; ++i)
    maximum = std::max(
        maximum,
        std::fabs(left.quaternion_xyzw[i] - right.quaternion_xyzw[i]));
  return maximum;
}

void PrintSnapshot(const char* label, const raceview::Snapshot& snapshot) {
  std::printf(
      "RACEVIEW_STATE_%s manager=0x%" PRIxPTR " shape=0x%" PRIxPTR
      " fov=%.9g local_pos=%.9g,%.9g,%.9g world_pos=%.9g,%.9g,%.9g"
      " shape_pos=%.9g,%.9g,%.9g\n",
      label, snapshot.manager, snapshot.shape_object,
      static_cast<double>(snapshot.fov_radians),
      static_cast<double>(snapshot.local_state.position[0]),
      static_cast<double>(snapshot.local_state.position[1]),
      static_cast<double>(snapshot.local_state.position[2]),
      static_cast<double>(snapshot.world_state.position[0]),
      static_cast<double>(snapshot.world_state.position[1]),
      static_cast<double>(snapshot.world_state.position[2]),
      static_cast<double>(snapshot.shape_state.state_a.position[0]),
      static_cast<double>(snapshot.shape_state.state_a.position[1]),
      static_cast<double>(snapshot.shape_state.state_a.position[2]));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 9 || std::strcmp(argv[8], kAcknowledgement) != 0) {
    std::fprintf(stderr,
                 "usage: %s PID LIB_BASE_HEX START_TICKS MANAGER_HEX "
                 "SHAPE_HEX SAMPLES INTERVAL_MS "
                 "I_ACCEPT_RACEVIEW_STATE_READ_ONLY_V1\n",
                 argv[0]);
    return 2;
  }
  std::uint64_t pid_value = 0;
  std::uint64_t base_value = 0;
  std::uint64_t start_ticks = 0;
  std::uint64_t manager_value = 0;
  std::uint64_t shape_value = 0;
  std::uint64_t samples = 0;
  std::uint64_t interval_ms = 0;
  if (!ParseUnsigned(argv[1], 10, &pid_value) ||
      !ParseUnsigned(argv[2], 16, &base_value) ||
      !ParseUnsigned(argv[3], 10, &start_ticks) ||
      !ParseUnsigned(argv[4], 16, &manager_value) ||
      !ParseUnsigned(argv[5], 16, &shape_value) ||
      !ParseUnsigned(argv[6], 10, &samples) ||
      !ParseUnsigned(argv[7], 10, &interval_ms) || pid_value == 0 ||
      pid_value > static_cast<std::uint64_t>(INT32_MAX) || base_value == 0 ||
      manager_value == 0 || shape_value == 0 || start_ticks == 0 ||
      samples == 0 || samples > kMaximumSamples || interval_ms == 0 ||
      interval_ms > 1000 || samples > kMaximumDurationMs / interval_ms) {
    std::fprintf(stderr, "invalid bounded RaceView sampler arguments\n");
    return 2;
  }

  const pid_t pid = static_cast<pid_t>(pid_value);
  std::uint64_t observed_ticks = 0;
  if (!ReadProcessStartTicks(pid, &observed_ticks) ||
      observed_ticks != start_ticks)
    return 3;
  std::vector<active::Mapping> mappings;
  if (!ReadMappings(pid, static_cast<std::uintptr_t>(base_value), &mappings))
    return 4;

  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
  ProcessMemory memory{};
  memory.fd = open(path, O_RDONLY | O_CLOEXEC);
  if (memory.fd < 0) return 5;

  raceview::Snapshot first{};
  raceview::Snapshot previous{};
  raceview::Snapshot current{};
  std::uint32_t changed_local = 0;
  std::uint32_t changed_world = 0;
  std::uint32_t changed_shape = 0;
  std::uint32_t changed_fov = 0;
  float max_local_world = 0.0f;
  float max_world_shape_a = 0.0f;
  float max_world_shape_b = 0.0f;
  float max_world_shape_c = 0.0f;
  const std::uint64_t started = MonotonicMilliseconds();
  raceview::Result result = raceview::Result::kInvalidArgument;
  std::uint64_t completed = 0;
  for (; completed < samples; ++completed) {
    result = raceview::ReadSnapshot(
        {&memory, &ReadRemote}, mappings.data(), mappings.size(),
        static_cast<std::uintptr_t>(base_value),
        static_cast<std::uintptr_t>(manager_value),
        static_cast<std::uintptr_t>(shape_value), &current);
    if (result != raceview::Result::kOk) break;
    if (completed == 0) {
      first = current;
    } else {
      changed_local += shape::Different(previous.local_state,
                                        current.local_state);
      changed_world += shape::Different(previous.world_state,
                                        current.world_state);
      changed_shape += shape::Different(previous.shape_state.state_a,
                                        current.shape_state.state_a);
      changed_fov += previous.fov_radians != current.fov_radians;
    }
    max_local_world =
        std::max(max_local_world,
                 MaximumDifference(current.local_state, current.world_state));
    max_world_shape_a =
        std::max(max_world_shape_a,
                 MaximumDifference(current.world_state,
                                   current.shape_state.state_a));
    max_world_shape_b =
        std::max(max_world_shape_b,
                 MaximumDifference(current.world_state,
                                   current.shape_state.state_b));
    max_world_shape_c =
        std::max(max_world_shape_c,
                 MaximumDifference(current.world_state,
                                   current.shape_state.state_c));
    previous = current;
    if (completed + 1 != samples) SleepMilliseconds(interval_ms);
  }
  close(memory.fd);

  std::uint64_t final_ticks = 0;
  const bool identity_stable = ReadProcessStartTicks(pid, &final_ticks) &&
                               final_ticks == start_ticks;
  if (completed != 0) {
    PrintSnapshot("FIRST", first);
    PrintSnapshot("LAST", previous);
  }
  std::printf(
      "RACEVIEW_STATE_END result=%d pid=%d start_ticks=%" PRIu64
      " manager=0x%" PRIxPTR " shape=0x%" PRIxPTR
      " samples_requested=%" PRIu64 " samples_completed=%" PRIu64
      " interval_ms=%" PRIu64 " elapsed_ms=%" PRIu64
      " changed_local=%u changed_world=%u changed_shape=%u changed_fov=%u"
      " max_local_world=%.9g max_world_shape_a=%.9g"
      " max_world_shape_b=%.9g max_world_shape_c=%.9g"
      " identity_stable=%u device_access=1 gameplay_writes=0 ptrace_calls=0"
      " invoked_methods=0 manager_bytes=%zu shape_bytes=%zu\n",
      static_cast<int>(result), static_cast<int>(pid), observed_ticks,
      static_cast<std::uintptr_t>(manager_value),
      static_cast<std::uintptr_t>(shape_value), samples, completed, interval_ms,
      MonotonicMilliseconds() - started, changed_local, changed_world,
      changed_shape, changed_fov, static_cast<double>(max_local_world),
      static_cast<double>(max_world_shape_a),
      static_cast<double>(max_world_shape_b),
      static_cast<double>(max_world_shape_c), identity_stable ? 1u : 0u,
      raceview::kManagerSize, shape::kObjectSize);
  return result == raceview::Result::kOk && completed == samples &&
                 identity_stable
             ? 0
             : 6;
}
