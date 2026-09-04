#pragma once

// Read-only resolver for the hash-pinned final-writer replay payload.  It
// resolves only payload-owned code/data and never invokes guest code or writes
// process memory.

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

#include "fc1_payload_elf_resolver_v1.h"
#include "final_writer_replay_protocol_v1.h"

namespace a9tas::final_writer_replay_elf_v1 {

inline constexpr char kPayloadBasename[] =
    "liba9tas_final_writer_replay_v1_build_only.so";
inline constexpr std::uint8_t kExpectedSha256[32] = {
    0xa6, 0x98, 0xce, 0xb0, 0x2b, 0xc6, 0x8d, 0xb8,
    0x92, 0xd5, 0x4a, 0xf8, 0x4a, 0xda, 0x6a, 0x74,
    0xf5, 0x9f, 0x15, 0x13, 0x18, 0x93, 0x76, 0x23,
    0x8a, 0x5b, 0x5b, 0x19, 0xcf, 0x15, 0xb7, 0x63,
};

struct Layout {
  std::uintptr_t load_bias{};
  std::uintptr_t wrapper{};
  std::uintptr_t shadow{};
  std::uintptr_t control{};
  std::uintptr_t targets{};
  std::uintptr_t audits{};
  std::uintptr_t evidence{};
  std::uint64_t control_size{};
  std::uint64_t target_size{};
  std::uint64_t audit_size{};
  std::uint64_t evidence_size{};
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
  if (path.size() < sizeof(kPayloadBasename) - 1) return false;
  const std::size_t offset = path.size() - (sizeof(kPayloadBasename) - 1);
  if (path.compare(offset, sizeof(kPayloadBasename) - 1,
                   kPayloadBasename) != 0)
    return false;
  return offset == 0 || path[offset - 1] == '/';
}

inline bool ReadMappings(pid_t pid, std::vector<Mapping>* all_out,
                         std::vector<Mapping>* payload_out,
                         std::string* path_out,
                         std::uintptr_t* load_bias_out) {
  char maps_path[64]{};
  std::snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps",
                static_cast<int>(pid));
  FILE* file = std::fopen(maps_path, "re");
  if (!file) return false;
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

  std::vector<Mapping> payload;
  std::uintptr_t bias = UINTPTR_MAX;
  bool readable = false, writable = false;
  for (const auto& mapping : all) {
    if (mapping.path != selected) continue;
    payload.push_back(mapping);
    if (mapping.file_offset == 0) bias = std::min(bias, mapping.begin);
    readable = readable || mapping.perms[0] == 'r';
    writable = writable || mapping.perms[1] == 'w';
  }
  if (payload.size() < 2 || bias == UINTPTR_MAX || !readable || !writable)
    return false;
  *all_out = std::move(all);
  *payload_out = std::move(payload);
  *path_out = selected;
  *load_bias_out = bias;
  return true;
}

inline const Mapping* MappingAt(const std::vector<Mapping>& maps,
                                std::uintptr_t address, std::size_t size) {
  if (size == 0 || address > UINTPTR_MAX - size) return nullptr;
  const std::uintptr_t end = address + size;
  for (const auto& mapping : maps)
    if (address >= mapping.begin && end <= mapping.end) return &mapping;
  return nullptr;
}

struct Symbols {
  std::uintptr_t wrapper{};
  std::uintptr_t shadow_data{};
  std::uintptr_t control_data{};
  std::uintptr_t targets_data{};
  std::uintptr_t audits_data{};
  std::uintptr_t evidence_data{};
  std::uintptr_t control_size_data{};
  std::uintptr_t target_size_data{};
  std::uintptr_t audit_size_data{};
  std::uintptr_t evidence_size_data{};
};

inline bool ResolveSymbols(int fd, std::uint64_t file_size,
                           const std::vector<Mapping>& payload_maps,
                           const std::string& mapped_path,
                           std::uintptr_t load_bias, Symbols* output) {
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
  const Elf64_Shdr* dynsym = nullptr;
  for (const auto& section : sections) {
    if (section.sh_type != SHT_DYNSYM) continue;
    if (dynsym != nullptr) return false;
    dynsym = &section;
  }
  if (!dynsym || dynsym->sh_entsize != sizeof(Elf64_Sym) ||
      dynsym->sh_link >= sections.size() || dynsym->sh_size == 0 ||
      dynsym->sh_size % sizeof(Elf64_Sym) != 0 ||
      dynsym->sh_size > 1024 * 1024 || dynsym->sh_offset > file_size ||
      dynsym->sh_size > file_size - dynsym->sh_offset)
    return false;
  const Elf64_Shdr& strings_header = sections[dynsym->sh_link];
  if (strings_header.sh_type != SHT_STRTAB || strings_header.sh_size == 0 ||
      strings_header.sh_size > 1024 * 1024 ||
      strings_header.sh_offset > file_size ||
      strings_header.sh_size > file_size - strings_header.sh_offset)
    return false;
  std::vector<Elf64_Sym> symbols(dynsym->sh_size / sizeof(Elf64_Sym));
  std::vector<char> strings(strings_header.sh_size);
  if (!ReadAt(fd, dynsym->sh_offset, symbols.data(), dynsym->sh_size) ||
      !ReadAt(fd, strings_header.sh_offset, strings.data(), strings.size()))
    return false;

  struct Wanted {
    const char* name;
    unsigned char type;
    std::uintptr_t* address;
    std::uint32_t matches;
  } wanted[] = {
      {"a9tas_final_writer_replay_callback_v1", STT_FUNC, &output->wrapper, 0},
      {"a9tas_final_writer_replay_shadow_storage_data_v1", STT_OBJECT,
       &output->shadow_data, 0},
      {"a9tas_final_writer_replay_control_storage_data_v1", STT_OBJECT,
       &output->control_data, 0},
      {"a9tas_final_writer_replay_target_storage_data_v1", STT_OBJECT,
       &output->targets_data, 0},
      {"a9tas_final_writer_replay_audit_storage_data_v1", STT_OBJECT,
       &output->audits_data, 0},
      {"a9tas_final_writer_replay_evidence_storage_data_v1", STT_OBJECT,
       &output->evidence_data, 0},
      {"a9tas_final_writer_replay_control_size_data_v1", STT_OBJECT,
       &output->control_size_data, 0},
      {"a9tas_final_writer_replay_target_size_data_v1", STT_OBJECT,
       &output->target_size_data, 0},
      {"a9tas_final_writer_replay_audit_size_data_v1", STT_OBJECT,
       &output->audit_size_data, 0},
      {"a9tas_final_writer_replay_evidence_size_data_v1", STT_OBJECT,
       &output->evidence_size_data, 0},
  };
  for (const auto& symbol : symbols) {
    if (symbol.st_name >= strings.size() || symbol.st_shndx == SHN_UNDEF ||
        ELF64_ST_BIND(symbol.st_info) != STB_GLOBAL)
      continue;
    const char* name = strings.data() + symbol.st_name;
    if (!std::memchr(name, '\0', strings.size() - symbol.st_name)) return false;
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
      output->shadow_data, output->control_data, output->targets_data,
      output->audits_data, output->evidence_data, output->control_size_data,
      output->target_size_data, output->audit_size_data,
      output->evidence_size_data,
  };
  for (const auto locator : locators)
    if (!in_file_load(locator, 8, PF_R | PF_W)) return false;
  return true;
}

