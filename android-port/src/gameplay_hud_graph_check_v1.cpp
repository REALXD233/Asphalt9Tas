// Explicitly gated, bounded, read-only gameplay HUD graph diagnostic.

#include "gameplay_hud_graph_v1.h"

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
namespace hud = a9tas::gameplay_hud_graph_v1;

namespace {

constexpr char kAcknowledgement[] =
    "I_ACCEPT_GAMEPLAY_HUD_GRAPH_READ_ONLY_V1";
constexpr std::size_t kRegisteredVtableSlots = 24;
constexpr std::uintptr_t kWidgetVisibilitySlot = 0x50;
constexpr std::uintptr_t kGameplayGuiInterfaceOffset = 0x238;
constexpr std::uintptr_t kGameplayGuiSetHudVisibleSlot = 0x20;
constexpr std::uintptr_t kGameplayGuiPrimaryNodeOffset = 0x298;
constexpr std::uintptr_t kGameplayGuiSecondaryNodeOffset = 0x2A8;
constexpr std::uintptr_t kGameplayGuiPrimarySetVisibleSlot = 0x110;
constexpr std::uintptr_t kGameplayGuiPrimaryGetVisibleSlot = 0x128;
constexpr std::uintptr_t kGameplayGuiSecondarySetVisibleSlot = 0x118;
constexpr std::uintptr_t kGameplayGuiSecondaryGetVisibleSlot = 0x128;
constexpr std::uintptr_t kGameplayGuiRootHiddenSlot = 0x170;

struct HudObjectCandidate {
  std::uintptr_t field_offset{};
  std::uintptr_t object{};
  std::uintptr_t vptr{};
  std::uintptr_t visibility_method{};
};

struct GameplayGuiVisibilityChain {
  bool interface_valid{};
  std::uintptr_t interface_object{};
  std::uintptr_t interface_vptr{};
  std::uintptr_t set_hud_visible{};
  std::uintptr_t primary_node{};
  std::uintptr_t primary_vptr{};
  std::uintptr_t primary_set_visible{};
  std::uintptr_t primary_get_visible{};
  std::uintptr_t secondary_node{};
  std::uintptr_t secondary_vptr{};
  std::uintptr_t secondary_set_visible{};
  std::uintptr_t secondary_get_visible{};
};

struct ProcessMemory { int fd{-1}; };

bool ReadRemote(void* context, std::uintptr_t address, void* output,
                std::size_t size) {
  auto* memory = static_cast<ProcessMemory*>(context);
  if (memory == nullptr || memory->fd < 0 || output == nullptr) return false;
  auto* cursor = static_cast<std::uint8_t*>(output);
  std::size_t done = 0;
  while (done < size) {
    const ssize_t amount = pread(memory->fd, cursor + done, size - done,
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
      char saved = *end;
      *end = '\0';
      const bool ok = ParseUnsigned(cursor, 10, output) && *output != 0;
      *end = saved;
      return ok;
    }
    cursor = end;
  }
  return false;
}

bool ReadMappings(pid_t pid, std::uintptr_t expected_base,
                  std::vector<active::Mapping>* output,
                  std::uintptr_t* game_end) {
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
  FILE* file = std::fopen(path, "re");
  if (file == nullptr) return false;
  std::vector<active::Mapping> mappings;
  bool base_seen = false;
  std::uintptr_t end_seen = 0;
  char line[2048]{};
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    unsigned long long begin = 0, end = 0, offset = 0;
    char perms[5]{};
    if (std::sscanf(line, "%llx-%llx %4s %llx", &begin, &end, perms,
                    &offset) != 4 || begin >= end)
      continue;
    active::Mapping mapping{};
    mapping.begin = static_cast<std::uintptr_t>(begin);
    mapping.end = static_cast<std::uintptr_t>(end);
    mapping.readable = perms[0] == 'r';
    mapping.writable = perms[1] == 'w';
    mapping.executable = perms[2] == 'x';
    mapping.private_mapping = perms[3] == 'p';
    mappings.push_back(mapping);
    if (std::strstr(line, "libAsphalt9.so") != nullptr) {
      if (mapping.begin == expected_base && offset == 0) base_seen = true;
      if (mapping.end > end_seen) end_seen = mapping.end;
    }
  }
  std::fclose(file);
  if (!base_seen || end_seen <= expected_base || mappings.empty()) return false;
  *output = std::move(mappings);
  *game_end = end_seen;
  return true;
}

