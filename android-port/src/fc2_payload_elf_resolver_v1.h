#pragma once

// Read-only resolver for the preloaded FC-2 ARM64 payload.  This resolver
// hashes the mapped file, proves its ELF/relocation structure, and reads only
// payload-owned locator objects through /proc/PID/mem.  It never invokes ARM64
// guest code and never writes target memory.

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

namespace a9tas::fc2_payload_elf_v1 {

constexpr char kPayloadBasename[] =
    "liba9tas_frame_callback_deferred_registration_v1_build_only.so";
constexpr std::uint8_t kExpectedSha256[32] = {
    0x55, 0x0f, 0xc3, 0xe5, 0x79, 0xd7, 0x32, 0xa9,
    0xf5, 0x70, 0xea, 0x86, 0x9e, 0xb4, 0x87, 0x16,
    0x1e, 0xd2, 0xe9, 0x5c, 0x35, 0x40, 0xfc, 0x67,
    0xc8, 0xf6, 0x78, 0x5e, 0x7a, 0xbb, 0xce, 0xfb,
};

using FileMapping = a9tas::fc1_payload_elf_v1::FileMapping;
namespace common = a9tas::fc1_payload_elf_v1::detail;

struct Layout {
  std::uintptr_t load_bias{};
  std::uintptr_t wrapper{};
  std::uintptr_t dedicated_observer{};
  std::uintptr_t shadow{};
  std::uintptr_t control{};
  std::uintptr_t evidence{};
  std::uintptr_t dedicated_object{};
  std::uintptr_t dedicated_vptr{};
  std::uintptr_t fail_safe_callback{};
  std::uint64_t control_size{};
  std::uint64_t evidence_size{};
  std::uint8_t file_sha256[32]{};
  char mapped_path[1024]{};
};

namespace detail {

inline bool ReadPayloadMappings(pid_t pid, std::vector<FileMapping>* output,
                                std::string* path_out,
                                std::uintptr_t* load_bias_out) {
  char maps_path[64]{};
  std::snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps",
                static_cast<int>(pid));
  FILE* file = std::fopen(maps_path, "re");
  if (!file) return false;
  char line[2048]{};
  std::vector<FileMapping> matches;
  while (std::fgets(line, sizeof(line), file)) {
    unsigned long long begin = 0, end = 0, offset = 0;
    char perms[5]{}, path[1024]{};
    const int fields = std::sscanf(
        line, "%llx-%llx %4s %llx %*s %*s %1023[^\n]", &begin, &end,
        perms, &offset, path);
    if (fields != 5) continue;
    std::string clean(path);
    while (!clean.empty() && clean.front() == ' ') clean.erase(0, 1);
    if (clean.find(kPayloadBasename) == std::string::npos) continue;
    FileMapping mapping{static_cast<std::uintptr_t>(begin),
                        static_cast<std::uintptr_t>(end), offset, {}, clean};
    std::memcpy(mapping.perms, perms, 4);
    matches.push_back(mapping);
  }
  std::fclose(file);
  if (matches.size() < 2) return false;
  const std::string path = matches.front().path;
  if (path.find(" (deleted)") != std::string::npos) return false;
  std::uintptr_t bias = UINTPTR_MAX;
  bool readable = false;
  bool writable = false;
  for (const auto& mapping : matches) {
    if (mapping.path != path) return false;
    if (mapping.file_offset == 0) bias = std::min(bias, mapping.begin);
    readable = readable || mapping.perms[0] == 'r';
    writable = writable || mapping.perms[1] == 'w';
  }
  if (bias == UINTPTR_MAX || !readable || !writable || path.size() >= 1024)
    return false;
  for (const auto& mapping : matches) {
    if (mapping.begin < bias || mapping.end < mapping.begin ||
        mapping.end - bias > 8 * 1024 * 1024 ||
        mapping.file_offset > 8 * 1024 * 1024)
      return false;
  }
  *output = std::move(matches);
  *path_out = path;
  *load_bias_out = bias;
  return true;
}

struct SymbolResult {
  std::uintptr_t wrapper{};
  std::uintptr_t dedicated_observer{};
  std::uintptr_t shadow_data{};
  std::uintptr_t control_data{};
  std::uintptr_t evidence_data{};
  std::uintptr_t dedicated_object_data{};
  std::uintptr_t dedicated_vtable_data{};
  std::uintptr_t control_size_data{};
  std::uintptr_t evidence_size_data{};
  std::uintptr_t shadow_rva{};
  std::uintptr_t control_rva{};
  std::uintptr_t evidence_rva{};
  std::uintptr_t dedicated_object_rva{};
  std::uintptr_t dedicated_vptr_rva{};
  std::uintptr_t fail_safe_rva{};
};

inline bool ResolveSymbols(int fd, std::uint64_t file_size,
                           const std::vector<FileMapping>& maps,
                           const std::string& mapped_path,
                           std::uintptr_t load_bias, SymbolResult* output) {
  Elf64_Ehdr header{};
  if (!common::ReadAt(fd, 0, &header, sizeof(header)) ||
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
  if (!common::ReadAt(fd, header.e_phoff, programs.data(),
                      programs.size() * sizeof(programs.front())))
    return false;
  std::uint32_t load_segments = 0;
  for (const auto& program : programs) {
    if (program.p_type != PT_LOAD) continue;
    if (program.p_filesz > program.p_memsz || program.p_align != 0x1000 ||
        program.p_offset > file_size ||
        program.p_filesz > file_size - program.p_offset ||
        program.p_vaddr > UINTPTR_MAX ||
        (program.p_offset & 0xFFFu) != (program.p_vaddr & 0xFFFu))
      return false;
    const std::uint64_t page_offset = common::PageDown(program.p_offset);
    const std::uint64_t page_vaddr = common::PageDown(program.p_vaddr);
    if (page_vaddr > UINTPTR_MAX - load_bias) return false;
    const auto expected_begin =
        load_bias + static_cast<std::uintptr_t>(page_vaddr);
    bool exact_mapping = false;
    for (const auto& mapping : maps) {
      if (mapping.begin == expected_begin &&
          mapping.file_offset == page_offset && mapping.path == mapped_path) {
        exact_mapping = true;
        break;
      }
    }
    if (!exact_mapping) return false;
    ++load_segments;
  }
  if (load_segments < 2) return false;

  std::vector<Elf64_Shdr> sections(header.e_shnum);
  if (!common::ReadAt(fd, header.e_shoff, sections.data(),
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
  std::vector<Elf64_Sym> symbols(
      static_cast<std::size_t>(dynsym->sh_size / sizeof(Elf64_Sym)));
  std::vector<char> strings(static_cast<std::size_t>(strings_header.sh_size));
  if (!common::ReadAt(fd, dynsym->sh_offset, symbols.data(), dynsym->sh_size) ||
      !common::ReadAt(fd, strings_header.sh_offset, strings.data(),
                      strings.size()))
    return false;

  struct Wanted {
    const char* name;
    unsigned char type;
    std::uintptr_t* address;
    std::uint32_t matches;
    std::uint32_t symbol_index;
  } wanted[] = {
      {"a9tas_fc2_frame_callback_bootstrap_v1", STT_FUNC,
       &output->wrapper, 0, 0},
      {"a9tas_fc2_dedicated_observer_v1", STT_FUNC,
       &output->dedicated_observer, 0, 0},
      {"a9tas_fc2_shadow_storage_data_v1", STT_OBJECT,
       &output->shadow_data, 0, 0},
      {"a9tas_fc2_control_storage_data_v1", STT_OBJECT,
       &output->control_data, 0, 0},
      {"a9tas_fc2_evidence_storage_data_v1", STT_OBJECT,
       &output->evidence_data, 0, 0},
      {"a9tas_fc2_dedicated_object_data_v1", STT_OBJECT,
       &output->dedicated_object_data, 0, 0},
      {"a9tas_fc2_dedicated_vtable_data_v1", STT_OBJECT,
       &output->dedicated_vtable_data, 0, 0},
      {"a9tas_fc2_control_size_data_v1", STT_OBJECT,
       &output->control_size_data, 0, 0},
      {"a9tas_fc2_evidence_size_data_v1", STT_OBJECT,
       &output->evidence_size_data, 0, 0},
  };
  for (std::size_t index = 0; index < symbols.size(); ++index) {
    const auto& symbol = symbols[index];
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
      *item.address = load_bias + static_cast<std::uintptr_t>(symbol.st_value);
      item.symbol_index = static_cast<std::uint32_t>(index);
      ++item.matches;
    }
  }
  for (const auto& item : wanted)
    if (item.matches != 1 || *item.address == 0) return false;

  std::uint32_t observer_symbol_index = 0;
  for (const auto& item : wanted)
    if (std::strcmp(item.name, "a9tas_fc2_dedicated_observer_v1") == 0)
      observer_symbol_index = item.symbol_index;
  if (observer_symbol_index == 0) return false;

  struct Relocation {
    std::uint32_t type{};
    std::uint32_t symbol{};
    std::int64_t addend{};
    std::uint32_t matches{};
  };
  const auto find_relocation = [&](std::uintptr_t target,
                                   Relocation* result) {
    for (const auto& section : sections) {
      if (section.sh_type != SHT_RELA) continue;
      if (section.sh_entsize != sizeof(Elf64_Rela) ||
          section.sh_size % sizeof(Elf64_Rela) != 0 ||
          section.sh_offset > file_size ||
          section.sh_size > file_size - section.sh_offset)
        return false;
      const std::size_t count = section.sh_size / sizeof(Elf64_Rela);
      for (std::size_t index = 0; index < count; ++index) {
        Elf64_Rela relocation{};
        if (!common::ReadAt(fd,
                            section.sh_offset + index * sizeof(relocation),
                            &relocation, sizeof(relocation)))
          return false;
        if (relocation.r_offset != target) continue;
        result->type = ELF64_R_TYPE(relocation.r_info);
        result->symbol = ELF64_R_SYM(relocation.r_info);
        result->addend = relocation.r_addend;
        ++result->matches;
      }
    }
    return true;
  };
  const auto symbol_rva = [&](std::uintptr_t address) {
    return address - load_bias;
  };
  const std::uintptr_t pointer_locators[] = {
      output->shadow_data, output->control_data, output->evidence_data,
      output->dedicated_object_data, output->dedicated_vtable_data,
  };
  std::uintptr_t pointer_targets[5]{};
  for (std::size_t index = 0; index < 5; ++index) {
    Relocation relocation{};
    if (!find_relocation(symbol_rva(pointer_locators[index]), &relocation) ||
        relocation.matches != 1 || relocation.type != R_AARCH64_RELATIVE ||
        relocation.symbol != 0 || relocation.addend <= 0)
      return false;
    pointer_targets[index] = static_cast<std::uintptr_t>(relocation.addend);
  }
  output->shadow_rva = pointer_targets[0];
  output->control_rva = pointer_targets[1];
  output->evidence_rva = pointer_targets[2];
  output->dedicated_object_rva = pointer_targets[3];
  output->dedicated_vptr_rva = pointer_targets[4];

  Relocation object_vptr{};
  Relocation slot_zero{};
  Relocation slot_eight{};
  Relocation observer_slot{};
  if (!find_relocation(output->dedicated_object_rva, &object_vptr) ||
      !find_relocation(output->dedicated_vptr_rva, &slot_zero) ||
      !find_relocation(output->dedicated_vptr_rva + 8, &slot_eight) ||
      !find_relocation(output->dedicated_vptr_rva + 16, &observer_slot) ||
      object_vptr.matches != 1 || object_vptr.type != R_AARCH64_RELATIVE ||
      object_vptr.symbol != 0 ||
      object_vptr.addend !=
          static_cast<std::int64_t>(output->dedicated_vptr_rva) ||
      slot_zero.matches != 1 || slot_zero.type != R_AARCH64_RELATIVE ||
      slot_zero.symbol != 0 || slot_zero.addend <= 0 ||
      slot_eight.matches != 1 || slot_eight.type != R_AARCH64_RELATIVE ||
      slot_eight.symbol != 0 || slot_eight.addend != slot_zero.addend ||
      observer_slot.matches != 1 || observer_slot.type != R_AARCH64_ABS64 ||
      observer_slot.symbol != observer_symbol_index || observer_slot.addend != 0)
    return false;
  output->fail_safe_rva = static_cast<std::uintptr_t>(slot_zero.addend);

  const auto in_file_load = [&](std::uintptr_t rva, std::size_t size,
                                std::uint32_t required_flags) {
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
  const auto runtime_rva = [&](std::uintptr_t address) {
    return address - load_bias;
  };
  if (!in_file_load(runtime_rva(output->wrapper), 24, PF_R | PF_X) ||
      !in_file_load(runtime_rva(output->dedicated_observer), 24,
                    PF_R | PF_X) ||
      !in_file_load(output->fail_safe_rva, 8, PF_R | PF_X) ||
      !in_file_load(output->shadow_rva, 0x908, PF_R | PF_W) ||
      !in_file_load(output->control_rva, 128, PF_R | PF_W) ||
      !in_file_load(output->evidence_rva, 256, PF_R | PF_W) ||
      !in_file_load(output->dedicated_object_rva, 8, PF_R | PF_W) ||
      output->dedicated_vptr_rva < 16 ||
      !in_file_load(output->dedicated_vptr_rva - 16, 40, PF_R | PF_W))
    return false;

  const auto read_file_rva = [&](std::uintptr_t rva, void* value,
                                 std::size_t size) {
    for (const auto& program : programs) {
      if (program.p_type != PT_LOAD || rva < program.p_vaddr) continue;
      const std::uint64_t relative = rva - program.p_vaddr;
      if (relative > program.p_filesz || size > program.p_filesz - relative)
        continue;
      return common::ReadAt(fd, program.p_offset + relative, value, size);
    }
    return false;
  };
  std::uint64_t prefix[2]{1, 1};
  if (!read_file_rva(output->dedicated_vptr_rva - 16, prefix,
                     sizeof(prefix)) ||
      prefix[0] != 0 || prefix[1] != 0)
    return false;
  return true;
}

}  // namespace detail

inline bool Resolve(pid_t pid, int process_mem, Layout* output) {
  if (pid <= 0 || process_mem < 0 || output == nullptr) return false;
  std::vector<FileMapping> maps;
  std::string path;
  std::uintptr_t load_bias = 0;
  if (!detail::ReadPayloadMappings(pid, &maps, &path, &load_bias)) return false;
  const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (file < 0) return false;
  struct stat info {};
  std::uint8_t hash[32]{};
  const bool file_ok = fstat(file, &info) == 0 && info.st_size >= 4096 &&
                       info.st_size <= 8 * 1024 * 1024 &&
                       common::HashFile(file, hash) &&
                       std::memcmp(hash, kExpectedSha256, sizeof(hash)) == 0;
  detail::SymbolResult symbols{};
  const bool symbols_ok =
      file_ok && detail::ResolveSymbols(
                     file, static_cast<std::uint64_t>(info.st_size), maps, path,
                     load_bias, &symbols);
  close(file);
  if (!symbols_ok) return false;

  Layout layout{};
  layout.load_bias = load_bias;
  layout.wrapper = symbols.wrapper;
  layout.dedicated_observer = symbols.dedicated_observer;
  const std::uintptr_t locator_addresses[] = {
      symbols.shadow_data,
      symbols.control_data,
      symbols.evidence_data,
      symbols.dedicated_object_data,
      symbols.dedicated_vtable_data,
      symbols.control_size_data,
      symbols.evidence_size_data,
  };
  for (const std::uintptr_t address : locator_addresses) {
    const auto* mapping = common::MappingAt(maps, address, 8);
    if (!mapping || mapping->path != path || mapping->perms[0] != 'r')
      return false;
  }
  for (const std::uintptr_t address : {layout.wrapper,
                                       layout.dedicated_observer}) {
    const auto* mapping = common::MappingAt(maps, address, 24);
    if (!mapping || mapping->path != path || mapping->perms[0] != 'r' ||
        mapping->perms[1] == 'w')
      return false;
  }

  if (!common::ReadAt(process_mem, symbols.shadow_data, &layout.shadow,
                      sizeof(layout.shadow)) ||
      !common::ReadAt(process_mem, symbols.control_data, &layout.control,
                      sizeof(layout.control)) ||
      !common::ReadAt(process_mem, symbols.evidence_data, &layout.evidence,
                      sizeof(layout.evidence)) ||
      !common::ReadAt(process_mem, symbols.dedicated_object_data,
                      &layout.dedicated_object,
                      sizeof(layout.dedicated_object)) ||
      !common::ReadAt(process_mem, symbols.dedicated_vtable_data,
                      &layout.dedicated_vptr,
                      sizeof(layout.dedicated_vptr)) ||
      !common::ReadAt(process_mem, symbols.control_size_data,
                      &layout.control_size, sizeof(layout.control_size)) ||
      !common::ReadAt(process_mem, symbols.evidence_size_data,
                      &layout.evidence_size, sizeof(layout.evidence_size)) ||
      layout.control_size != 128 || layout.evidence_size != 256 ||
      (layout.shadow & 63u) != 0 || (layout.control & 63u) != 0 ||
      (layout.evidence & 63u) != 0 || (layout.dedicated_object & 63u) != 0 ||
      (layout.dedicated_vptr & 7u) != 0 || (layout.wrapper & 3u) != 0 ||
      (layout.dedicated_observer & 3u) != 0)
    return false;

  std::uintptr_t runtime_object_vptr = 0;
  std::uintptr_t runtime_slots[3]{};
  std::uint64_t runtime_prefix[2]{1, 1};
  if (!common::ReadAt(process_mem, layout.dedicated_object,
                      &runtime_object_vptr, sizeof(runtime_object_vptr)) ||
      layout.dedicated_vptr < 16 ||
      !common::ReadAt(process_mem, layout.dedicated_vptr - 16,
                      runtime_prefix, sizeof(runtime_prefix)) ||
      !common::ReadAt(process_mem, layout.dedicated_vptr, runtime_slots,
                      sizeof(runtime_slots)) ||
      runtime_object_vptr != layout.dedicated_vptr ||
      runtime_prefix[0] != 0 || runtime_prefix[1] != 0 ||
      runtime_slots[0] == 0 || runtime_slots[0] != runtime_slots[1] ||
      runtime_slots[2] != layout.dedicated_observer)
    return false;
  layout.fail_safe_callback = runtime_slots[0];

  const auto expected_runtime = [&](std::uintptr_t rva) {
    return load_bias + rva;
  };
  if (layout.shadow != expected_runtime(symbols.shadow_rva) ||
      layout.control != expected_runtime(symbols.control_rva) ||
      layout.evidence != expected_runtime(symbols.evidence_rva) ||
      layout.dedicated_object !=
          expected_runtime(symbols.dedicated_object_rva) ||
      layout.dedicated_vptr != expected_runtime(symbols.dedicated_vptr_rva) ||
      layout.fail_safe_callback != expected_runtime(symbols.fail_safe_rva))
    return false;

  struct Region {
    std::uintptr_t address;
    std::size_t size;
  } writable[] = {
      {layout.shadow, 0x908},
      {layout.control, 128},
      {layout.evidence, 256},
      {layout.dedicated_object, 8},
      {layout.dedicated_vptr - 16, 40},
  };
  for (const auto& region : writable) {
    const auto* mapping = common::MappingAt(maps, region.address, region.size);
    if (!mapping || mapping->path != path || mapping->perms[0] != 'r' ||
        mapping->perms[1] != 'w')
      return false;
  }
  const auto* fail_safe_map =
      common::MappingAt(maps, layout.fail_safe_callback, 8);
  if (!fail_safe_map || fail_safe_map->path != path ||
      fail_safe_map->perms[0] != 'r' || fail_safe_map->perms[1] == 'w')
    return false;

  std::memcpy(layout.file_sha256, hash, sizeof(hash));
  std::memcpy(layout.mapped_path, path.c_str(), path.size() + 1);
  *output = layout;
  return true;
}

}  // namespace a9tas::fc2_payload_elf_v1
