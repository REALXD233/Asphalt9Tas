// Narrow host transaction for the preloaded RaceView recorder.
//
// The only game-owned write is the aligned callback pointer at node +0x58.
// Payload-owned control/evidence are configured before that pointer is
// replaced.  INSTALL and FINALIZE/ROLLBACK stop the whole process only for the
// bounded verify/write/verify transaction; no ptrace or guest call is used.

#include "camera_raceview_callback_node_v1.h"
#include "camera_raceview_record_protocol_v1.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace callback = a9tas::camera_raceview_callback_node_v1;
namespace record = a9tas::camera_raceview_record_v1;

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_RACEVIEW_RECORD_SINGLE_SLOT_V1";
constexpr char kPayloadBasename[] =
    "liba9tas_camera_raceview_record_v1_build_only.so";
constexpr char kPayloadSha256[] =
    "c253eecb2244756edb53d54ad5f546301522378b7c83e4bbaaa1d491342b2639";
constexpr std::uintptr_t kWrapperRva = 0x1BF0;
constexpr std::uintptr_t kControlPointerRva = 0x4E00;
constexpr std::uintptr_t kEvidencePointerRva = 0x4EC0;
constexpr std::uintptr_t kFramesPointerRva = 0x4EC8;
constexpr std::uintptr_t kControlStorageRva = 0x4D80;
constexpr std::uintptr_t kEvidenceStorageRva = 0x4E40;
constexpr std::uintptr_t kFramesStorageRva = 0x4F00;
constexpr std::uintptr_t kManagerShapeOffset = 0xE8;
constexpr std::uint64_t kStopTimeoutMs = 2000;
constexpr std::uint8_t kWrapperSignature[24] = {
    0xfd, 0x7b, 0xbb, 0xa9, 0xfa, 0x67, 0x01, 0xa9,
    0xf8, 0x5f, 0x02, 0xa9, 0xf6, 0x57, 0x03, 0xa9,
    0xf4, 0x4f, 0x04, 0xa9, 0xfd, 0x03, 0x00, 0x91,
};

enum class Action : std::uint32_t {
  kInstall = 1,
  kStatus = 2,
  kFinalize = 3,
  kRollback = 4,
};

enum ReportFlag : std::uint32_t {
  kProcessIdentity = 1u << 0,
  kPayloadResolved = 1u << 1,
  kNodeIdentity = 1u << 2,
  kPayloadConfigured = 1u << 3,
  kProcessStopped = 1u << 4,
  kSingleSlotInstalled = 1u << 5,
  kEvidenceComplete = 1u << 6,
  kOriginalSlotFinal = 1u << 7,
  kProcessResumed = 1u << 8,
  kOutputWritten = 1u << 9,
};

#pragma pack(push, 1)
struct Report {
  char magic[8];
  std::uint32_t version;
  std::uint32_t size;
  std::uint32_t action;
  std::uint32_t flags;
  std::uint64_t pid;
  std::uint64_t start_ticks;
  std::uint64_t game_base;
  std::uint64_t payload_base;
  std::uint64_t manager;
  std::uint64_t shape;
  std::uint64_t node;
  std::uint64_t node_vptr;
  std::uint64_t original_callback;
  std::uint64_t wrapper;
  std::uint64_t control;
  std::uint64_t evidence_address;
  std::uint64_t frames;
  std::uint32_t requested_frames;
  std::uint32_t captured_frames;
  std::uint64_t payload_write_attempts;
  std::uint64_t game_write_attempts;
  std::uint64_t rollback_attempts;
  std::uint64_t read_errors;
  std::uint64_t semantic_errors;
  record::Evidence evidence;
};
#pragma pack(pop)

struct Mapping {
  std::uintptr_t begin{};
  std::uintptr_t end{};
  std::uint64_t file_offset{};
  char permissions[5]{};
  std::string path;
};

