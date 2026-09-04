#pragma once

// Read-only resolver for the exact hash-pinned recording payload.  Symbol
// RVAs belong to the pinned ELF identity. LDPlayer NativeBridge leaves guest
// exported RELATIVE pointer data zero in the host-visible view, so the exact
// hash-pinned storage RVAs are used and range-checked against mapped BSS.

#include "fc1_payload_elf_resolver_v1.h"
#include "natural_action_recording_protocol_v1.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace a9tas::natural_action_recording_elf_v1 {

inline constexpr char kPayloadBasename[] =
    "liba9tas_natural_action_recording_v1_review_only.so";
inline constexpr std::uint8_t kExpectedSha256[32] = {
    0xad,0x3a,0xe0,0x35,0x43,0xb7,0x48,0x91,
    0x02,0xad,0xb5,0x3d,0xba,0x32,0x02,0x96,
    0xe9,0x3b,0x03,0x4b,0x12,0xfe,0x18,0xbc,
    0xf2,0x16,0x96,0x9b,0xc1,0x23,0xc5,0x27,
};
inline constexpr std::uintptr_t kWrapperRva = 0x1b50;
inline constexpr std::uintptr_t kControlLocatorRva = 0x4040;
inline constexpr std::uintptr_t kEvidenceLocatorRva = 0x4048;
inline constexpr std::uintptr_t kCountsLocatorRva = 0x4050;
inline constexpr std::uintptr_t kShadowLocatorRva = 0x4058;
inline constexpr std::uintptr_t kControlSizeRva = 0x4060;
inline constexpr std::uintptr_t kEvidenceSizeRva = 0x4064;
inline constexpr std::uintptr_t kCountsSizeRva = 0x4068;
inline constexpr std::uintptr_t kShadowSizeRva = 0x406c;
inline constexpr std::uintptr_t kControlStorageRva = 0x3fc0;
inline constexpr std::uintptr_t kEvidenceStorageRva = 0x3f40;
inline constexpr std::uintptr_t kCountsStorageRva = 0x4080;
inline constexpr std::uintptr_t kShadowStorageRva = 0x78c0;

struct Mapping {
  std::uintptr_t begin{};
  std::uintptr_t end{};
  std::uint64_t offset{};
  char perms[5]{};
  std::string path;
};

struct Layout {
  std::uintptr_t load_bias{};
  std::uintptr_t wrapper{};
  std::uintptr_t control{};
  std::uintptr_t evidence{};
  std::uintptr_t counts{};
  std::uintptr_t shadow{};
  std::uint32_t control_size{};
  std::uint32_t evidence_size{};
  std::uint32_t counts_size{};
  std::uint32_t shadow_size{};
  std::uint8_t file_sha256[32]{};
  char mapped_path[1024]{};
};

namespace detail {

using a9tas::fc1_payload_elf_v1::detail::HashFile;
using a9tas::fc1_payload_elf_v1::detail::ReadAt;

inline bool EndsWith(const std::string& path) {
  const std::size_t size = sizeof(kPayloadBasename) - 1;
  if (path.size() < size) return false;
  const std::size_t offset = path.size() - size;
  return path.compare(offset, size, kPayloadBasename) == 0 &&
         (offset == 0 || path[offset - 1] == '/');
}

inline bool ReadMaps(pid_t pid, std::vector<Mapping>* maps,
                     std::string* path_out, std::uintptr_t* bias_out) {
  char file_path[64]{};
  std::snprintf(file_path, sizeof(file_path), "/proc/%d/maps",
                static_cast<int>(pid));
  FILE* file = std::fopen(file_path, "re");
  if (!file) return false;
  std::vector<Mapping> result;
  char line[2048]{};
  while (std::fgets(line, sizeof(line), file)) {
    unsigned long long begin = 0, end = 0, offset = 0;
    char perms[5]{}, path[1024]{};
    const int fields = std::sscanf(
        line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]",
        &begin, &end, perms, &offset, path);
    if (fields < 4 || begin >= end) continue;
    std::string clean = fields == 5 ? std::string(path) : std::string();
    while (!clean.empty() && clean.front() == ' ') clean.erase(0, 1);
    Mapping mapping{static_cast<std::uintptr_t>(begin),
                    static_cast<std::uintptr_t>(end), offset, {}, clean};
    std::memcpy(mapping.perms, perms, 4);
    result.push_back(std::move(mapping));
  }
  std::fclose(file);
  std::string selected;
  std::uintptr_t bias = UINTPTR_MAX;
  std::uint32_t segments = 0;
  bool writable = false;
  for (const auto& mapping : result) {
    if (!EndsWith(mapping.path)) continue;
    if (mapping.path.find(" (deleted)") != std::string::npos) return false;
    if (selected.empty()) selected = mapping.path;
    if (selected != mapping.path) return false;
    if (mapping.offset == 0) bias = std::min(bias, mapping.begin);
    writable = writable || mapping.perms[1] == 'w';
    ++segments;
  }
  if (selected.empty() || selected.size() >= 1024 || bias == UINTPTR_MAX ||
      segments < 2 || !writable)
    return false;
  *maps = std::move(result);
  *path_out = selected;
  *bias_out = bias;
  return true;
}

inline const Mapping* At(const std::vector<Mapping>& maps,
                         std::uintptr_t address, std::size_t size) {
  if (size == 0 || address > UINTPTR_MAX - size) return nullptr;
  for (const auto& mapping : maps)
    if (address >= mapping.begin && address + size <= mapping.end)
      return &mapping;
  return nullptr;
}

