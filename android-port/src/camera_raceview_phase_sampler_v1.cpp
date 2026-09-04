// Timestamped read-only RaceView phase sampler. Reuse the already reviewed
// process identity and exact-layout helpers without changing the successful
// consistency sampler.

#define main a9tas_embedded_raceview_state_sampler_v1_main
#include "camera_raceview_state_sampler_v1.cpp"
#undef main

namespace {

constexpr char kPhaseAcknowledgement[] =
    "I_ACCEPT_RACEVIEW_PHASE_READ_ONLY_V1";

std::uint64_t MonotonicMicroseconds() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<std::uint64_t>(now.tv_sec) * 1000000u +
         static_cast<std::uint64_t>(now.tv_nsec) / 1000u;
}

void PrintPhase(std::uint64_t sample, std::uint64_t elapsed_us,
                std::uint32_t mask, const raceview::Snapshot& state) {
  const auto& manager = state.world_state;
  const auto& final_shape = state.shape_state.state_a;
  std::printf(
      "RACEVIEW_PHASE sample=%" PRIu64 " t_us=%" PRIu64 " mask=%u"
      " mp=%.9g,%.9g,%.9g mq=%.9g,%.9g,%.9g,%.9g"
      " sp=%.9g,%.9g,%.9g sq=%.9g,%.9g,%.9g,%.9g fov=%.9g\n",
      sample, elapsed_us, mask,
      static_cast<double>(manager.position[0]),
      static_cast<double>(manager.position[1]),
      static_cast<double>(manager.position[2]),
      static_cast<double>(manager.quaternion_xyzw[0]),
      static_cast<double>(manager.quaternion_xyzw[1]),
      static_cast<double>(manager.quaternion_xyzw[2]),
      static_cast<double>(manager.quaternion_xyzw[3]),
      static_cast<double>(final_shape.position[0]),
      static_cast<double>(final_shape.position[1]),
      static_cast<double>(final_shape.position[2]),
      static_cast<double>(final_shape.quaternion_xyzw[0]),
      static_cast<double>(final_shape.quaternion_xyzw[1]),
      static_cast<double>(final_shape.quaternion_xyzw[2]),
      static_cast<double>(final_shape.quaternion_xyzw[3]),
      static_cast<double>(state.fov_radians));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 9 ||
      std::strcmp(argv[8], kPhaseAcknowledgement) != 0) {
    std::fprintf(stderr,
                 "usage: %s PID LIB_BASE_HEX START_TICKS MANAGER_HEX "
                 "SHAPE_HEX SAMPLES INTERVAL_MS "
                 "I_ACCEPT_RACEVIEW_PHASE_READ_ONLY_V1\n",
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
      samples == 0 || samples > 2400 || interval_ms == 0 ||
      interval_ms > 20 || samples > 12000 / interval_ms) {
    std::fprintf(stderr, "invalid bounded RaceView phase arguments\n");
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

  raceview::Snapshot previous{};
  raceview::Snapshot current{};
  raceview::Result result = raceview::Result::kInvalidArgument;
  std::uint64_t completed = 0;
  const std::uint64_t started_us = MonotonicMicroseconds();
  std::printf(
      "RACEVIEW_PHASE_BEGIN pid=%d start_ticks=%" PRIu64
      " manager=0x%" PRIx64 " shape=0x%" PRIx64
      " samples=%" PRIu64 " interval_ms=%" PRIu64 "\n",
      static_cast<int>(pid), start_ticks, manager_value, shape_value, samples,
      interval_ms);

  for (; completed < samples; ++completed) {
    result = raceview::ReadSnapshot(
        {&memory, &ReadRemote}, mappings.data(), mappings.size(),
        static_cast<std::uintptr_t>(base_value),
        static_cast<std::uintptr_t>(manager_value),
        static_cast<std::uintptr_t>(shape_value), &current);
    if (result != raceview::Result::kOk) break;

    std::uint32_t mask = 0;
    if (completed == 0 ||
        shape::Different(previous.world_state, current.world_state))
      mask |= 1u;
    if (completed == 0 ||
        shape::Different(previous.shape_state.state_a,
                         current.shape_state.state_a))
      mask |= 2u;
    if (completed == 0 || previous.fov_radians != current.fov_radians)
      mask |= 4u;
    PrintPhase(completed, MonotonicMicroseconds() - started_us, mask, current);
    previous = current;
    SleepMilliseconds(interval_ms);
  }

  close(memory.fd);
  std::uint64_t final_ticks = 0;
  const bool identity_stable =
      ReadProcessStartTicks(pid, &final_ticks) && final_ticks == start_ticks;
  std::printf(
      "RACEVIEW_PHASE_END result=%d samples_requested=%" PRIu64
      " samples_completed=%" PRIu64 " elapsed_us=%" PRIu64
      " identity_stable=%d device_access=1 gameplay_writes=0"
      " ptrace_calls=0 invoked_methods=0 input_events=0\n",
      static_cast<int>(result), samples, completed,
      MonotonicMicroseconds() - started_us, identity_stable ? 1 : 0);
  return result == raceview::Result::kOk && completed == samples &&
                 identity_stable
             ? 0
             : 6;
}