struct PayloadLayout {
  std::uintptr_t base{};
  std::uintptr_t wrapper{};
  std::uintptr_t control{};
  std::uintptr_t evidence{};
  std::uintptr_t frames{};
  std::string path;
};

struct NodeSnapshot {
  std::uintptr_t node{};
  std::uintptr_t vptr{};
  std::uint8_t enabled{};
  std::uintptr_t owner{};
  std::uintptr_t self{};
  std::uintptr_t function{};
  std::uintptr_t context{};
  std::uintptr_t shape{};
};

static_assert(sizeof(record::Control) == 128);
static_assert(sizeof(record::Evidence) == 128);
static_assert(sizeof(record::Frame) == 112);

bool ParseUnsigned(const char* text, int base, std::uint64_t* output) {
  if (text == nullptr || output == nullptr || *text == '\0') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, base);
  if (errno != 0 || end == text || *end != '\0') return false;
  *output = static_cast<std::uint64_t>(value);
  return true;
}

bool ParseAction(const char* text, Action* output) {
  if (text == nullptr || output == nullptr) return false;
  if (std::strcmp(text, "install") == 0) *output = Action::kInstall;
  else if (std::strcmp(text, "status") == 0) *output = Action::kStatus;
  else if (std::strcmp(text, "finalize") == 0) *output = Action::kFinalize;
  else if (std::strcmp(text, "rollback") == 0) *output = Action::kRollback;
  else return false;
  return true;
}

bool ReadExact(int fd, std::uintptr_t address, void* output,
               std::size_t size) {
  auto* bytes = static_cast<std::uint8_t*>(output);
  std::size_t done = 0;
  while (done < size) {
    const ssize_t count = pread(fd, bytes + done, size - done,
                                static_cast<off_t>(address + done));
    if (count <= 0) return false;
    done += static_cast<std::size_t>(count);
  }
  return true;
}

bool WriteExactVerified(int fd, std::uintptr_t address, const void* input,
                        std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(input);
  std::size_t done = 0;
  while (done < size) {
    const ssize_t count = pwrite(fd, bytes + done, size - done,
                                 static_cast<off_t>(address + done));
    if (count <= 0) return false;
    done += static_cast<std::size_t>(count);
  }
  std::vector<std::uint8_t> observed(size);
  return ReadExact(fd, address, observed.data(), observed.size()) &&
         std::memcmp(observed.data(), input, size) == 0;
}

bool ReadStartTicks(pid_t pid, std::uint64_t* output) {
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
      errno = 0;
      char* parsed = nullptr;
      const unsigned long long value = std::strtoull(cursor, &parsed, 10);
      if (errno != 0 || parsed != end || value == 0) return false;
      *output = static_cast<std::uint64_t>(value);
      return true;
    }
    cursor = end;
  }
  return false;
}

bool ReadMappings(pid_t pid, std::vector<Mapping>* output) {
  char maps_path[64]{};
  std::snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps",
                static_cast<int>(pid));
  FILE* file = std::fopen(maps_path, "re");
  if (file == nullptr) return false;
  std::vector<Mapping> mappings;
  char line[2048]{};
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    unsigned long long begin = 0, end = 0, offset = 0;
    char permissions[5]{}, raw_path[1024]{};
    const int fields = std::sscanf(
        line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &begin, &end,
        permissions, &offset, raw_path);
    if (fields < 4 || begin >= end) continue;
    std::string clean = fields == 5 ? raw_path : "";
    while (!clean.empty() && clean.front() == ' ') clean.erase(0, 1);
    Mapping mapping{static_cast<std::uintptr_t>(begin),
                    static_cast<std::uintptr_t>(end), offset, {}, clean};
    std::memcpy(mapping.permissions, permissions, 4);
    mappings.push_back(std::move(mapping));
  }
  std::fclose(file);
  if (mappings.empty()) return false;
  *output = std::move(mappings);
  return true;
}

