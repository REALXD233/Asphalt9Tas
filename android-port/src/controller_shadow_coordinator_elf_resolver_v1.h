#pragma once

// Read-only resolver for the hash-pinned controller-shadow coordinator.
// It accepts one exact mapped payload, proves its ELF64/AArch64 layout and
// dereferences payload-owned locator exports through /proc/PID/mem.  It never
// invokes guest code and never writes process memory.

#include "controller_shadow_coordinator_protocol_v1.h"
#include "fc1_payload_elf_resolver_v1.h"

#include <elf.h>
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

namespace a9tas::controller_shadow_coordinator_elf_v1 {

namespace protocol = a9tas::controller_shadow_coordinator_v1;

inline constexpr char kPayloadBasename[] =
    "liba9tas_controller_shadow_coordinator_v1_build_only.so";
inline constexpr std::uint8_t kExpectedSha256[32] = {
    0x63, 0x6d, 0x07, 0x59, 0xcb, 0x8c, 0xcd, 0x48,
    0x30, 0xc7, 0xde, 0x2b, 0x82, 0xb6, 0xa4, 0x5b,
    0x14, 0x8f, 0x98, 0xce, 0x46, 0x56, 0xad, 0xa5,
    0x30, 0x45, 0xe5, 0x4d, 0xf3, 0xc2, 0xf7, 0xde,
};

struct Layout {
  std::uintptr_t load_bias{};
  std::uintptr_t wrapper{};
  std::uintptr_t shadow{};
  std::uintptr_t control{};
  std::uintptr_t evidence{};
  std::uintptr_t frames{};
  std::uint64_t shadow_size{};
  std::uint64_t control_size{};
  std::uint64_t evidence_size{};
  std::uint64_t frame_size{};
  std::uint64_t frame_capacity{};
  std::uint64_t prefix_size{};
  std::uint64_t update_slot{};
  std::uint8_t file_sha256[32]{};
  char mapped_path[1024]{};
};

struct Mapping {
  std::uintptr_t begin{};
  std::uintptr_t end{};
  std::uint64_t file_offset{};
  char perms[5]{};
  std::string path;
};

namespace detail {

using a9tas::fc1_payload_elf_v1::detail::HashFile;
using a9tas::fc1_payload_elf_v1::detail::PageDown;
using a9tas::fc1_payload_elf_v1::detail::ReadAt;

inline bool EndsWithBasename(const std::string& path) {
  constexpr std::size_t length = sizeof(kPayloadBasename) - 1;
  if (path.size() < length) return false;
  const std::size_t offset = path.size() - length;
  return path.compare(offset, length, kPayloadBasename) == 0 &&
         (offset == 0 || path[offset - 1] == '/');
}

inline bool ReadMappings(pid_t pid, std::vector<Mapping>* all_out,
                         std::vector<Mapping>* payload_out,
                         std::string* path_out,
                         std::uintptr_t* load_bias_out) {
  if (all_out == nullptr || payload_out == nullptr || path_out == nullptr ||
      load_bias_out == nullptr)
    return false;
  char maps_path[64]{};
  std::snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps",
                static_cast<int>(pid));
  FILE* file = std::fopen(maps_path, "re");
  if (file == nullptr) return false;
  std::vector<Mapping> all;
  char line[2048]{};
  while (std::fgets(line, sizeof(line), file)) {
    unsigned long long begin = 0, end = 0, offset = 0;
    char perms[5]{}, path[1024]{};
    const int fields = std::sscanf(
        line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &begin, &end,
        perms, &offset, path);
    if (fields < 4 || begin >= end) continue;
    std::string clean = fields == 5 ? std::string(path) : std::string();
    while (!clean.empty() && clean.front() == ' ') clean.erase(0, 1);
    Mapping mapping{static_cast<std::uintptr_t>(begin),
                    static_cast<std::uintptr_t>(end), offset, {}, clean};
    std::memcpy(mapping.perms, perms, 4);
    all.push_back(std::move(mapping));
  }
  std::fclose(file);