inline bool Writable(const std::vector<Mapping>& maps,
                     std::uintptr_t address, std::size_t size) {
  std::uintptr_t cursor = address;
  if (size == 0 || address > UINTPTR_MAX - size) return false;
  const std::uintptr_t end = address + size;
  while (cursor < end) {
    const Mapping* mapping = At(maps, cursor, 1);
    if (!mapping || mapping->perms[0] != 'r' || mapping->perms[1] != 'w')
      return false;
    cursor = std::min(end, mapping->end);
  }
  return true;
}

}  // namespace detail

inline bool Resolve(pid_t pid, int mem, Layout* output,
                    const char** failure_reason = nullptr) {
  using namespace a9tas::natural_action_recording_v1;
  if (failure_reason) *failure_reason = "invalid_arguments";
  const auto fail = [&](const char* reason) {
    if (failure_reason) *failure_reason = reason;
    return false;
  };
  if (pid <= 0 || mem < 0 || output == nullptr)
    return fail("invalid_arguments");
  std::vector<Mapping> maps;
  std::string path;
  std::uintptr_t bias = 0;
  if (!detail::ReadMaps(pid, &maps, &path, &bias))
    return fail("payload_maps");
  const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (file < 0) return fail("payload_open");
  struct stat info {};
  std::uint8_t hash[32]{};
  const bool identity = fstat(file, &info) == 0 && info.st_size >= 4096 &&
      info.st_size <= 8 * 1024 * 1024 && detail::HashFile(file, hash) &&
      std::memcmp(hash, kExpectedSha256, sizeof(hash)) == 0;
  close(file);
  if (!identity) return fail("payload_sha256");

  Layout layout{};
  layout.load_bias = bias;
  layout.wrapper = bias + kWrapperRva;
  layout.control = bias + kControlStorageRva;
  layout.evidence = bias + kEvidenceStorageRva;
  layout.counts = bias + kCountsStorageRva;
  layout.shadow = bias + kShadowStorageRva;
  const std::uintptr_t locators[] = {
      bias + kControlLocatorRva, bias + kEvidenceLocatorRva,
      bias + kCountsLocatorRva, bias + kShadowLocatorRva,
  };
  const std::uintptr_t sizes[] = {
      bias + kControlSizeRva, bias + kEvidenceSizeRva,
      bias + kCountsSizeRva, bias + kShadowSizeRva,
  };
  const Mapping* wrapper_map = detail::At(maps, layout.wrapper, 24);
  if (!wrapper_map || wrapper_map->path != path ||
      wrapper_map->perms[0] != 'r')
    return fail("wrapper_mapping");
  // LDPlayer NativeBridge guest AArch64 code is translated from a host r--p
  // file mapping, so /proc/PID/maps does not expose an x bit for guest text.
  // Executability is instead pinned by the exact payload SHA and the reviewed
  // AArch64 PT_LOAD(PF_X) identity for kWrapperRva.
  for (std::size_t index = 0; index < 4; ++index) {
    const Mapping* locator_map = detail::At(maps, locators[index], 8);
    const Mapping* size_map = detail::At(maps, sizes[index], 4);
    if (!locator_map || !size_map || locator_map->path != path ||
        size_map->path != path || locator_map->perms[0] != 'r' ||
        size_map->perms[0] != 'r')
      return fail("locator_mapping");
  }
  std::uintptr_t exported_pointers[4]{};
  if (!detail::ReadAt(mem, locators[0], &exported_pointers[0], 8) ||
      !detail::ReadAt(mem, locators[1], &exported_pointers[1], 8) ||
      !detail::ReadAt(mem, locators[2], &exported_pointers[2], 8) ||
      !detail::ReadAt(mem, locators[3], &exported_pointers[3], 8) ||
      !detail::ReadAt(mem, sizes[0], &layout.control_size, 4) ||
      !detail::ReadAt(mem, sizes[1], &layout.evidence_size, 4) ||
      !detail::ReadAt(mem, sizes[2], &layout.counts_size, 4) ||
      !detail::ReadAt(mem, sizes[3], &layout.shadow_size, 4))
    return fail("relocated_storage_read");
  const std::uintptr_t fixed_storage[4] = {
      layout.control, layout.evidence, layout.counts, layout.shadow,
  };
  for (std::size_t index = 0; index < 4; ++index)
    if (exported_pointers[index] != 0 &&
        exported_pointers[index] != fixed_storage[index])
      return fail("exported_storage_pointer_identity");
  if (layout.control_size != sizeof(Control) ||
      layout.evidence_size != sizeof(Evidence) ||
      layout.counts_size != sizeof(std::uint32_t) * kMaximumFrames ||
      layout.shadow_size != kShadowVtableSize)
    return fail("relocated_storage_sizes");
  if ((layout.wrapper & 3u) != 0 || (layout.control & 63u) != 0 ||
      (layout.evidence & 63u) != 0 || (layout.counts & 63u) != 0 ||
      (layout.shadow & 63u) != 0)
    return fail("relocated_storage_alignment");
  if (!detail::Writable(maps, layout.control, sizeof(Control)))
    return fail("control_not_writable");
  if (!detail::Writable(maps, layout.evidence, sizeof(Evidence)))
    return fail("evidence_not_writable");
  if (!detail::Writable(maps, layout.counts, layout.counts_size))
    return fail("counts_not_writable");
  if (!detail::Writable(maps, layout.shadow, layout.shadow_size))
    return fail("shadow_not_writable");
  std::memcpy(layout.file_sha256, hash, sizeof(hash));
  std::memcpy(layout.mapped_path, path.c_str(), path.size() + 1);
  *output = layout;
  if (failure_reason) *failure_reason = "none";
  return true;
}

}  // namespace a9tas::natural_action_recording_elf_v1