const Mapping* MappingAt(const std::vector<Mapping>& mappings,
                         std::uintptr_t address, std::size_t size) {
  if (size == 0 || address > UINTPTR_MAX - size) return nullptr;
  const std::uintptr_t end = address + size;
  for (const auto& mapping : mappings)
    if (address >= mapping.begin && end <= mapping.end) return &mapping;
  return nullptr;
}

bool PrivateObject(const Mapping* mapping) {
  return mapping != nullptr && mapping->permissions[0] == 'r' &&
         mapping->permissions[3] == 'p';
}

bool PrivateWritableRange(const std::vector<Mapping>& mappings,
                          std::uintptr_t address, std::size_t size,
                          const std::string& payload_path) {
  if (size == 0 || address > UINTPTR_MAX - size) return false;
  const std::uintptr_t end = address + size;
  std::uintptr_t cursor = address;
  bool first = true;
  while (cursor < end) {
    const Mapping* mapping = MappingAt(mappings, cursor, 1);
    if (mapping == nullptr || mapping->permissions[0] != 'r' ||
        mapping->permissions[1] != 'w' || mapping->permissions[3] != 'p' ||
        (first && mapping->path != payload_path) ||
        (!mapping->path.empty() && mapping->path != payload_path))
      return false;
    cursor = std::min(end, mapping->end);
    first = false;
  }
  return cursor == end;
}

bool ResolvePayload(int mem, const std::vector<Mapping>& mappings,
                    PayloadLayout* output) {
  std::uintptr_t base = UINTPTR_MAX;
  std::string path;
  std::uint32_t matches = 0;
  for (const auto& mapping : mappings) {
    if (mapping.path.find(kPayloadBasename) == std::string::npos) continue;
    if (mapping.path.find(" (deleted)") != std::string::npos) return false;
    if (path.empty()) path = mapping.path;
    if (mapping.path != path) return false;
    if (mapping.file_offset == 0) base = std::min(base, mapping.begin);
    ++matches;
  }
  if (matches < 2 || base == UINTPTR_MAX || path.empty()) return false;
  PayloadLayout layout{};
  layout.base = base;
  layout.path = path;
  layout.wrapper = base + kWrapperRva;
  const std::uintptr_t control_pointer = base + kControlPointerRva;
  const std::uintptr_t evidence_pointer = base + kEvidencePointerRva;
  const std::uintptr_t frames_pointer = base + kFramesPointerRva;
  std::uint8_t signature[sizeof(kWrapperSignature)]{};
  if (!ReadExact(mem, layout.wrapper, signature, sizeof(signature)) ||
      std::memcmp(signature, kWrapperSignature, sizeof(signature)) != 0 ||
      !ReadExact(mem, control_pointer, &layout.control,
                 sizeof(layout.control)) ||
      !ReadExact(mem, evidence_pointer, &layout.evidence,
                 sizeof(layout.evidence)) ||
      !ReadExact(mem, frames_pointer, &layout.frames,
                 sizeof(layout.frames)))
    return false;
  const Mapping* wrapper_map =
      MappingAt(mappings, layout.wrapper, sizeof(kWrapperSignature));
  const Mapping* control_map =
      MappingAt(mappings, layout.control, sizeof(record::Control));
  const Mapping* evidence_map =
      MappingAt(mappings, layout.evidence, sizeof(record::Evidence));
  const bool frames_range = PrivateWritableRange(
      mappings, layout.frames,
      sizeof(record::Frame) * static_cast<std::size_t>(record::kMaximumFrames),
      path);
  if (wrapper_map == nullptr || wrapper_map->path != path ||
      wrapper_map->permissions[0] != 'r' ||
      wrapper_map->permissions[1] == 'w' || !PrivateObject(control_map) ||
      !PrivateObject(evidence_map) || !frames_range ||
      control_map->permissions[1] != 'w' ||
      evidence_map->permissions[1] != 'w' ||
      control_map->path != path || evidence_map->path != path ||
      layout.control != base + kControlStorageRva ||
      layout.evidence != base + kEvidenceStorageRva ||
      layout.frames != base + kFramesStorageRva ||
      (layout.control & 63u) != 0 ||
      (layout.evidence & 63u) != 0 || (layout.frames & 63u) != 0)
    return false;
  *output = layout;
  return true;
}

