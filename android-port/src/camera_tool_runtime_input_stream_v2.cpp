// Persistent payload-only input channel for Camera Tool runtime v2.  A host/UI
// sends updates only when input or a parameter changes; the ARM64 callback
// integrates every CameraUpdate independently.  HUD commands publish to a
// shared visibility query.  The stream installs one aligned ARM64 branch and
// the payload preserves the original LDRB/RET semantics for every other object.

#include "camera_tool_runtime_protocol_v2.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <inttypes.h>
#include <string>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

namespace protocol = a9tas::camera_tool_runtime_v2;

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_CAMERA_TOOL_INPUT_STREAM_V2";

struct DynamicCommand {
  protocol::Mode mode{protocol::Mode::kFreeFlight};
  std::uint32_t input_bits{};
  float yaw_degrees{};
  float pitch_degrees{};
  float move_speed{100.0f};
  float sensitivity{0.2f};
  float orbital_distance{5.0f};
  float orbital_zoom_speed{1.0f};
  float fov_radians{0.959931076f};
  std::uint32_t absolute_override_flags{protocol::kAbsoluteOverrideMask};
  float absolute_position[3]{};
  float absolute_rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
};

struct HudIdentity {
  std::uintptr_t root{};
  std::uintptr_t root_vptr{};
  std::uintptr_t interface{};
  std::uintptr_t interface_vptr{};
  std::uintptr_t hidden_getter{};
};

static_assert(sizeof(HudIdentity) == 5 * sizeof(std::uintptr_t));

constexpr std::uintptr_t kHudInterfaceOffset = 0x238u;
constexpr std::uint32_t kHudLoadMask = 0xFFC003FFu;
constexpr std::uint32_t kHudLoadValue = 0x39400000u;
constexpr std::uint32_t kArm64Ret = 0xD65F03C0u;

bool ParseUnsigned(const char* text, int base, std::uint64_t* output) {
  if (text == nullptr || output == nullptr || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, base);
  if (errno != 0 || end == text || *end != '\0') return false;
  *output = static_cast<std::uint64_t>(value);
  return true;
}