bool ReadVirtualMethod(ProcessMemory* memory,
                       const std::vector<active::Mapping>& mappings,
                       std::uintptr_t game_base, std::uintptr_t game_end,
                       std::uintptr_t object, std::uintptr_t slot,
                       std::uintptr_t* vptr_out,
                       std::uintptr_t* method_out) {
  if (memory == nullptr || vptr_out == nullptr || method_out == nullptr ||
      !active::AlignedPointer(object) ||
      !active::ObjectMapping(active::MappingAt(
          mappings.data(), mappings.size(), object, sizeof(std::uintptr_t))))
    return false;
  std::uintptr_t vptr = 0;
  std::uintptr_t method_slot = 0;
  std::uintptr_t method = 0;
  if (!ReadRemote(memory, object, &vptr, sizeof(vptr)) ||
      !hud::InGameImage(vptr, game_base, game_end) ||
      !active::Add(vptr, slot, &method_slot) ||
      !hud::InGameImage(method_slot, game_base, game_end) ||
      !ReadRemote(memory, method_slot, &method, sizeof(method)) ||
      !hud::InGameImage(method, game_base, game_end))
    return false;
  const active::Mapping* method_mapping = active::MappingAt(
      mappings.data(), mappings.size(), method, sizeof(std::uint32_t));
  // Houdini exposes translated guest code through a readable, non-writable
  // game-image mapping that is not necessarily marked executable in /proc/maps.
  // Exact-build RVAs provide the code identity; requiring host X here would
  // reject the proven x86_64+ARM bridge path.
  if (method_mapping == nullptr || !method_mapping->readable ||
      method_mapping->writable)
    return false;
  *vptr_out = vptr;
  *method_out = method;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5 || std::strcmp(argv[4], kAcknowledgement) != 0) {
    std::fprintf(stderr,
                 "usage: %s PID LIB_BASE_HEX START_TICKS %s\n",
                 argv[0], kAcknowledgement);
    return 2;
  }
  std::uint64_t pid_value = 0, base_value = 0, expected_ticks = 0;
  if (!ParseUnsigned(argv[1], 10, &pid_value) ||
      !ParseUnsigned(argv[2], 16, &base_value) ||
      !ParseUnsigned(argv[3], 10, &expected_ticks) || pid_value == 0 ||
      pid_value > static_cast<std::uint64_t>(INT32_MAX) || base_value == 0 ||
      expected_ticks == 0)
    return 2;
  const pid_t pid = static_cast<pid_t>(pid_value);
  std::uint64_t observed_ticks = 0;
  if (!ReadProcessStartTicks(pid, &observed_ticks) ||
      observed_ticks != expected_ticks)
    return 3;
  std::vector<active::Mapping> mappings;
  std::uintptr_t game_end = 0;
  const auto game_base = static_cast<std::uintptr_t>(base_value);
  if (!ReadMappings(pid, game_base, &mappings, &game_end)) return 4;
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/mem", static_cast<int>(pid));
  ProcessMemory memory{};
  memory.fd = open(path, O_RDONLY | O_CLOEXEC);
  if (memory.fd < 0) return 5;
  hud::Resolution result{};
  const hud::Result status = hud::Resolve(
      {&memory, &ReadRemote}, mappings.data(), mappings.size(), game_base,
      game_end, &result);
  std::uintptr_t registered_vtable[kRegisteredVtableSlots]{};
  bool registered_vtable_read = false;
  std::vector<HudObjectCandidate> hud_objects;
  GameplayGuiVisibilityChain visibility_chain{};
  std::uintptr_t root_visibility_vptr = 0;
  std::uintptr_t root_hidden_getter = 0;
  if (status == hud::Result::kOk) {
    registered_vtable_read = ReadRemote(
        &memory, result.registered_screen_vptr, registered_vtable,
        sizeof(registered_vtable));
    (void)ReadVirtualMethod(&memory, mappings, game_base, game_end,
                          result.hud, kGameplayGuiRootHiddenSlot,
                            &root_visibility_vptr,
                          &root_hidden_getter);
    for (std::uintptr_t offset = sizeof(std::uintptr_t);
         offset + sizeof(std::uintptr_t) <= hud::kGameplayHudObjectSize;
         offset += alignof(std::uintptr_t)) {
      std::uintptr_t object = 0;
      if (!ReadRemote(&memory, result.hud + offset, &object, sizeof(object)) ||
          !active::AlignedPointer(object) ||
          !active::ObjectMapping(active::MappingAt(
              mappings.data(), mappings.size(), object,
              sizeof(std::uintptr_t))))
        continue;
      std::uintptr_t vptr = 0;
      std::uintptr_t method = 0;
      if (!ReadVirtualMethod(&memory, mappings, game_base, game_end, object,
                             kWidgetVisibilitySlot, &vptr, &method))
        continue;
      hud_objects.push_back({offset, object, vptr, method});
    }

    if (active::Add(result.hud, kGameplayGuiInterfaceOffset,
                    &visibility_chain.interface_object) &&
        ReadVirtualMethod(&memory, mappings, game_base, game_end,
                          visibility_chain.interface_object,
                          kGameplayGuiSetHudVisibleSlot,
                          &visibility_chain.interface_vptr,
                          &visibility_chain.set_hud_visible)) {
      visibility_chain.interface_valid = true;
      ReadRemote(&memory, result.hud + kGameplayGuiPrimaryNodeOffset,
                 &visibility_chain.primary_node,
                 sizeof(visibility_chain.primary_node));
      if (visibility_chain.primary_node != 0 &&
          !ReadVirtualMethod(&memory, mappings, game_base, game_end,
                             visibility_chain.primary_node,
                             kGameplayGuiPrimarySetVisibleSlot,
                             &visibility_chain.primary_vptr,
                             &visibility_chain.primary_set_visible)) {
        visibility_chain.interface_valid = false;
      }
      if (visibility_chain.interface_valid &&
          visibility_chain.primary_node != 0 &&
          !ReadVirtualMethod(&memory, mappings, game_base, game_end,
                             visibility_chain.primary_node,
                             kGameplayGuiPrimaryGetVisibleSlot,
                             &visibility_chain.primary_vptr,
                             &visibility_chain.primary_get_visible)) {
        visibility_chain.interface_valid = false;
      }
      ReadRemote(&memory, result.hud + kGameplayGuiSecondaryNodeOffset,
                 &visibility_chain.secondary_node,
                 sizeof(visibility_chain.secondary_node));
      if (visibility_chain.secondary_node != 0 &&
          !ReadVirtualMethod(&memory, mappings, game_base, game_end,
                             visibility_chain.secondary_node,
                             kGameplayGuiSecondarySetVisibleSlot,
                             &visibility_chain.secondary_vptr,
                             &visibility_chain.secondary_set_visible)) {
        visibility_chain.interface_valid = false;
      }
      if (visibility_chain.interface_valid &&
          visibility_chain.secondary_node != 0 &&
          !ReadVirtualMethod(&memory, mappings, game_base, game_end,
                             visibility_chain.secondary_node,
                             kGameplayGuiSecondaryGetVisibleSlot,
                             &visibility_chain.secondary_vptr,
                             &visibility_chain.secondary_get_visible)) {
        visibility_chain.interface_valid = false;
      }
    }
  }
  close(memory.fd);
  std::printf(
      "GAMEPLAY_HUD_GRAPH_V1 result=%d pid=%d start_ticks=%" PRIu64
      " mappings=%zu scanned=%" PRIu64 " candidates=%u hud=0x%" PRIxPTR
      " hud_vptr=0x%" PRIxPTR " hud_vptr_rva=0x%" PRIxPTR
      " hud_hidden_getter=0x%" PRIxPTR
      " owner=0x%" PRIxPTR
      " owner_vptr=0x%" PRIxPTR " registered=0x%" PRIxPTR
      " registered_vptr=0x%" PRIxPTR " writes=0 calls=0 ptrace=0\n",
      static_cast<int>(status), static_cast<int>(pid), observed_ticks,
      mappings.size(), result.bytes_scanned, result.structural_candidates,
      result.hud, result.hud_vptr, result.matched_hud_vptr_rva,
      root_hidden_getter,
      result.gui_owner, result.gui_owner_vptr,
      result.registered_screen, result.registered_screen_vptr);
  std::printf(
      "GAMEPLAY_HUD_INTERFACE_V1 within_hud=%u offset=0x%" PRIxPTR
      " object_size=0x%zx writes=0 calls=0 ptrace=0\n",
      result.registered_screen_within_hud ? 1u : 0u,
      result.registered_screen_offset, hud::kGameplayHudObjectSize);
  std::printf("GAMEPLAY_HUD_VTABLE_V1 read=%u slots=%zu", 
              registered_vtable_read ? 1u : 0u, kRegisteredVtableSlots);
  if (registered_vtable_read) {
    for (std::size_t slot = 0; slot < kRegisteredVtableSlots; ++slot) {
      std::printf(" s%02zu=0x%" PRIxPTR, slot, registered_vtable[slot]);
    }
  }
  std::printf(" writes=0 calls=0 ptrace=0\n");
  std::printf("GAMEPLAY_HUD_OBJECTS_V1 count=%zu depth=1", hud_objects.size());
  for (const HudObjectCandidate& candidate : hud_objects) {
    std::printf(" f0x%" PRIxPTR "=0x%" PRIxPTR ":v0x%" PRIxPTR
                ":m0x%" PRIxPTR,
                candidate.field_offset, candidate.object,
                candidate.vptr - game_base,
                candidate.visibility_method - game_base);
  }
  std::printf(" writes=0 calls=0 ptrace=0\n");
  std::printf(
      "GAMEPLAY_HUD_VISIBILITY_CHAIN_V1 valid=%u interface=0x%" PRIxPTR
      ":v0x%" PRIxPTR ":m0x%" PRIxPTR
      " primary=0x%" PRIxPTR ":v0x%" PRIxPTR ":m0x%" PRIxPTR
      ":g0x%" PRIxPTR
      " secondary=0x%" PRIxPTR ":v0x%" PRIxPTR ":m0x%" PRIxPTR
      ":g0x%" PRIxPTR
      " writes=0 calls=0 ptrace=0\n",
      visibility_chain.interface_valid ? 1u : 0u,
      visibility_chain.interface_object,
      visibility_chain.interface_vptr == 0
          ? 0
          : visibility_chain.interface_vptr - game_base,
      visibility_chain.set_hud_visible == 0
          ? 0
          : visibility_chain.set_hud_visible - game_base,
      visibility_chain.primary_node,
      visibility_chain.primary_vptr == 0
          ? 0
          : visibility_chain.primary_vptr - game_base,
      visibility_chain.primary_set_visible == 0
          ? 0
          : visibility_chain.primary_set_visible - game_base,
      visibility_chain.primary_get_visible == 0
          ? 0
          : visibility_chain.primary_get_visible - game_base,
      visibility_chain.secondary_node,
      visibility_chain.secondary_vptr == 0
          ? 0
          : visibility_chain.secondary_vptr - game_base,
      visibility_chain.secondary_set_visible == 0
          ? 0
          : visibility_chain.secondary_set_visible - game_base,
      visibility_chain.secondary_get_visible == 0
          ? 0
          : visibility_chain.secondary_get_visible - game_base);
  if (status == hud::Result::kOk && !registered_vtable_read) return 7;
  return status == hud::Result::kOk ? 0 : 6;
}