bool ReadNode(int mem, const std::vector<Mapping>& mappings,
               std::uintptr_t game_base, std::uintptr_t manager,
               std::uintptr_t expected_shape, NodeSnapshot* output,
               std::uintptr_t expected_node_vptr_rva =
                   callback::kExpectedNodeVptrRva) {
  if (game_base == 0 || manager == 0 || output == nullptr ||
      manager > UINTPTR_MAX - callback::kManagerNodeOffset ||
      manager > UINTPTR_MAX - kManagerShapeOffset)
    return false;
  std::uintptr_t node = 0;
  std::uintptr_t shape = 0;
  if (!ReadExact(mem, manager + callback::kManagerNodeOffset, &node,
                 sizeof(node)) ||
      !ReadExact(mem, manager + kManagerShapeOffset, &shape, sizeof(shape)) ||
      node == 0 || shape == 0 || shape != expected_shape ||
      !PrivateObject(MappingAt(mappings, manager, 0x110)) ||
      !PrivateObject(MappingAt(mappings, node, callback::kNodeSize)) ||
      !PrivateObject(MappingAt(mappings, shape, 0x98)))
    return false;
  std::array<std::uint8_t, callback::kNodeSize> bytes{};
  if (!ReadExact(mem, node, bytes.data(), bytes.size())) return false;
  NodeSnapshot snapshot{};
  snapshot.node = node;
  snapshot.shape = shape;
  snapshot.enabled = bytes[callback::kEnabledOffset];
  std::memcpy(&snapshot.vptr, bytes.data(), sizeof(snapshot.vptr));
  std::memcpy(&snapshot.owner, bytes.data() + callback::kOwnerOffset,
              sizeof(snapshot.owner));
  std::memcpy(&snapshot.self, bytes.data() + callback::kSelfOffset,
              sizeof(snapshot.self));
  std::memcpy(&snapshot.function, bytes.data() + callback::kCallbackOffset,
              sizeof(snapshot.function));
  std::memcpy(&snapshot.context, bytes.data() + callback::kContextOffset,
              sizeof(snapshot.context));
  if (snapshot.vptr != game_base + expected_node_vptr_rva ||
      snapshot.enabled != 1 || snapshot.owner != manager ||
      snapshot.self != node + callback::kOwnerOffset ||
      snapshot.context != 0)
    return false;
  *output = snapshot;
  return true;
}

std::uint64_t MonotonicMs() {
  timespec now{};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
  return static_cast<std::uint64_t>(now.tv_sec) * 1000u +
         static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
}

bool ProcessStopped(pid_t pid) {
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(pid));
  FILE* file = std::fopen(path, "re");
  if (file == nullptr) return false;
  char line[256]{};
  bool stopped = false;
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    char state = 0;
    if (std::sscanf(line, "State:\t%c", &state) == 1) {
      stopped = state == 'T' || state == 't';
      break;
    }
  }
  std::fclose(file);
  return stopped;
}

bool StopProcess(pid_t pid) {
  if (kill(pid, SIGSTOP) != 0) return false;
  const std::uint64_t deadline = MonotonicMs() + kStopTimeoutMs;
  while (MonotonicMs() < deadline) {
    if (ProcessStopped(pid)) return true;
    usleep(1000);
  }
  return false;
}

bool ResumeProcess(pid_t pid) { return kill(pid, SIGCONT) == 0; }

bool EvidenceHeader(const record::Evidence& evidence) {
  return std::memcmp(evidence.magic, record::kEvidenceMagic, 8) == 0 &&
         evidence.version == record::kVersion &&
         evidence.size == sizeof(record::Evidence);
}

