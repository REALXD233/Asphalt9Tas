// Explicitly gated, bounded read-only pointer graph for the active blended-
// camera wrapper.  No virtual/interface method is invoked.

#include "camera_wrapper_graph_v1.h"

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

namespace active = a9tas::camera_active_state_resolver_v1;
namespace graph = a9tas::camera_wrapper_graph_v1;

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_CAMERA_WRAPPER_GRAPH_READ_ONLY_V1";

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

bool ReadMappings(pid_t pid, std::uintptr_t expected_game_base,
                  std::vector<active::Mapping>* output,
                  std::uintptr_t* game_image_end) {
  if (pid <= 0 || expected_game_base == 0 || output == nullptr ||
      game_image_end == nullptr)
    return false;
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
  FILE* file = std::fopen(path, "re");
  if (file == nullptr) return false;
  std::vector<active::Mapping> mappings;
  bool saw_zero_offset_game_mapping = false;
  std::uintptr_t observed_game_end = 0;
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
    if (std::strstr(line, "libAsphalt9.so") != nullptr) {
      if (mapping.begin == expected_game_base && offset == 0)
        saw_zero_offset_game_mapping = true;
      if (mapping.end > observed_game_end) observed_game_end = mapping.end;
    }
  }
  std::fclose(file);
  if (mappings.empty() || !saw_zero_offset_game_mapping ||
      observed_game_end <= expected_game_base)
    return false;
  *output = std::move(mappings);
  *game_image_end = observed_game_end;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5 || std::strcmp(argv[4], kAcknowledgement) != 0) {
    std::fprintf(stderr,
                 "usage: %s PID LIB_BASE_HEX START_TICKS "
                 "I_ACCEPT_CAMERA_WRAPPER_GRAPH_READ_ONLY_V1\n",
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
    std::fprintf(stderr, "invalid wrapper-graph arguments; no process opened\n");
    return 2;
  }
  const pid_t pid = static_cast<pid_t>(pid_value);
  const auto game_base = static_cast<std::uintptr_t>(base_value);
  std::uint64_t observed_start_ticks = 0;
  if (!ReadProcessStartTicks(pid, &observed_start_ticks) ||
      observed_start_ticks != expected_start_ticks) {
    std::fprintf(stderr,
                 "wrapper-graph PID/start-time mismatch; no mem open\n");
    return 3;
  }
  std::vector<active::Mapping> mappings;
  std::uintptr_t game_image_end = 0;
  if (!ReadMappings(pid, game_base, &mappings, &game_image_end)) return 4;

  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
  ProcessMemory memory{};
  memory.fd = open(path, O_RDONLY | O_CLOEXEC);
  if (memory.fd < 0) return 5;
  active::Resolution resolution{};
  const active::Result resolve_result = active::Resolve(
      {&memory, &ReadRemote}, mappings.data(), mappings.size(), game_base,
      &resolution);
  graph::Graph wrapper_graph{};
  graph::Result graph_result = graph::Result::kInvalidArgument;
  if (resolve_result == active::Result::kOk) {
    graph_result = graph::Build(
        {&memory, &ReadRemote}, mappings.data(), mappings.size(), game_base,
        game_image_end, resolution, &wrapper_graph);
  }
  close(memory.fd);

  std::printf(
      "CAMERA_WRAPPER_GRAPH_V1 resolve_result=%d graph_result=%d pid=%d "
      "start_ticks=%" PRIu64 " mapping_count=%zu scanned=%" PRIu64
      " candidates=%u race_view=0x%" PRIxPTR
      " manager=0x%" PRIxPTR " wrapper=0x%" PRIxPTR
      " primary_vptr=0x%" PRIxPTR " game_image_end=0x%" PRIxPTR
      " mapped_pointers=%u private_object_pointers=%u"
      " known_interface_vptrs=%u object_head_image_pointers=%u"
      " dereferences=%u\n",
      static_cast<int>(resolve_result), static_cast<int>(graph_result),
      static_cast<int>(pid), observed_start_ticks, mappings.size(),
      resolution.bytes_scanned, resolution.structural_candidates,
      resolution.race_view, resolution.camera_manager,
      resolution.blended_camera, resolution.camera_vptr, game_image_end,
      wrapper_graph.mapped_pointer_count,
      wrapper_graph.private_object_pointer_count,
      wrapper_graph.known_interface_vptr_count,
      wrapper_graph.object_head_image_pointer_count,
      wrapper_graph.pointer_dereference_count);
  if (graph_result == graph::Result::kOk) {
    for (const graph::WordNode& node : wrapper_graph.words) {
      std::printf(
          "CAMERA_WRAPPER_WORD offset=0x%" PRIxPTR
          " value=0x%" PRIxPTR " value_flags=0x%x"
          " value_image_rva=0x%" PRIxPTR " known_interface_vptr=%u"
          " static_capture_slot=%u"
          " pointee_head=0x%" PRIxPTR " head_flags=0x%x"
          " head_image_rva=0x%" PRIxPTR "\n",
          node.offset, node.value, node.value_mapping_flags,
          node.value_image_rva,
          node.value_is_known_interface_vptr ? 1u : 0u,
          node.statically_interesting ? 1u : 0u,
          node.pointee_head, node.pointee_head_mapping_flags,
          node.pointee_head_image_rva);
    }
  }
  std::printf(
      "CAMERA_WRAPPER_GRAPH_END device_access=1 gameplay_writes=0 "
      "ptrace_calls=0 invoked_methods=0 wrapper_bytes=%zu\n",
      graph::kWrapperSize);
  return resolve_result == active::Result::kOk &&
                 graph_result == graph::Result::kOk
             ? 0
             : 6;
}