inline bool WritableRange(const std::vector<Mapping>& maps,
                          std::uintptr_t address, std::size_t size) {
  if (size == 0 || address > UINTPTR_MAX - size) return false;
  const std::uintptr_t wanted_end = address + size;
  std::uintptr_t cursor = address;
  while (cursor < wanted_end) {
    const Mapping* covering = nullptr;
    for (const auto& mapping : maps)
      if (cursor >= mapping.begin && cursor < mapping.end) {
        covering = &mapping;
        break;
      }
    if (!covering || covering->perms[0] != 'r' ||
        covering->perms[1] != 'w')
      return false;
    cursor = std::min(wanted_end, covering->end);
  }
  return true;
}

}  // namespace detail

inline bool Resolve(pid_t pid, int process_mem, Layout* output) {
  using namespace a9tas::final_writer_replay_v1;
  if (pid <= 0 || process_mem < 0 || output == nullptr) return false;
  std::vector<Mapping> all_maps, payload_maps;
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
  const std::uintptr_t locator_addresses[] = {
      symbols.shadow_data, symbols.control_data, symbols.targets_data,
      symbols.audits_data, symbols.evidence_data, symbols.control_size_data,
      symbols.target_size_data, symbols.audit_size_data,
      symbols.evidence_size_data,
  };
  const auto* wrapper_map = detail::MappingAt(all_maps, layout.wrapper, 24);
  if (!wrapper_map || wrapper_map->path != path ||
      wrapper_map->perms[0] != 'r' || wrapper_map->perms[1] == 'w')
    return false;
  for (const auto address : locator_addresses) {
    const auto* mapping = detail::MappingAt(all_maps, address, 8);
    if (!mapping || mapping->path != path || mapping->perms[0] != 'r')
      return false;
  }

  if (!detail::ReadAt(process_mem, symbols.shadow_data, &layout.shadow, 8) ||
      !detail::ReadAt(process_mem, symbols.control_data, &layout.control, 8) ||
      !detail::ReadAt(process_mem, symbols.targets_data, &layout.targets, 8) ||
      !detail::ReadAt(process_mem, symbols.audits_data, &layout.audits, 8) ||
      !detail::ReadAt(process_mem, symbols.evidence_data, &layout.evidence, 8) ||
      !detail::ReadAt(process_mem, symbols.control_size_data,
                      &layout.control_size, 8) ||
      !detail::ReadAt(process_mem, symbols.target_size_data,
                      &layout.target_size, 8) ||
      !detail::ReadAt(process_mem, symbols.audit_size_data,
                      &layout.audit_size, 8) ||
      !detail::ReadAt(process_mem, symbols.evidence_size_data,
                      &layout.evidence_size, 8) ||
      layout.control_size != sizeof(Control) ||
      layout.target_size != sizeof(FrameTarget) ||
      layout.audit_size != sizeof(FrameAudit) ||
      layout.evidence_size != sizeof(Evidence) ||
      (layout.wrapper & 3u) != 0 || (layout.shadow & 63u) != 0 ||
      (layout.control & 63u) != 0 || (layout.targets & 63u) != 0 ||
      (layout.audits & 63u) != 0 || (layout.evidence & 63u) != 0)
    return false;

  if (!detail::WritableRange(all_maps, layout.shadow, kPrimaryShadowSize) ||
      !detail::WritableRange(all_maps, layout.control, sizeof(Control)) ||
      !detail::WritableRange(all_maps, layout.targets,
                             sizeof(FrameTarget) * kMaximumFrames) ||
      !detail::WritableRange(all_maps, layout.audits,
                             sizeof(FrameAudit) * kMaximumFrames) ||
      !detail::WritableRange(all_maps, layout.evidence, sizeof(Evidence)))
    return false;
  std::memcpy(layout.file_sha256, hash, sizeof(hash));
  std::memcpy(layout.mapped_path, path.c_str(), path.size() + 1);
  *output = layout;
  return true;
}

}  // namespace a9tas::final_writer_replay_elf_v1
