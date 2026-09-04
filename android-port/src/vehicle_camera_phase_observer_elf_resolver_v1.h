#pragma once

// Read-only resolver for the exact hash-pinned combined phase-observer ELF.
// The artifact embeds the immutable final-writer payload plus two observer
// wrappers.  No guest function is called and no process memory is written.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "final_writer_replay_elf_resolver_v1.h"
#include "vehicle_camera_phase_observer_protocol_v1.h"

namespace a9tas::vehicle_camera_phase_observer_elf_v1 {

namespace base_elf = final_writer_replay_elf_v1;
namespace final_writer = final_writer_replay_v1;
namespace phase = vehicle_camera_phase_observer_v1;

inline constexpr char kPayloadBasename[] =
    "liba9tas_vehicle_camera_phase_observer_v1_build_only.so";
inline constexpr std::uint8_t kExpectedSha256[32] = {
    0xc4, 0xf6, 0x23, 0x83, 0xdf, 0x62, 0x55, 0x29,
    0x94, 0x50, 0xfb, 0x8e, 0x46, 0x9f, 0x7d, 0xae,
    0x96, 0x5b, 0x31, 0x0f, 0x51, 0x2b, 0x1b, 0xf5,
    0x71, 0x51, 0xf5, 0x1c, 0xbd, 0xcb, 0x93, 0x81,
};

// Exact RVAs are permitted here because the whole file is SHA-256 pinned.
inline constexpr std::uintptr_t kVehicleWrapperRva = 0x2DB8;
inline constexpr std::uintptr_t kCameraWrapperRva = 0x32B4;
inline constexpr std::uintptr_t kShadowLocatorRva = 0x6600;
inline constexpr std::uintptr_t kControlLocatorRva = 0x6608;
inline constexpr std::uintptr_t kTargetLocatorRva = 0x6610;
inline constexpr std::uintptr_t kAuditLocatorRva = 0x6618;
inline constexpr std::uintptr_t kEvidenceLocatorRva = 0x6620;
inline constexpr std::uintptr_t kControlSizeRva = 0x6628;
inline constexpr std::uintptr_t kTargetSizeRva = 0x6630;
inline constexpr std::uintptr_t kAuditSizeRva = 0x6638;
inline constexpr std::uintptr_t kEvidenceSizeRva = 0x6640;
inline constexpr std::uintptr_t kPhaseControlLocatorRva = 0x7040;
inline constexpr std::uintptr_t kPhaseEvidenceLocatorRva = 0x7140;
inline constexpr std::uintptr_t kPhaseEventsLocatorRva = 0x7148;

inline constexpr std::uintptr_t kExpectedShadowStorageRva = 0x6680;
inline constexpr std::uintptr_t kExpectedControlStorageRva = 0x64C0;
inline constexpr std::uintptr_t kExpectedEvidenceStorageRva = 0x6540;
inline constexpr std::uintptr_t kExpectedAuditStorageRva = 0x7180;
inline constexpr std::uintptr_t kExpectedTargetStorageRva = 0x93B80;
inline constexpr std::uintptr_t kExpectedPhaseControlStorageRva = 0x6FC0;
inline constexpr std::uintptr_t kExpectedPhaseEvidenceStorageRva = 0x7080;
inline constexpr std::uintptr_t kExpectedPhaseEventsStorageRva = 0xDA0C0;

struct Layout : base_elf::Layout {
  std::uintptr_t camera_wrapper{};
  std::uintptr_t phase_control{};
  std::uintptr_t phase_evidence{};
  std::uintptr_t phase_events{};
};

namespace detail {

using base_elf::detail::HashFile;
using base_elf::detail::ReadAt;

inline bool EndsWithBasename(const std::string& path) {
  if (path.size() < sizeof(kPayloadBasename) - 1) return false;
  const std::size_t offset = path.size() - (sizeof(kPayloadBasename) - 1);
  return path.compare(offset, sizeof(kPayloadBasename) - 1,
                      kPayloadBasename) == 0 &&
         (offset == 0 || path[offset - 1] == '/');
}

inline bool ReadMappings(pid_t pid, std::vector<base_elf::Mapping>* all_out,
                         std::string* path_out,
                         std::uintptr_t* load_bias_out) {
  if (pid <= 0 || all_out == nullptr || path_out == nullptr ||
      load_bias_out == nullptr)
    return false;
  char maps_path[64]{};
  std::snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps",
                static_cast<int>(pid));
  FILE* file = std::fopen(maps_path, "re");
  if (file == nullptr) return false;
  std::vector<base_elf::Mapping> all;
  char line[2048]{};
  while (std::fgets(line, sizeof(line), file)) {
    unsigned long long begin = 0, end = 0, offset = 0;
    char permissions[5]{}, raw_path[1024]{};
    const int fields = std::sscanf(
        line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &begin, &end,
        permissions, &offset, raw_path);
    if (fields < 4 || begin >= end) continue;
    std::string path = fields == 5 ? std::string(raw_path) : std::string();
    while (!path.empty() && path.front() == ' ') path.erase(0, 1);
    base_elf::Mapping mapping{static_cast<std::uintptr_t>(begin),
                              static_cast<std::uintptr_t>(end), offset, {},
                              std::move(path)};
    std::memcpy(mapping.perms, permissions, 4);
    all.push_back(std::move(mapping));
  }
  std::fclose(file);