bool ParseFloat(const char* text, float* output) {
  if (text == nullptr || output == nullptr || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const float value = std::strtof(text, &end);
  if (errno != 0 || end == text || *end != '\0' || !std::isfinite(value))
    return false;
  *output = value;
  return true;
}

bool ReadExact(int fd, std::uintptr_t address, void* output,
               std::size_t size) {
  auto* cursor = static_cast<std::uint8_t*>(output);
  std::size_t done = 0;
  while (done < size) {
    const ssize_t n = pread(fd, cursor + done, size - done,
                            static_cast<off_t>(address + done));
    if (n <= 0) return false;
    done += static_cast<std::size_t>(n);
  }
  return true;
}

bool WriteExactVerified(int fd, std::uintptr_t address, const void* input,
                        std::size_t size) {
  const auto* cursor = static_cast<const std::uint8_t*>(input);
  std::size_t done = 0;
  while (done < size) {
    const ssize_t n = pwrite(fd, cursor + done, size - done,
                             static_cast<off_t>(address + done));
    if (n <= 0) return false;
    done += static_cast<std::size_t>(n);
  }
  std::uint8_t readback[128]{};
  if (size > sizeof(readback) || !ReadExact(fd, address, readback, size))
    return false;
  return std::memcmp(readback, input, size) == 0;
}

bool EncodeArm64Branch(std::uintptr_t source, std::uintptr_t target,
                       std::uint32_t* output) {
  if (output == nullptr || (source & 3u) != 0 || (target & 3u) != 0)
    return false;
  std::uint64_t encoded_delta = 0;
  if (target >= source) {
    const std::uint64_t distance = target - source;
    if (distance > 0x07FFFFFCu) return false;
    encoded_delta = distance;
  } else {
    const std::uint64_t distance = source - target;
    if (distance > 0x08000000u) return false;
    encoded_delta = static_cast<std::uint64_t>(
        -static_cast<std::int64_t>(distance));
  }
  *output = 0x14000000u |
      static_cast<std::uint32_t>((encoded_delta >> 2) & 0x03FFFFFFu);
  return true;
}

bool ReadStartTicks(pid_t pid, std::uint64_t* output) {
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
  FILE* file = std::fopen(path, "re");
  if (file == nullptr) return false;
  char line[4096]{};
  const bool read = std::fgets(line, sizeof(line), file) != nullptr;
  std::fclose(file);
  if (!read) return false;
  char* close_paren = std::strrchr(line, ')');
  if (close_paren == nullptr || close_paren[1] != ' ') return false;
  char* save = nullptr;
  char* token = strtok_r(close_paren + 2, " ", &save);
  for (int field = 3; token != nullptr && field < 22; ++field)
    token = strtok_r(nullptr, " ", &save);
  return token != nullptr && ParseUnsigned(token, 10, output);
}

bool HeaderValid(const protocol::Control& control,
                 const protocol::Evidence& evidence) {
  return std::memcmp(control.magic, protocol::kControlMagic, 8) == 0 &&
      control.version == protocol::kVersion &&
      control.size == sizeof(protocol::Control) &&
      (control.flags == protocol::kConfigured ||
       control.flags == (protocol::kConfigured | protocol::kActive)) &&
      std::memcmp(evidence.magic, protocol::kEvidenceMagic, 8) == 0 &&
      evidence.version == protocol::kVersion &&
      evidence.size == sizeof(protocol::Evidence);
}

bool ParseSet(char* save, DynamicCommand* output) {
  const char* mode_text = strtok_r(nullptr, " \t\r\n", &save);
  const char* bits_text = strtok_r(nullptr, " \t\r\n", &save);
  std::uint64_t mode = 0;
  std::uint64_t bits = 0;
  if (!ParseUnsigned(mode_text, 0, &mode) || !ParseUnsigned(bits_text, 0, &bits) ||
      (mode != static_cast<std::uint32_t>(protocol::Mode::kFreeFlight) &&
       mode != static_cast<std::uint32_t>(protocol::Mode::kOrbital)) ||
      bits > protocol::kKnownInputMask)
    return false;
  DynamicCommand value{};
  value.mode = static_cast<protocol::Mode>(mode);
  value.input_bits = static_cast<std::uint32_t>(bits);
  float* fields[] = {
      &value.yaw_degrees, &value.pitch_degrees, &value.move_speed,
      &value.sensitivity, &value.orbital_distance,
      &value.orbital_zoom_speed, &value.fov_radians,
  };
  for (float* field : fields) {
    const char* token = strtok_r(nullptr, " \t\r\n", &save);
    if (!ParseFloat(token, field)) return false;
  }
  if (strtok_r(nullptr, " \t\r\n", &save) != nullptr ||
      value.move_speed < 0.1f || value.move_speed > 1000.0f ||
      value.orbital_distance < 0.1f || value.orbital_zoom_speed < 0.0f ||
      value.fov_radians <= 0.0f)
    return false;
  *output = value;
  return true;
}

bool ParseAbsolute(char* save, DynamicCommand* output) {
  const char* flags_text = strtok_r(nullptr, " \t\r\n", &save);
  std::uint64_t flags = 0;
  if (!ParseUnsigned(flags_text, 0, &flags) || flags == 0 ||
      (flags & ~protocol::kAbsoluteOverrideMask) != 0)
    return false;
  DynamicCommand value{};
  value.mode = protocol::Mode::kAbsolute;
  value.absolute_override_flags = static_cast<std::uint32_t>(flags);
  float* fields[] = {
      &value.absolute_position[0], &value.absolute_position[1],
      &value.absolute_position[2], &value.absolute_rotation[0],
      &value.absolute_rotation[1], &value.absolute_rotation[2],
      &value.absolute_rotation[3], &value.fov_radians,
  };
  for (float* field : fields) {
    const char* token = strtok_r(nullptr, " \t\r\n", &save);
    if (!ParseFloat(token, field)) return false;
  }
  const float norm_squared =
      value.absolute_rotation[0] * value.absolute_rotation[0] +
      value.absolute_rotation[1] * value.absolute_rotation[1] +
      value.absolute_rotation[2] * value.absolute_rotation[2] +
      value.absolute_rotation[3] * value.absolute_rotation[3];
  if (strtok_r(nullptr, " \t\r\n", &save) != nullptr ||
      value.fov_radians <= 0.0f || norm_squared < 0.998f ||
      norm_squared > 1.002f)
    return false;
  *output = value;
  return true;
}

bool Publish(int mem, std::uintptr_t control_address,
             protocol::Control* control, const DynamicCommand& command) {
  const std::uint64_t current = control->published_command;
  const std::size_t inactive_slot =
      static_cast<std::size_t>((current & 1u) ^ 1u);
  const std::uint64_t ordinal = current >> 1;
  if (ordinal == (UINT64_MAX >> 1)) return false;
  const std::uint64_t publication =
      ((ordinal + 1u) << 1) | static_cast<std::uint64_t>(inactive_slot);
  protocol::Control::Command value{};
  value.mode = command.mode;
  value.input_bits = command.input_bits;
  value.yaw_degrees = command.yaw_degrees;
  value.pitch_degrees = command.pitch_degrees;
  value.move_speed = command.move_speed;
  value.sensitivity = command.sensitivity;
  value.orbital_distance = command.orbital_distance;
  value.orbital_zoom_speed = command.orbital_zoom_speed;
  value.fov_radians = command.fov_radians;
  value.absolute_override_flags = command.absolute_override_flags;
  std::memcpy(value.absolute_position, command.absolute_position,
              sizeof(value.absolute_position));
  std::memcpy(value.absolute_rotation, command.absolute_rotation,
              sizeof(value.absolute_rotation));
  if (!WriteExactVerified(mem,
          control_address + offsetof(protocol::Control, commands) +
              inactive_slot * sizeof(protocol::Control::Command),
          &value, sizeof(value)) ||
      !WriteExactVerified(mem,
          control_address + offsetof(protocol::Control, published_command),
          &publication, sizeof(publication)))
    return false;
  const std::uint32_t active = protocol::kConfigured | protocol::kActive;
  if (!WriteExactVerified(mem,
          control_address + offsetof(protocol::Control, flags),
          &active, sizeof(active)))
    return false;
  control->commands[inactive_slot] = value;
  control->published_command = publication;
  control->flags = active;
  return true;
}

bool ConfigureHud(int mem, std::uintptr_t control_address,
                   protocol::Control* control, const HudIdentity& identity) {
  if (control == nullptr || identity.root == 0 || identity.root_vptr == 0 ||
      identity.interface == 0 || identity.interface_vptr == 0 ||
      identity.hidden_getter == 0 ||
      identity.root > UINTPTR_MAX - kHudInterfaceOffset ||
      identity.interface != identity.root + kHudInterfaceOffset ||
      control->published_hud_visibility != 0 ||
      control->hud_cache_flush_pending != protocol::kHudFlushSettled ||
      control->expected_hud_visibility_wrapper == 0)
    return false;
  std::uintptr_t live_root_vptr = 0;
  std::uintptr_t live_interface_vptr = 0;
  std::uintptr_t live_hidden_getter = 0;
  std::uint32_t getter_code[2]{};
  std::uint32_t branch = 0;
  if (!ReadExact(mem, identity.root, &live_root_vptr,
                  sizeof(live_root_vptr)) ||
      live_root_vptr != identity.root_vptr ||
      !ReadExact(mem, identity.interface, &live_interface_vptr,
                  sizeof(live_interface_vptr)) ||
      live_interface_vptr != identity.interface_vptr ||
      identity.root_vptr > UINTPTR_MAX - protocol::kHudHiddenSlot ||
      !ReadExact(mem, identity.root_vptr + protocol::kHudHiddenSlot,
                 &live_hidden_getter, sizeof(live_hidden_getter)) ||
      live_hidden_getter != identity.hidden_getter ||
      !ReadExact(mem, identity.hidden_getter, getter_code, sizeof(getter_code)) ||
      (getter_code[0] & kHudLoadMask) != kHudLoadValue ||
      getter_code[1] != kArm64Ret ||
      !EncodeArm64Branch(identity.hidden_getter,
                         control->expected_hud_visibility_wrapper, &branch))
    return false;

  std::uint64_t original_code = 0;
  std::memcpy(&original_code, getter_code, sizeof(original_code));
  const std::uint64_t initial_publication =
      (1ull << 2) | protocol::kHudPublicationActive | 1u;
  const std::uint64_t disabled_publication = 0;
  const std::uint32_t pending = protocol::kHudFlushInstall;
  const std::uint64_t flush_sequence = control->hud_cache_flush_sequence + 1u;
  if (flush_sequence == 0 ||
      !WriteExactVerified(
          mem, control_address + offsetof(protocol::Control, expected_hud_root),
          &identity, sizeof(identity)) ||
      !WriteExactVerified(mem, control_address +
          offsetof(protocol::Control, hud_original_code),
          &original_code, sizeof(original_code)) ||
      !WriteExactVerified(mem, control_address +
          offsetof(protocol::Control, hud_patch_instruction),
          &branch, sizeof(branch)) ||
      !WriteExactVerified(mem, control_address +
          offsetof(protocol::Control, hud_cache_flush_target),
          &identity.hidden_getter, sizeof(identity.hidden_getter)) ||
      !WriteExactVerified(mem, control_address +
          offsetof(protocol::Control, hud_cache_flush_sequence),
          &flush_sequence, sizeof(flush_sequence)) ||
      !WriteExactVerified(mem, control_address +
          offsetof(protocol::Control, published_hud_visibility),
          &initial_publication, sizeof(initial_publication)) ||
      !WriteExactVerified(mem, control_address +
          offsetof(protocol::Control, hud_cache_flush_pending),
          &pending, sizeof(pending)))
    return false;
  if (!WriteExactVerified(mem, identity.hidden_getter, &branch,
                          sizeof(branch))) {
    std::uint32_t uncertain_instruction = 0;
    if (ReadExact(mem, identity.hidden_getter, &uncertain_instruction,
                  sizeof(uncertain_instruction)) &&
        uncertain_instruction == branch) {
      const std::uint32_t restore_pending = protocol::kHudFlushRestore;
      (void)WriteExactVerified(mem, control_address +
          offsetof(protocol::Control, published_hud_visibility),
          &disabled_publication, sizeof(disabled_publication));
      (void)WriteExactVerified(mem, control_address +
          offsetof(protocol::Control, hud_cache_flush_pending),
          &restore_pending, sizeof(restore_pending));
      (void)WriteExactVerified(mem, identity.hidden_getter, getter_code,
                               sizeof(getter_code[0]));
    }
    return false;
  }
  control->expected_hud_root = identity.root;
  control->expected_hud_root_vptr = identity.root_vptr;
  control->expected_hud_interface = identity.interface;
  control->expected_hud_interface_vptr = identity.interface_vptr;
  control->expected_hud_hidden_getter = identity.hidden_getter;
  control->published_hud_visibility = initial_publication;
  control->hud_original_code = original_code;
  control->hud_patch_instruction = branch;
  control->hud_cache_flush_pending = pending;
  control->hud_cache_flush_target = identity.hidden_getter;
  control->hud_cache_flush_sequence = flush_sequence;
  return true;
}

bool RestoreHud(int mem, std::uintptr_t control_address,
                 protocol::Control* control) {
  if (control == nullptr || control->expected_hud_root == 0 ||
      control->expected_hud_root_vptr == 0 ||
      control->expected_hud_hidden_getter == 0 ||
      control->hud_original_code == 0 || control->hud_patch_instruction == 0)
    return false;
  const std::uint64_t disabled = 0;
  if (!WriteExactVerified(mem,
          control_address +
              offsetof(protocol::Control, published_hud_visibility),
          &disabled, sizeof(disabled)))
    return false;
  control->published_hud_visibility = 0;
  std::uint32_t live_instruction = 0;
  const std::uint32_t original_instruction =
      static_cast<std::uint32_t>(control->hud_original_code);
  const std::uint32_t pending = protocol::kHudFlushRestore;
  if (!ReadExact(mem, control->expected_hud_hidden_getter, &live_instruction,
                 sizeof(live_instruction)) ||
      (live_instruction != control->hud_patch_instruction &&
       live_instruction != original_instruction) ||
      !WriteExactVerified(mem, control_address +
          offsetof(protocol::Control, hud_cache_flush_pending),
          &pending, sizeof(pending)))
    return false;
  control->hud_cache_flush_pending = pending;
  if (live_instruction == control->hud_patch_instruction) {
    if (!WriteExactVerified(mem, control->expected_hud_hidden_getter,
                            &original_instruction,
                            sizeof(original_instruction)))
      return false;
  }
  return true;
}

bool PublishHud(int mem, std::uintptr_t control_address,
                protocol::Control* control, bool visible,
                std::uint64_t* publication_out) {
  if (control == nullptr || publication_out == nullptr ||
      control->expected_hud_root == 0 ||
      control->expected_hud_hidden_getter == 0)
    return false;
  std::uint32_t live_instruction = 0;
  if (!ReadExact(mem, control->expected_hud_hidden_getter, &live_instruction,
                 sizeof(live_instruction)) ||
      live_instruction != control->hud_patch_instruction)
    return false;
  const std::uint64_t ordinal =
      (control->published_hud_visibility >> 2) + 1u;
  if (ordinal == 0 || ordinal > (UINT64_MAX >> 2)) return false;
  const std::uint64_t publication =
      (ordinal << 2) | protocol::kHudPublicationActive |
      static_cast<std::uint64_t>(visible ? 1u : 0u);
  if (!WriteExactVerified(mem, control_address +
          offsetof(protocol::Control, published_hud_visibility),
          &publication, sizeof(publication)))
    return false;
  control->published_hud_visibility = publication;
  *publication_out = publication;
  return true;
}

void PrintStatus(int mem, std::uintptr_t control_address,
                 std::uintptr_t evidence_address, const char* tag) {
  protocol::Control control{};
  protocol::Evidence evidence{};
  if (!ReadExact(mem, control_address, &control, sizeof(control)) ||
      !ReadExact(mem, evidence_address, &evidence, sizeof(evidence)) ||
      !HeaderValid(control, evidence)) {
    std::printf("CAMERA_TOOL_INPUT_%s invalid=1\n", tag);
  } else {
    const auto& command = control.commands[control.published_command & 1u];
    std::printf(
        "CAMERA_TOOL_INPUT_%s active=%u mode=%u bits=0x%x sequence=%" PRIu64
        " entries=%" PRIu64 " steps=%" PRIu64 " vehicle_reads=%" PRIu64
        " failures=%" PRIu64 " status=%d dt=%.6g max_dt=%.6g"
        " hud_pub=%" PRIu64 " hud_applied=%" PRIu64
        " hud_visible=%u hud_calls=%" PRIu64 " hud_failures=%" PRIu64
        " hud_flush_pending=%u hud_flushes=%" PRIu64 "\n",
        tag, (control.flags & protocol::kActive) != 0 ? 1u : 0u,
        static_cast<unsigned>(command.mode), command.input_bits,
        control.published_command, evidence.wrapper_entries,
        evidence.runtime_steps, evidence.vehicle_reads, evidence.failures,
        evidence.last_status, static_cast<double>(evidence.last_dt_seconds),
        static_cast<double>(evidence.max_dt_seconds),
        control.published_hud_visibility, evidence.last_hud_publication,
        evidence.last_hud_visible, evidence.hud_calls,
        evidence.hud_failures, control.hud_cache_flush_pending,
        evidence.hud_cache_flushes);
  }
  std::fflush(stdout);
}

void PrintCapture(int mem, std::uintptr_t control_address,
                  std::uintptr_t evidence_address) {
  protocol::Control control{};
  protocol::Evidence evidence{};
  if (!ReadExact(mem, control_address, &control, sizeof(control)) ||
      !ReadExact(mem, evidence_address, &evidence, sizeof(evidence)) ||
      !HeaderValid(control, evidence)) {
    std::printf("CAMERA_TOOL_INPUT_CAPTURE invalid=1\n");
  } else {
    std::printf(
        "CAMERA_TOOL_INPUT_CAPTURE active=%u natural="
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g "
        "applied=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
        (control.flags & protocol::kActive) != 0 ? 1u : 0u,
        static_cast<double>(evidence.natural_transform[0]),
        static_cast<double>(evidence.natural_transform[1]),
        static_cast<double>(evidence.natural_transform[2]),
        static_cast<double>(evidence.natural_transform[3]),
        static_cast<double>(evidence.natural_transform[4]),
        static_cast<double>(evidence.natural_transform[5]),
        static_cast<double>(evidence.natural_transform[6]),
        static_cast<double>(evidence.natural_fov),
        static_cast<double>(evidence.applied_transform[0]),
        static_cast<double>(evidence.applied_transform[1]),
        static_cast<double>(evidence.applied_transform[2]),
        static_cast<double>(evidence.applied_transform[3]),
        static_cast<double>(evidence.applied_transform[4]),
        static_cast<double>(evidence.applied_transform[5]),
        static_cast<double>(evidence.applied_transform[6]),
        static_cast<double>(evidence.applied_fov));
  }
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6 || std::strcmp(argv[5], kAcknowledgement) != 0) {
    std::fprintf(stderr,
        "usage: %s PID START_TICKS CONTROL_HEX EVIDENCE_HEX "
        "I_ACCEPT_CAMERA_TOOL_INPUT_STREAM_V2\n", argv[0]);
    return 2;
  }
  std::uint64_t pid_value = 0;
  std::uint64_t start_ticks = 0;
  std::uint64_t control_address = 0;
  std::uint64_t evidence_address = 0;
  if (!ParseUnsigned(argv[1], 10, &pid_value) || pid_value == 0 ||
      pid_value > INT32_MAX ||
      !ParseUnsigned(argv[2], 10, &start_ticks) || start_ticks == 0 ||
      !ParseUnsigned(argv[3], 16, &control_address) || control_address == 0 ||
      !ParseUnsigned(argv[4], 16, &evidence_address) || evidence_address == 0)
    return 2;
  const pid_t pid = static_cast<pid_t>(pid_value);
  std::uint64_t live_ticks = 0;
  if (!ReadStartTicks(pid, &live_ticks) || live_ticks != start_ticks) return 3;
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
  const int mem = open(mem_path, O_RDWR | O_CLOEXEC);
  if (mem < 0) return 4;
  protocol::Control control{};
  protocol::Evidence evidence{};
  if (!ReadExact(mem, control_address, &control, sizeof(control)) ||
      !ReadExact(mem, evidence_address, &evidence, sizeof(evidence)) ||
      !HeaderValid(control, evidence)) {
    close(mem); return 5;
  }
  PrintStatus(mem, control_address, evidence_address, "READY");

  char line[1024]{};
  while (std::fgets(line, sizeof(line), stdin) != nullptr) {
    char* save = nullptr;
    char* verb = strtok_r(line, " \t\r\n", &save);
    if (verb == nullptr) continue;
    if (std::strcmp(verb, "SET") == 0) {
      DynamicCommand command{};
      if (!ParseSet(save, &command) ||
          !Publish(mem, control_address, &control, command)) {
        std::printf("CAMERA_TOOL_INPUT_ERROR command=SET\n");
      } else {
        std::printf("CAMERA_TOOL_INPUT_SET sequence=%" PRIu64
                    " mode=%u bits=0x%x\n",
                    control.published_command,
                    static_cast<unsigned>(command.mode), command.input_bits);
      }
      std::fflush(stdout);
    } else if (std::strcmp(verb, "SETABS") == 0) {
      DynamicCommand command{};
      if (!ParseAbsolute(save, &command) ||
          !Publish(mem, control_address, &control, command)) {
        std::printf("CAMERA_TOOL_INPUT_ERROR command=SETABS\n");
      } else {
        std::printf("CAMERA_TOOL_INPUT_SETABS sequence=%" PRIu64
                    " flags=0x%x\n",
                    control.published_command,
                    command.absolute_override_flags);
      }
      std::fflush(stdout);
    } else if (std::strcmp(verb, "CAPTURE") == 0) {
      PrintCapture(mem, control_address, evidence_address);
    } else if (std::strcmp(verb, "HUDINIT") == 0) {
      HudIdentity identity{};
      std::uint64_t values[5]{};
      bool valid = true;
      for (std::size_t index = 0; index < sizeof(values) / sizeof(values[0]);
           ++index) {
        const char* token = strtok_r(nullptr, " \t\r\n", &save);
        valid = valid && ParseUnsigned(token, 16, &values[index]);
      }
      valid = valid && strtok_r(nullptr, " \t\r\n", &save) == nullptr;
      identity.root = static_cast<std::uintptr_t>(values[0]);
      identity.root_vptr = static_cast<std::uintptr_t>(values[1]);
      identity.interface = static_cast<std::uintptr_t>(values[2]);
      identity.interface_vptr = static_cast<std::uintptr_t>(values[3]);
      identity.hidden_getter = static_cast<std::uintptr_t>(values[4]);
      if (!valid || !ConfigureHud(mem, control_address, &control, identity)) {
        std::printf("CAMERA_TOOL_INPUT_ERROR command=HUDINIT\n");
      } else {
        std::printf(
            "CAMERA_TOOL_INPUT_HUD_READY root=0x%" PRIxPTR
            " interface=0x%" PRIxPTR " hidden_getter=0x%" PRIxPTR "\n",
            identity.root, identity.interface, identity.hidden_getter);
      }
      std::fflush(stdout);
    } else if (std::strcmp(verb, "HUDRESTORE") == 0) {
      if (strtok_r(nullptr, " \t\r\n", &save) != nullptr ||
          !RestoreHud(mem, control_address, &control)) {
        std::printf("CAMERA_TOOL_INPUT_ERROR command=HUDRESTORE\n");
      } else {
        std::printf("CAMERA_TOOL_INPUT_HUD_RESTORED\n");
      }
      std::fflush(stdout);
    } else if (std::strcmp(verb, "HUD") == 0) {
      const char* visible_text = strtok_r(nullptr, " \t\r\n", &save);
      std::uint64_t visible = 0;
      std::uint64_t publication = 0;
      if (!ParseUnsigned(visible_text, 10, &visible) || visible > 1u ||
          strtok_r(nullptr, " \t\r\n", &save) != nullptr ||
          !PublishHud(mem, control_address, &control, visible != 0,
                      &publication)) {
        std::printf("CAMERA_TOOL_INPUT_ERROR command=HUD\n");
      } else {
        std::printf(
            "CAMERA_TOOL_INPUT_HUD_SET publication=%" PRIu64
            " visible=%" PRIu64 "\n",
            publication, visible);
      }
      std::fflush(stdout);
    } else if (std::strcmp(verb, "STATUS") == 0) {
      PrintStatus(mem, control_address, evidence_address, "STATUS");
    } else if (std::strcmp(verb, "DISABLE") == 0) {
      const std::uint32_t inactive = protocol::kConfigured;
      if (!WriteExactVerified(mem,
          control_address + offsetof(protocol::Control, flags),
          &inactive, sizeof(inactive))) {
        std::printf("CAMERA_TOOL_INPUT_ERROR command=DISABLE\n");
      } else {
        control.flags = inactive;
        std::printf("CAMERA_TOOL_INPUT_DISABLED\n");
      }
      std::fflush(stdout);
    } else if (std::strcmp(verb, "QUIT") == 0) {
      PrintStatus(mem, control_address, evidence_address, "FINAL");
      close(mem);
      return 0;
    } else {
      std::printf("CAMERA_TOOL_INPUT_ERROR command=UNKNOWN\n");
      std::fflush(stdout);
    }
  }
  close(mem);
  return 6;
}