bool WriteOutput(const char* path, Report* report, int mem,
                 std::uintptr_t frames, std::uint32_t frame_count,
                 bool include_frames) {
  if (path == nullptr || *path == '\0' || access(path, F_OK) == 0) return false;
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) return false;
  report->flags |= kOutputWritten;
  bool ok = std::fwrite(report, sizeof(*report), 1, file) == 1;
  if (ok && include_frames) {
    std::array<record::Frame, 64> buffer{};
    std::uint32_t cursor = 0;
    while (ok && cursor < frame_count) {
      const std::uint32_t amount =
          std::min<std::uint32_t>(frame_count - cursor, buffer.size());
      const std::size_t bytes = amount * sizeof(record::Frame);
      ok = ReadExact(mem, frames + cursor * sizeof(record::Frame),
                     buffer.data(), bytes) &&
           std::fwrite(buffer.data(), bytes, 1, file) == 1;
      cursor += amount;
    }
  }
  ok = ok && std::fflush(file) == 0 && std::ferror(file) == 0;
  const bool close_ok = std::fclose(file) == 0;
  if (!ok || !close_ok) {
    report->flags &= ~kOutputWritten;
    std::remove(path);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 10 || std::strcmp(argv[9], kAcknowledgement) != 0) {
    std::fprintf(stderr,
                 "usage: %s ACTION PID START_TICKS GAME_BASE_HEX "
                 "MANAGER_HEX SHAPE_HEX FRAMES OUTPUT "
                 "I_ACCEPT_RACEVIEW_RECORD_SINGLE_SLOT_V1\n",
                 argv[0]);
    return 2;
  }
  Action action{};
  std::uint64_t values[6]{};
  const int bases[6] = {10, 10, 16, 16, 16, 10};
  if (!ParseAction(argv[1], &action)) return 2;
  for (int i = 0; i < 6; ++i)
    if (!ParseUnsigned(argv[i + 2], bases[i], &values[i])) return 2;
  if (values[0] == 0 || values[0] > INT32_MAX || values[1] == 0 ||
      values[2] == 0 || values[3] == 0 || values[4] == 0 ||
      values[5] == 0 || values[5] > record::kMaximumFrames ||
      access(argv[8], F_OK) == 0)
    return 2;

  const pid_t pid = static_cast<pid_t>(values[0]);
  const std::uint64_t expected_ticks = values[1];
  const auto game_base = static_cast<std::uintptr_t>(values[2]);
  const auto manager = static_cast<std::uintptr_t>(values[3]);
  const auto shape = static_cast<std::uintptr_t>(values[4]);
  const auto frame_count = static_cast<std::uint32_t>(values[5]);

  Report report{};
  std::memcpy(report.magic, "A9CRTR1", 8);
  report.version = 1;
  report.size = sizeof(report);
  report.action = static_cast<std::uint32_t>(action);
  report.pid = values[0];
  report.start_ticks = expected_ticks;
  report.game_base = game_base;
  report.manager = manager;
  report.shape = shape;
  report.requested_frames = frame_count;

  std::uint64_t current_ticks = 0;
  if (!ReadStartTicks(pid, &current_ticks) || current_ticks != expected_ticks)
    return 3;
  report.flags |= kProcessIdentity;

  std::vector<Mapping> mappings;
  if (!ReadMappings(pid, &mappings)) return 3;
  char mem_path[64]{};
  std::snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem",
                static_cast<int>(pid));
  const int open_flags = action == Action::kStatus ? O_RDONLY : O_RDWR;
  const int mem = open(mem_path, open_flags | O_CLOEXEC);
  if (mem < 0) return 4;

  PayloadLayout payload{};
  NodeSnapshot node{};
  if (!ResolvePayload(mem, mappings, &payload)) {
    close(mem);
    return 3;
  }
  report.flags |= kPayloadResolved;
  report.payload_base = payload.base;
  report.wrapper = payload.wrapper;
  report.control = payload.control;
  report.evidence_address = payload.evidence;
  report.frames = payload.frames;
  if (!ReadNode(mem, mappings, game_base, manager, shape, &node)) {
    close(mem);
    return 3;
  }
  report.flags |= kNodeIdentity;
  report.node = node.node;
  report.node_vptr = node.vptr;
  report.original_callback = game_base + callback::kExpectedCallbackRva;
  if (node.function != report.original_callback &&
      node.function != payload.wrapper) {
    close(mem);
    return 3;
  }

  record::Control control{};
  record::Evidence evidence{};
  if (action == Action::kInstall) {
    if (node.function != report.original_callback) {
      close(mem);
      return 5;
    }
    std::memcpy(control.magic, record::kControlMagic, 8);
    control.version = record::kVersion;
    control.size = sizeof(control);
    control.flags = record::kConfigured | record::kContinuousCapture;
    control.frame_count = frame_count;
    control.expected_manager = manager;
    control.expected_node = node.node;
    control.expected_shape = shape;
    control.original_callback = report.original_callback;
    control.expected_node_vptr = node.vptr;
    control.frame_permit = record::kContinuousPermit;
    std::memcpy(evidence.magic, record::kEvidenceMagic, 8);
    evidence.version = record::kVersion;
    evidence.size = sizeof(evidence);
    report.payload_write_attempts = 2;
    if (!WriteExactVerified(mem, payload.evidence, &evidence,
                            sizeof(evidence)) ||
        !WriteExactVerified(mem, payload.control, &control, sizeof(control))) {
      close(mem);
      return 6;
    }
    report.flags |= kPayloadConfigured;
    if (!StopProcess(pid)) {
      (void)ResumeProcess(pid);
      close(mem);
      return 7;
    }
    report.flags |= kProcessStopped;
    std::vector<Mapping> stopped_maps;
    NodeSnapshot stopped_node{};
    const bool pinned = ReadStartTicks(pid, &current_ticks) &&
        current_ticks == expected_ticks && ReadMappings(pid, &stopped_maps) &&
        ReadNode(mem, stopped_maps, game_base, manager, shape, &stopped_node) &&
        stopped_node.node == node.node && stopped_node.vptr == node.vptr &&
        stopped_node.function == report.original_callback;
    bool installed = false;
    if (pinned) {
      ++report.game_write_attempts;
      installed = WriteExactVerified(
          mem, node.node + callback::kCallbackOffset, &payload.wrapper,
          sizeof(payload.wrapper));
    }
    if (!installed) {
      std::uintptr_t current_function = 0;
      if (ReadExact(mem, node.node + callback::kCallbackOffset,
                    &current_function, sizeof(current_function)) &&
          current_function == payload.wrapper) {
        ++report.rollback_attempts;
        (void)WriteExactVerified(
            mem, node.node + callback::kCallbackOffset,
            &report.original_callback, sizeof(report.original_callback));
      }
      (void)ResumeProcess(pid);
      close(mem);
      return 8;
    }
    report.flags |= kSingleSlotInstalled;
    if (!ResumeProcess(pid)) {
      std::uintptr_t current_function = 0;
      if (ReadExact(mem, node.node + callback::kCallbackOffset,
                    &current_function, sizeof(current_function)) &&
          current_function == payload.wrapper) {
        ++report.rollback_attempts;
        (void)WriteExactVerified(
            mem, node.node + callback::kCallbackOffset,
            &report.original_callback, sizeof(report.original_callback));
      }
      (void)ResumeProcess(pid);
      close(mem);
      return 9;
    }
    report.flags |= kProcessResumed;
  } else {
    if (!ReadExact(mem, payload.control, &control, sizeof(control)) ||
        !ReadExact(mem, payload.evidence, &evidence, sizeof(evidence)) ||
        !EvidenceHeader(evidence)) {
      close(mem);
      return 6;
    }
    // Exact configured identity check; no wildcard attachment to stale state.
    const bool configured =
        std::memcmp(control.magic, record::kControlMagic, 8) == 0 &&
        control.version == record::kVersion &&
        control.size == sizeof(control) &&
        control.flags ==
            (record::kConfigured | record::kContinuousCapture) &&
        control.frame_count == frame_count &&
        control.expected_manager == manager &&
        control.expected_node == node.node &&
        control.expected_shape == shape &&
        control.original_callback == report.original_callback &&
        control.expected_node_vptr == node.vptr &&
        control.frame_permit == record::kContinuousPermit;
    if (!configured) {
      close(mem);
      return 6;
    }
    report.flags |= kPayloadConfigured;
    report.evidence = evidence;
    report.captured_frames = evidence.recorded_frames;
    const bool complete =
        evidence.processed_frames == frame_count &&
        evidence.recorded_frames == frame_count &&
        evidence.claimed_permits == frame_count && evidence.failures == 0 &&
        evidence.recursive_entries == 0 &&
        evidence.original_calls == evidence.wrapper_entries &&
        evidence.original_returns == evidence.original_calls &&
        evidence.last_status == record::kComplete;
    if (complete) report.flags |= kEvidenceComplete;
    if (action == Action::kStatus) {
      const bool written = WriteOutput(argv[8], &report, mem, payload.frames,
                                       0, false);
      close(mem);
      std::printf(
          "RACEVIEW_RECORD_STATUS complete=%u captured=%u entries=%" PRIu64
          " failures=%" PRIu64 " payload_sha256=%s\n",
          complete ? 1u : 0u, evidence.processed_frames,
          evidence.wrapper_entries, evidence.failures, kPayloadSha256);
      return written ? 0 : 10;
    }
    if (action == Action::kFinalize && !complete) {
      close(mem);
      return 11;
    }
    if (!StopProcess(pid)) {
      (void)ResumeProcess(pid);
      close(mem);
      return 7;
    }
    report.flags |= kProcessStopped;
    std::vector<Mapping> stopped_maps;
    NodeSnapshot stopped_node{};
    bool restored = ReadMappings(pid, &stopped_maps) &&
        ReadNode(mem, stopped_maps, game_base, manager, shape, &stopped_node) &&
        stopped_node.node == node.node && stopped_node.vptr == node.vptr;
    if (restored && stopped_node.function == payload.wrapper) {
      ++report.game_write_attempts;
      ++report.rollback_attempts;
      restored = WriteExactVerified(
          mem, node.node + callback::kCallbackOffset,
          &report.original_callback, sizeof(report.original_callback));
    } else if (restored) {
      restored = stopped_node.function == report.original_callback;
    }
    if (restored) report.flags |= kOriginalSlotFinal;
    const bool resumed = ResumeProcess(pid);
    if (resumed) report.flags |= kProcessResumed;
    if (!restored || !resumed) {
      close(mem);
      return 12;
    }
  }

  report.evidence = evidence;
  if (action == Action::kInstall) {
    (void)ReadExact(mem, payload.evidence, &report.evidence,
                    sizeof(report.evidence));
  }
  const bool include_frames = action == Action::kFinalize;
  const bool written = WriteOutput(argv[8], &report, mem, payload.frames,
                                   frame_count, include_frames);
  close(mem);
  std::printf(
      "RACEVIEW_RECORD_TRANSACTION action=%u flags=0x%x node=0x%" PRIx64
      " original=0x%" PRIx64 " wrapper=0x%" PRIx64
      " game_writes=%" PRIu64 " payload_sha256=%s output=%s\n",
      report.action, report.flags, report.node, report.original_callback,
      report.wrapper, report.game_write_attempts, kPayloadSha256, argv[8]);
  return written ? 0 : 10;
}