  std::string selected;
  std::uintptr_t load_bias = UINTPTR_MAX;
  std::uint32_t selected_mappings = 0;
  bool readable = false, writable = false;
  for (const auto& mapping : all) {
    if (!EndsWithBasename(mapping.path)) continue;
    if (mapping.path.find(" (deleted)") != std::string::npos) return false;
    if (selected.empty()) selected = mapping.path;
    if (mapping.path != selected) return false;
    if (mapping.file_offset == 0)
      load_bias = std::min(load_bias, mapping.begin);
    readable = readable || mapping.perms[0] == 'r';
    writable = writable || mapping.perms[1] == 'w';
    ++selected_mappings;
  }
  if (selected.empty() || selected.size() >= 1024 ||
      selected_mappings < 2 || load_bias == UINTPTR_MAX || !readable ||
      !writable)
    return false;
  *all_out = std::move(all);
  *path_out = std::move(selected);
  *load_bias_out = load_bias;
  return true;
}

inline bool ReadPointer(int mem, std::uintptr_t load_bias,
                        std::uintptr_t locator_rva,
                        std::uintptr_t expected_storage_rva,
                        std::uintptr_t* output) {
  std::uintptr_t value = 0;
  return load_bias <= UINTPTR_MAX - locator_rva &&
         load_bias <= UINTPTR_MAX - expected_storage_rva &&
         base_elf::detail::ReadAt(mem, load_bias + locator_rva, &value,
                                  sizeof(value)) &&
         value == load_bias + expected_storage_rva &&
         ((*output = value), true);
}

}  // namespace detail