  std::string selected;
  for (const auto& mapping : all) {
    if (!EndsWithBasename(mapping.path)) continue;
    if (mapping.path.find(" (deleted)") != std::string::npos) return false;
    if (selected.empty()) selected = mapping.path;
    if (selected != mapping.path) return false;
  }
  if (selected.empty() || selected.size() >= 1024) return false;

  std::vector<Mapping> payload_maps;
  std::uintptr_t bias = UINTPTR_MAX;
  std::uint32_t zero_offset_mappings = 0;
  bool readable = false;
  bool writable = false;
  for (const auto& mapping : all) {
    if (mapping.path != selected) continue;
    payload_maps.push_back(mapping);
    if (mapping.file_offset == 0) {
      bias = std::min(bias, mapping.begin);
      ++zero_offset_mappings;
    }
    readable = readable || mapping.perms[0] == 'r';
    writable = writable || mapping.perms[1] == 'w';
  }
  if (payload_maps.size() < 2 || bias == UINTPTR_MAX ||
      zero_offset_mappings != 1 || !readable || !writable)
    return false;
  *all_out = std::move(all);
  *payload_out = std::move(payload_maps);
  *path_out = selected;
  *load_bias_out = bias;
  return true;
}

inline const Mapping* MappingAt(const std::vector<Mapping>& mappings,
                                std::uintptr_t address,
                                std::size_t size) {
  if (size == 0 || address > UINTPTR_MAX - size) return nullptr;
  const std::uintptr_t end = address + size;
  for (const auto& mapping : mappings)
    if (address >= mapping.begin && end <= mapping.end) return &mapping;
  return nullptr;
}

inline bool WritableRange(const std::vector<Mapping>& mappings,
                          std::uintptr_t address, std::size_t size) {
  if (size == 0 || address > UINTPTR_MAX - size) return false;
  const std::uintptr_t end = address + size;
  std::uintptr_t cursor = address;
  while (cursor < end) {
    const Mapping* covering = nullptr;
    for (const auto& mapping : mappings)
      if (cursor >= mapping.begin && cursor < mapping.end) {
        covering = &mapping;
        break;
      }
    if (covering == nullptr || covering->perms[0] != 'r' ||
        covering->perms[1] != 'w')
      return false;
    cursor = std::min(end, covering->end);
  }
  return true;
}

struct Symbols {
  std::uintptr_t wrapper{};
  std::uintptr_t shadow_data{};
  std::uintptr_t control_data{};
  std::uintptr_t evidence_data{};
  std::uintptr_t frames_data{};
  std::uintptr_t shadow_size_data{};
  std::uintptr_t control_size_data{};
  std::uintptr_t evidence_size_data{};
  std::uintptr_t frame_size_data{};
  std::uintptr_t frame_capacity_data{};
  std::uintptr_t prefix_size_data{};
  std::uintptr_t update_slot_data{};
};

inline bool ResolveSymbols(int fd, std::uint64_t file_size,
                           const std::vector<Mapping>& payload_maps,
                           const std::string& mapped_path,
                           std::uintptr_t load_bias, Symbols* output) {
  if (output == nullptr) return false;
  Elf64_Ehdr header{};
  if (!ReadAt(fd, 0, &header, sizeof(header)) ||
      std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_type != ET_DYN ||
      header.e_machine != EM_AARCH64 ||
      header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 ||
      header.e_phnum > 64 || header.e_phoff > file_size ||
      static_cast<std::uint64_t>(header.e_phnum) * sizeof(Elf64_Phdr) >
          file_size - header.e_phoff ||
      header.e_shentsize != sizeof(Elf64_Shdr) || header.e_shnum == 0 ||
      header.e_shnum > 256 || header.e_shoff > file_size ||
      static_cast<std::uint64_t>(header.e_shnum) * sizeof(Elf64_Shdr) >
          file_size - header.e_shoff)
    return false;

  std::vector<Elf64_Phdr> programs(header.e_phnum);
  if (!ReadAt(fd, header.e_phoff, programs.data(),
              programs.size() * sizeof(programs.front())))
    return false;
  std::uint32_t load_count = 0;
  for (const auto& program : programs) {
    if (program.p_type != PT_LOAD) continue;
    if (program.p_filesz > program.p_memsz || program.p_align != 0x1000 ||
        program.p_offset > file_size ||
        program.p_filesz > file_size - program.p_offset ||
        program.p_vaddr > UINTPTR_MAX ||
        (program.p_offset & 0xFFFu) != (program.p_vaddr & 0xFFFu))
      return false;
    const std::uint64_t page_offset = PageDown(program.p_offset);
    const std::uint64_t page_vaddr = PageDown(program.p_vaddr);
    if (page_vaddr > UINTPTR_MAX - load_bias) return false;
    const std::uintptr_t expected_begin = load_bias + page_vaddr;
    bool exact = false;
    for (const auto& mapping : payload_maps)
      if (mapping.begin == expected_begin &&
          mapping.file_offset == page_offset && mapping.path == mapped_path) {
        exact = true;
        break;
      }
    if (!exact) return false;
    ++load_count;
  }
  if (load_count < 2) return false;

  std::vector<Elf64_Shdr> sections(header.e_shnum);
  if (!ReadAt(fd, header.e_shoff, sections.data(),
              sections.size() * sizeof(sections.front())))
    return false;
  const Elf64_Shdr* dynamic_symbols = nullptr;
  for (const auto& section : sections) {
    if (section.sh_type != SHT_DYNSYM) continue;
    if (dynamic_symbols != nullptr) return false;
    dynamic_symbols = &section;
  }
  if (dynamic_symbols == nullptr ||
      dynamic_symbols->sh_entsize != sizeof(Elf64_Sym) ||
      dynamic_symbols->sh_link >= sections.size() ||
      dynamic_symbols->sh_size == 0 ||
      dynamic_symbols->sh_size % sizeof(Elf64_Sym) != 0 ||
      dynamic_symbols->sh_size > 1024 * 1024 ||
      dynamic_symbols->sh_offset > file_size ||
      dynamic_symbols->sh_size > file_size - dynamic_symbols->sh_offset)
    return false;
  const Elf64_Shdr& strings_header = sections[dynamic_symbols->sh_link];
  if (strings_header.sh_type != SHT_STRTAB || strings_header.sh_size == 0 ||
      strings_header.sh_size > 1024 * 1024 ||
      strings_header.sh_offset > file_size ||
      strings_header.sh_size > file_size - strings_header.sh_offset)
    return false;
  std::vector<Elf64_Sym> symbols(dynamic_symbols->sh_size /
                                 sizeof(Elf64_Sym));
  std::vector<char> strings(strings_header.sh_size);
  if (!ReadAt(fd, dynamic_symbols->sh_offset, symbols.data(),
              dynamic_symbols->sh_size) ||
      !ReadAt(fd, strings_header.sh_offset, strings.data(), strings.size()))
    return false;

  struct Wanted {
    const char* name;
    unsigned char type;
    std::uintptr_t* address;
    std::uint32_t matches;
  } wanted[] = {
      {"a9tas_controller_tick_wrapper_v1", STT_FUNC, &output->wrapper, 0},
      {"a9tas_controller_shadow_storage_data_v1", STT_OBJECT,
       &output->shadow_data, 0},
      {"a9tas_controller_shadow_control_data_v1", STT_OBJECT,
       &output->control_data, 0},
      {"a9tas_controller_shadow_evidence_data_v1", STT_OBJECT,
       &output->evidence_data, 0},
      {"a9tas_controller_shadow_frames_data_v1", STT_OBJECT,
       &output->frames_data, 0},
      {"a9tas_controller_shadow_storage_size_v1", STT_OBJECT,
       &output->shadow_size_data, 0},
      {"a9tas_controller_shadow_control_size_v1", STT_OBJECT,
       &output->control_size_data, 0},
      {"a9tas_controller_shadow_evidence_size_v1", STT_OBJECT,
       &output->evidence_size_data, 0},
      {"a9tas_controller_shadow_frame_size_v1", STT_OBJECT,
       &output->frame_size_data, 0},
      {"a9tas_controller_shadow_frame_capacity_v1", STT_OBJECT,
       &output->frame_capacity_data, 0},
      {"a9tas_controller_shadow_prefix_size_v1", STT_OBJECT,
       &output->prefix_size_data, 0},
      {"a9tas_controller_shadow_update_slot_v1", STT_OBJECT,
       &output->update_slot_data, 0},
  };
  for (const auto& symbol : symbols) {
    if (symbol.st_name >= strings.size() || symbol.st_shndx == SHN_UNDEF ||
        ELF64_ST_BIND(symbol.st_info) != STB_GLOBAL)
      continue;
    const char* name = strings.data() + symbol.st_name;
    if (std::memchr(name, '\0', strings.size() - symbol.st_name) == nullptr)
      return false;
    for (auto& item : wanted) {
      if (std::strcmp(name, item.name) != 0) continue;
      if (ELF64_ST_TYPE(symbol.st_info) != item.type || symbol.st_value == 0 ||
          (item.type == STT_OBJECT && symbol.st_size != 8) ||
          (item.type == STT_FUNC && symbol.st_size < 24) ||
          symbol.st_value > UINTPTR_MAX - load_bias)
        return false;
      *item.address = load_bias + symbol.st_value;
      ++item.matches;
    }
  }
  for (const auto& item : wanted)
    if (item.matches != 1 || *item.address == 0) return false;

  const auto in_file_load = [&](std::uintptr_t address, std::size_t size,
                                std::uint32_t required_flags) {
    if (address < load_bias) return false;
    const std::uint64_t rva = address - load_bias;
    for (const auto& program : programs) {
      if (program.p_type != PT_LOAD ||
          (program.p_flags & required_flags) != required_flags ||
          rva < program.p_vaddr)
        continue;
      const std::uint64_t relative = rva - program.p_vaddr;
      if (relative <= program.p_filesz && size <= program.p_filesz - relative)
        return true;
    }
    return false;
  };
  if (!in_file_load(output->wrapper, 24, PF_R | PF_X)) return false;
  const std::uintptr_t locators[] = {
      output->shadow_data,       output->control_data,
      output->evidence_data,     output->frames_data,
      output->shadow_size_data,  output->control_size_data,
      output->evidence_size_data, output->frame_size_data,
      output->frame_capacity_data, output->prefix_size_data,
      output->update_slot_data,
  };
  for (const std::uintptr_t locator : locators)
    if (!in_file_load(locator, 8, PF_R | PF_W)) return false;
  return true;
}

inline bool NonOverlapping(const Layout& layout) {
  struct Range {
    std::uintptr_t begin;
    std::uintptr_t end;
  } ranges[4]{};
  const std::uintptr_t begins[4] = {
      layout.shadow, layout.control, layout.evidence, layout.frames,
  };
  const std::uint64_t sizes[4] = {
      layout.shadow_size, layout.control_size, layout.evidence_size,
      layout.frame_size * layout.frame_capacity,
  };
  for (std::size_t index = 0; index < 4; ++index) {
    if (begins[index] == 0 || sizes[index] == 0 ||
        sizes[index] > UINTPTR_MAX ||
        begins[index] > UINTPTR_MAX - static_cast<std::uintptr_t>(sizes[index]))
      return false;
    ranges[index] = {
        begins[index],
        begins[index] + static_cast<std::uintptr_t>(sizes[index]),
    };
  }
  for (std::size_t left = 0; left < 4; ++left)
    for (std::size_t right = left + 1; right < 4; ++right)
      if (ranges[left].begin < ranges[right].end &&
          ranges[right].begin < ranges[left].end)
        return false;
  return true;
}

}  // namespace detail

