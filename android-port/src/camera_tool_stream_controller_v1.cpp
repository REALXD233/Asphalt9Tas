// Persistent command stream for the already-installed Camera Tool payload.
// This is a host transaction transport only: it never installs a hook and it
// never writes game-owned memory.  One privileged process remains alive so a
// desktop controller can publish high-rate seqlock commands without spawning
// one `su` process per update.

#define A9TAS_CAMERA_TOOL_TRANSACTION_NO_MAIN
#include "camera_tool_transaction_controller_v1.cpp"
#undef A9TAS_CAMERA_TOOL_TRANSACTION_NO_MAIN

#include "vehicle_state_resolver_v1.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace camera_tool_txn {
namespace {

constexpr char kStreamAcknowledgement[] =
    "I_ACCEPT_CAMERA_TOOL_PERSISTENT_STREAM_V1";

struct StreamSession {
  pid_t pid{};
  std::uint64_t start_ticks{};
  std::uintptr_t game_base{};
  std::uintptr_t manager{};
  std::uintptr_t shape{};
  int mem{-1};
  PayloadLayout payload{};
  NodeSnapshot node{};
  std::uintptr_t original{};
  std::uintptr_t embedded_vptr{};
  std::uintptr_t combined_function{};
  std::uintptr_t source{};
  std::uintptr_t source_vptr{};
  std::uintptr_t position_setter{};
  std::uintptr_t rotation_setter{};
  std::uintptr_t fov_setter{};
  bool vehicle_resolved{};
  a9tas::vehicle_state_v1::Layout vehicle{};
  a9tas::vehicle_state_v1::BackendLayout vehicle_backend{};
};

struct StreamCommand {
  std::uint32_t flags{};
  float position[3]{};
  float rotation[4]{};
  float fov{};
};

bool FiniteStreamCommand(const StreamCommand& command) {
  if ((command.flags & camera::kOverridePosition) != 0)
    for (float value : command.position)
      if (!std::isfinite(value)) return false;
  if ((command.flags & camera::kOverrideRotation) != 0)
    for (float value : command.rotation)
      if (!std::isfinite(value)) return false;
  return (command.flags & camera::kOverrideFov) == 0 ||
      std::isfinite(command.fov);
}

bool ReadConfigured(const StreamSession& session, camera::Control* control,
                    camera::Evidence* evidence) {
  std::uint64_t observed_ticks = 0;
  std::uintptr_t callback_function = 0;
  if (!ReadStartTicks(session.pid, &observed_ticks) ||
      observed_ticks != session.start_ticks ||
      !ReadExact(session.mem,
                 session.node.node + callback::kCallbackOffset,
                 &callback_function, sizeof(callback_function)) ||
      callback_function != session.payload.wrapper ||
      !ReadExact(session.mem, session.payload.control, control,
                 sizeof(*control)) ||
      !ReadExact(session.mem, session.payload.evidence, evidence,
                 sizeof(*evidence)) ||
      !EvidenceHeader(*evidence) ||
      !ConfiguredIdentity(*control, session.manager, session.node,
                          session.shape, session.original,
                          session.embedded_vptr, session.combined_function,
                          session.source, session.source_vptr,
                          session.position_setter, session.rotation_setter,
                          session.fov_setter))
    return false;
  return true;
}

bool PublishInactive(const StreamSession& session) {
  const std::uint32_t inactive = camera::kConfigured;
  return WriteExactVerified(
      session.mem,
      session.payload.control + offsetof(camera::Control, flags),
      &inactive, sizeof(inactive));
}

bool PublishCommand(const StreamSession& session,
                    const StreamCommand& command,
                    std::uint64_t* published_sequence) {
  if (published_sequence == nullptr || command.flags == 0 ||
      (command.flags & ~camera::kAbsoluteOverrideMask) != 0)
    return false;
  camera::Control control{};
  camera::Evidence evidence{};
  if (!ReadConfigured(session, &control, &evidence) ||
      evidence.failures != 0 || !FiniteStreamCommand(command))
    return false;
  const std::uint64_t odd = control.command_sequence + 1;
  const std::uint64_t even = control.command_sequence + 2;
  if (even <= control.command_sequence || (odd & 1u) == 0 ||
      (even & 1u) != 0)
    return false;

  struct TargetBytes {
    float position[3];
    float rotation[4];
    float fov;
  } target{};
  static_assert(sizeof(TargetBytes) == 32);
  std::memcpy(target.position, command.position, sizeof(target.position));
  std::memcpy(target.rotation, command.rotation, sizeof(target.rotation));
  target.fov = command.fov;

  bool ok = WriteExactVerified(
      session.mem,
      session.payload.control + offsetof(camera::Control, command_sequence),
      &odd, sizeof(odd));
  ok = ok && WriteExactVerified(
      session.mem,
      session.payload.control + offsetof(camera::Control, override_flags),
      &command.flags, sizeof(command.flags));
  ok = ok && WriteExactVerified(
      session.mem,
      session.payload.control + offsetof(camera::Control, position),
      &target, sizeof(target));
  ok = ok && WriteExactVerified(
      session.mem,
      session.payload.control + offsetof(camera::Control, command_sequence),
      &even, sizeof(even));
  const std::uint32_t active = camera::kConfigured | camera::kActive;
  ok = ok && WriteExactVerified(
      session.mem,
      session.payload.control + offsetof(camera::Control, flags),
      &active, sizeof(active));
  if (!ok) {
    (void)PublishInactive(session);
    return false;
  }
  *published_sequence = even;
  return true;
}

bool ParseSet(char* line, StreamCommand* output) {
  if (line == nullptr || output == nullptr) return false;
  char* save = nullptr;
  char* token = strtok_r(line, " \t\r\n", &save);
  if (token == nullptr || std::strcmp(token, "SET") != 0) return false;
  token = strtok_r(nullptr, " \t\r\n", &save);
  std::uint64_t flags = 0;
  if (token == nullptr || !ParseUnsigned(token, 0, &flags) || flags == 0 ||
      flags > camera::kAbsoluteOverrideMask)
    return false;
  StreamCommand command{};
  command.flags = static_cast<std::uint32_t>(flags);
  float* values[] = {
      &command.position[0], &command.position[1], &command.position[2],
      &command.rotation[0], &command.rotation[1], &command.rotation[2],
      &command.rotation[3], &command.fov,
  };
  for (float* value : values) {
    token = strtok_r(nullptr, " \t\r\n", &save);
    if (token == nullptr || !ParseFloat(token, value)) return false;
  }
  if (strtok_r(nullptr, " \t\r\n", &save) != nullptr) return false;
  if (!FiniteStreamCommand(command))
    return false;
  *output = command;
  return true;
}

void PrintState(const char* tag, const camera::Control& control,
                const camera::Evidence& evidence) {
  std::printf(
      "CAMERA_TOOL_STREAM_%s active=%u sequence=%" PRIu64
      " entries=%" PRIu64 " failures=%" PRIu64
      " source_calls=%" PRIu64 " source_returns=%" PRIu64
      " source_readbacks=%" PRIu64
      " natural=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
      tag, (control.flags & camera::kActive) != 0 ? 1u : 0u,
      control.command_sequence, evidence.wrapper_entries, evidence.failures,
      evidence.source_override_calls, evidence.source_override_returns,
      evidence.source_readback_passes,
      static_cast<double>(evidence.natural_transform[0]),
      static_cast<double>(evidence.natural_transform[1]),
      static_cast<double>(evidence.natural_transform[2]),
      static_cast<double>(evidence.natural_transform[3]),
      static_cast<double>(evidence.natural_transform[4]),
      static_cast<double>(evidence.natural_transform[5]),
      static_cast<double>(evidence.natural_transform[6]),
      static_cast<double>(evidence.natural_fov));
  std::fflush(stdout);
}

bool ReadVehicleTarget(StreamSession* session, float output[3]) {
  if (session == nullptr || output == nullptr) return false;
  if (!session->vehicle_resolved) {
    if (!a9tas::vehicle_state_v1::Resolve(
            session->pid, session->mem, session->game_base,
            &session->vehicle) ||
        !a9tas::vehicle_state_v1::ResolveBackendLayout(
            session->mem, session->game_base, session->vehicle,
            &session->vehicle_backend))
      return false;
    session->vehicle_resolved = true;
  }

  std::uintptr_t live_interface = 0;
  std::uintptr_t live_native_body = 0;
  std::uintptr_t live_native_vptr = 0;
  float transform[16]{};
  if (!ReadExact(session->mem, session->vehicle.physics_base + 0x30,
                 &live_interface, sizeof(live_interface)) ||
      live_interface != session->vehicle_backend.interface ||
      !ReadExact(session->mem,
                 session->vehicle_backend.physics_velocity_interface + 0x90,
                 &live_native_body, sizeof(live_native_body)) ||
      live_native_body != session->vehicle_backend.native_body ||
      !ReadExact(session->mem, live_native_body, &live_native_vptr,
                 sizeof(live_native_vptr)) ||
      live_native_vptr != session->game_base +
          a9tas::vehicle_state_v1::kNativePhysicsBodyVtableRva ||
      !ReadExact(session->mem, session->vehicle_backend.native_pose_address,
                 transform, sizeof(transform)) ||
      !a9tas::vehicle_state_v1::Finite(transform, 16))
    return false;
  output[0] = transform[12];
  output[1] = transform[13];
  output[2] = transform[14];
  return true;
}

}  // namespace