inline bool Resolve(pid_t pid, int process_mem, Layout* output) {
  if (pid <= 0 || process_mem < 0 || output == nullptr) return false;
  std::vector<base_elf::Mapping> mappings;
  std::string path;
  std::uintptr_t load_bias = 0;
  if (!detail::ReadMappings(pid, &mappings, &path, &load_bias)) return false;
  const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (file < 0) return false;
  struct stat info {};
  std::uint8_t hash[32]{};
  const bool file_ok = fstat(file, &info) == 0 && info.st_size >= 4096 &&
                       info.st_size <= 8 * 1024 * 1024 &&
                       detail::HashFile(file, hash) &&
                       std::memcmp(hash, kExpectedSha256, sizeof(hash)) == 0;
  close(file);
  if (!file_ok || load_bias > UINTPTR_MAX - kCameraWrapperRva) return false;

  Layout layout{};
  layout.load_bias = load_bias;
  layout.wrapper = load_bias + kVehicleWrapperRva;
  layout.camera_wrapper = load_bias + kCameraWrapperRva;
  if (!detail::ReadPointer(process_mem, load_bias, kShadowLocatorRva,
                           kExpectedShadowStorageRva, &layout.shadow) ||
      !detail::ReadPointer(process_mem, load_bias, kControlLocatorRva,
                           kExpectedControlStorageRva, &layout.control) ||
      !detail::ReadPointer(process_mem, load_bias, kTargetLocatorRva,
                           kExpectedTargetStorageRva, &layout.targets) ||
      !detail::ReadPointer(process_mem, load_bias, kAuditLocatorRva,
                           kExpectedAuditStorageRva, &layout.audits) ||
      !detail::ReadPointer(process_mem, load_bias, kEvidenceLocatorRva,
                           kExpectedEvidenceStorageRva, &layout.evidence) ||
      !detail::ReadPointer(process_mem, load_bias, kPhaseControlLocatorRva,
                           kExpectedPhaseControlStorageRva,
                           &layout.phase_control) ||
      !detail::ReadPointer(process_mem, load_bias, kPhaseEvidenceLocatorRva,
                           kExpectedPhaseEvidenceStorageRva,
                           &layout.phase_evidence) ||
      !detail::ReadPointer(process_mem, load_bias, kPhaseEventsLocatorRva,
                           kExpectedPhaseEventsStorageRva,
                           &layout.phase_events) ||
      !base_elf::detail::ReadAt(process_mem, load_bias + kControlSizeRva,
                                &layout.control_size, 8) ||
      !base_elf::detail::ReadAt(process_mem, load_bias + kTargetSizeRva,
                                &layout.target_size, 8) ||
      !base_elf::detail::ReadAt(process_mem, load_bias + kAuditSizeRva,
                                &layout.audit_size, 8) ||
      !base_elf::detail::ReadAt(process_mem, load_bias + kEvidenceSizeRva,
                                &layout.evidence_size, 8))
    return false;

  if (layout.control_size != sizeof(final_writer::Control) ||
      layout.target_size != sizeof(final_writer::FrameTarget) ||
      layout.audit_size != sizeof(final_writer::FrameAudit) ||
      layout.evidence_size != sizeof(final_writer::Evidence) ||
      (layout.wrapper & 3u) != 0 || (layout.camera_wrapper & 3u) != 0 ||
      (layout.shadow & 63u) != 0 || (layout.control & 63u) != 0 ||
      (layout.targets & 63u) != 0 || (layout.audits & 63u) != 0 ||
      (layout.evidence & 63u) != 0 || (layout.phase_control & 63u) != 0 ||
      (layout.phase_evidence & 63u) != 0 || (layout.phase_events & 63u) != 0)
    return false;

  const auto* vehicle_map =
      base_elf::detail::MappingAt(mappings, layout.wrapper, 24);
  const auto* camera_map =
      base_elf::detail::MappingAt(mappings, layout.camera_wrapper, 24);
  if (vehicle_map == nullptr || camera_map == nullptr ||
      vehicle_map->path != path || camera_map->path != path ||
      vehicle_map->perms[0] != 'r' || vehicle_map->perms[1] == 'w' ||
      camera_map->perms[0] != 'r' || camera_map->perms[1] == 'w' ||
      !base_elf::detail::WritableRange(
          mappings, layout.shadow, final_writer::kPrimaryShadowSize) ||
      !base_elf::detail::WritableRange(
          mappings, layout.control, sizeof(final_writer::Control)) ||
      !base_elf::detail::WritableRange(
          mappings, layout.targets,
          sizeof(final_writer::FrameTarget) * final_writer::kMaximumFrames) ||
      !base_elf::detail::WritableRange(
          mappings, layout.audits,
          sizeof(final_writer::FrameAudit) * final_writer::kMaximumFrames) ||
      !base_elf::detail::WritableRange(
          mappings, layout.evidence, sizeof(final_writer::Evidence)) ||
      !base_elf::detail::WritableRange(
          mappings, layout.phase_control, sizeof(phase::Control)) ||
      !base_elf::detail::WritableRange(
          mappings, layout.phase_evidence, sizeof(phase::Evidence)) ||
      !base_elf::detail::WritableRange(
          mappings, layout.phase_events,
          sizeof(phase::Event) * phase::kMaximumEvents))
    return false;

  std::memcpy(layout.file_sha256, hash, sizeof(hash));
  std::memcpy(layout.mapped_path, path.c_str(), path.size() + 1);
  *output = layout;
  return true;
}

}  // namespace a9tas::vehicle_camera_phase_observer_elf_v1