inline bool Resolve(pid_t pid, int process_mem, Layout* output) {
  if (pid <= 0 || process_mem < 0 || output == nullptr) return false;
  std::vector<Mapping> all_maps;
  std::vector<Mapping> payload_maps;
  std::string path;
  std::uintptr_t load_bias = 0;
  if (!detail::ReadMappings(pid, &all_maps, &payload_maps, &path, &load_bias))
    return false;
  const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (file < 0) return false;
  struct stat info {};
  std::uint8_t hash[32]{};
  const bool file_ok = fstat(file, &info) == 0 && info.st_size >= 4096 &&
                       info.st_size <= 8 * 1024 * 1024 &&
                       detail::HashFile(file, hash) &&
                       std::memcmp(hash, kExpectedSha256, sizeof(hash)) == 0;
  detail::Symbols symbols{};
  const bool symbols_ok =
      file_ok && detail::ResolveSymbols(
                     file, static_cast<std::uint64_t>(info.st_size),
                     payload_maps, path, load_bias, &symbols);
  close(file);
  if (!symbols_ok) return false;

  Layout layout{};
  layout.load_bias = load_bias;
  layout.wrapper = symbols.wrapper;
  const auto* wrapper_mapping = detail::MappingAt(all_maps, layout.wrapper, 24);
  if (wrapper_mapping == nullptr || wrapper_mapping->path != path ||
      wrapper_mapping->perms[0] != 'r' || wrapper_mapping->perms[1] == 'w')
    return false;
  const std::uintptr_t locators[] = {
      symbols.shadow_data,       symbols.control_data,
      symbols.evidence_data,     symbols.frames_data,
      symbols.shadow_size_data,  symbols.control_size_data,
      symbols.evidence_size_data, symbols.frame_size_data,
      symbols.frame_capacity_data, symbols.prefix_size_data,
      symbols.update_slot_data,
  };
  for (const std::uintptr_t address : locators) {
    const auto* mapping = detail::MappingAt(all_maps, address, 8);
    if (mapping == nullptr || mapping->path != path ||
        mapping->perms[0] != 'r')
      return false;
  }
  if (!detail::ReadAt(process_mem, symbols.shadow_data, &layout.shadow, 8) ||
      !detail::ReadAt(process_mem, symbols.control_data, &layout.control, 8) ||
      !detail::ReadAt(process_mem, symbols.evidence_data, &layout.evidence, 8) ||
      !detail::ReadAt(process_mem, symbols.frames_data, &layout.frames, 8) ||
      !detail::ReadAt(process_mem, symbols.shadow_size_data,
                      &layout.shadow_size, 8) ||
      !detail::ReadAt(process_mem, symbols.control_size_data,
                      &layout.control_size, 8) ||
      !detail::ReadAt(process_mem, symbols.evidence_size_data,
                      &layout.evidence_size, 8) ||
      !detail::ReadAt(process_mem, symbols.frame_size_data,
                      &layout.frame_size, 8) ||
      !detail::ReadAt(process_mem, symbols.frame_capacity_data,
                      &layout.frame_capacity, 8) ||
      !detail::ReadAt(process_mem, symbols.prefix_size_data,
                      &layout.prefix_size, 8) ||
      !detail::ReadAt(process_mem, symbols.update_slot_data,
                      &layout.update_slot, 8))
    return false;
  if (layout.shadow_size != protocol::kControllerShadowSize ||
      layout.control_size != sizeof(protocol::Control) ||
      layout.evidence_size != sizeof(protocol::Evidence) ||
      layout.frame_size != sizeof(unified_tick_v1::RecordingFrameV1) ||
      layout.frame_capacity != protocol::kMaximumFrames ||
      layout.prefix_size != protocol::kControllerPrefixSize ||
      layout.update_slot != protocol::kControllerUpdateSlotOffset ||
      (layout.wrapper & 3u) != 0 || (layout.shadow & 63u) != 0 ||
      (layout.control & 63u) != 0 || (layout.evidence & 63u) != 0 ||
      (layout.frames & 63u) != 0 || !detail::NonOverlapping(layout))
    return false;
  const std::size_t frame_bytes =
      static_cast<std::size_t>(layout.frame_size * layout.frame_capacity);
  if (!detail::WritableRange(all_maps, layout.shadow, layout.shadow_size) ||
      !detail::WritableRange(all_maps, layout.control, layout.control_size) ||
      !detail::WritableRange(all_maps, layout.evidence,
                             layout.evidence_size) ||
      !detail::WritableRange(all_maps, layout.frames, frame_bytes))
    return false;
  std::memcpy(layout.file_sha256, hash, sizeof(hash));
  std::memcpy(layout.mapped_path, path.c_str(), path.size() + 1);
  *output = layout;
  return true;
}

}  // namespace a9tas::controller_shadow_coordinator_elf_v1