int CameraToolStreamMain(int argc, char** argv) {
  if (argc != 7 || std::strcmp(argv[6], kStreamAcknowledgement) != 0) {
    std::fprintf(stderr,
        "usage: %s PID START_TICKS GAME_BASE_HEX MANAGER_HEX SHAPE_HEX "
        "I_ACCEPT_CAMERA_TOOL_PERSISTENT_STREAM_V1\n", argv[0]);
    return 2;
  }
  std::uint64_t values[5]{};
  const int bases[5] = {10, 10, 16, 16, 16};
  for (int index = 0; index < 5; ++index)
    if (!ParseUnsigned(argv[index + 1], bases[index], &values[index])) return 2;
  if (values[0] == 0 || values[0] > INT32_MAX || values[1] == 0 ||
      values[2] == 0 || values[3] == 0 || values[4] == 0)
    return 2;

  StreamSession session{};
  session.pid = static_cast<pid_t>(values[0]);
  session.start_ticks = values[1];
  session.game_base = values[2];
  session.manager = values[3];
  session.shape = values[4];
  std::uint64_t observed_ticks = 0;
  if (!ReadStartTicks(session.pid, &observed_ticks) ||
      observed_ticks != session.start_ticks)
    return 3;
  std::vector<Mapping> mappings;
  if (!ReadMappings(session.pid, &mappings)) return 3;
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", session.pid);
  session.mem = open(mem_path, O_RDWR | O_CLOEXEC);
  if (session.mem < 0) return 4;

  bool valid = ResolvePayload(session.mem, mappings, &session.payload) &&
      ReadNode(session.mem, mappings, session.game_base, session.manager,
               session.shape, &session.node);
  session.original = session.game_base + callback::kExpectedCallbackRva;
  session.embedded_vptr = session.game_base + kEmbeddedVptrRva;
  session.combined_function = session.game_base + kCombinedFunctionRva;
  session.source_vptr = session.game_base + kSourceVptrRva;
  session.position_setter = session.game_base + kPositionSetterRva;
  session.rotation_setter = session.game_base + kRotationSetterRva;
  session.fov_setter = session.game_base + kFovSetterRva;
  std::uintptr_t live_embedded_vptr = 0;
  std::uintptr_t live_combined = 0;
  std::uintptr_t live_source_vptr = 0;
  std::uintptr_t live_position_setter = 0;
  std::uintptr_t live_rotation_setter = 0;
  valid = valid && session.node.function == session.payload.wrapper &&
      ReadExact(session.mem, session.manager + kManagerSourceOffset,
                &session.source, sizeof(session.source)) &&
      session.source != 0 &&
      ReadExact(session.mem, session.source, &live_source_vptr,
                sizeof(live_source_vptr)) &&
      live_source_vptr == session.source_vptr &&
      ReadExact(session.mem, session.source_vptr + 0x88,
                &live_position_setter, sizeof(live_position_setter)) &&
      live_position_setter == session.position_setter &&
      ReadExact(session.mem, session.source_vptr + 0x90,
                &live_rotation_setter, sizeof(live_rotation_setter)) &&
      live_rotation_setter == session.rotation_setter &&
      ReadExact(session.mem, session.manager + 0x08,
                &live_embedded_vptr, sizeof(live_embedded_vptr)) &&
      live_embedded_vptr == session.embedded_vptr &&
      ReadExact(session.mem, live_embedded_vptr + 0x40,
                &live_combined, sizeof(live_combined)) &&
      live_combined == session.combined_function;

  camera::Control control{};
  camera::Evidence evidence{};
  valid = valid && ReadConfigured(session, &control, &evidence) &&
      evidence.failures == 0;
  if (!valid) {
    close(session.mem);
    return 5;
  }

  PrintState("READY", control, evidence);
  char line[1024]{};
  std::uint64_t commands = 0;
  std::uint64_t target_reads = 0;
  int result = 0;
  while (std::fgets(line, sizeof(line), stdin) != nullptr) {
    if (std::strcmp(line, "STATUS\n") == 0 ||
        std::strcmp(line, "STATUS\r\n") == 0) {
      if (!ReadConfigured(session, &control, &evidence)) {
        result = 6;
        break;
      }
      PrintState("STATUS", control, evidence);
      continue;
    }
    if (std::strcmp(line, "DISABLE\n") == 0 ||
        std::strcmp(line, "DISABLE\r\n") == 0) {
      if (!PublishInactive(session) ||
          !ReadConfigured(session, &control, &evidence) ||
          (control.flags & camera::kActive) != 0) {
        result = 7;
        break;
      }
      ++commands;
      PrintState("DISABLED", control, evidence);
      continue;
    }
    if (std::strcmp(line, "TARGET\n") == 0 ||
        std::strcmp(line, "TARGET\r\n") == 0) {
      float target[3]{};
      if (!ReadConfigured(session, &control, &evidence) ||
          evidence.failures != 0 || !ReadVehicleTarget(&session, target)) {
        result = 9;
        break;
      }
      ++target_reads;
      std::printf("CAMERA_TOOL_STREAM_TARGET reads=%" PRIu64
                  " game=%.9g,%.9g,%.9g\n", target_reads,
                  static_cast<double>(target[0]),
                  static_cast<double>(target[1]),
                  static_cast<double>(target[2]));
      std::fflush(stdout);
      continue;
    }
    if (std::strcmp(line, "QUIT\n") == 0 ||
        std::strcmp(line, "QUIT\r\n") == 0) {
      if (!PublishInactive(session)) result = 7;
      break;
    }
    StreamCommand command{};
    std::uint64_t sequence = 0;
    if (!ParseSet(line, &command) ||
        !PublishCommand(session, command, &sequence)) {
      result = 8;
      break;
    }
    ++commands;
    std::printf("CAMERA_TOOL_STREAM_SET sequence=%" PRIu64
                " commands=%" PRIu64 " active=1\n", sequence, commands);
    std::fflush(stdout);
  }

  // stdin EOF is also a controller disconnect. Never leave the payload active
  // after the sole authoritative command stream has gone away.
  if (!PublishInactive(session) && result == 0) result = 7;
  close(session.mem);
  std::printf("CAMERA_TOOL_STREAM_END result=%d commands=%" PRIu64
              " target_reads=%" PRIu64
              " payload_writes_only=1 game_writes=0 hook_installs=0\n",
              result, commands, target_reads);
  std::fflush(stdout);
  return result;
}

}  // namespace camera_tool_txn

int main(int argc, char** argv) {
  return camera_tool_txn::CameraToolStreamMain(argc, argv);
}
