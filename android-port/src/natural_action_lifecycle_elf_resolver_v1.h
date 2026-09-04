#pragma once

// Read-only resolver for the build-only persistent natural-action callback
// payload. It authenticates the exact mapped DSO and resolves payload-owned
// storage through ELF symbols and relocations. It never calls guest code and
// never writes target memory.

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

#ifndef A9TAS_NAL_ACTION_PAYLOAD_REVIEW
#define A9TAS_NAL_ACTION_PAYLOAD_REVIEW 0
#endif

#if A9TAS_NAL_ACTION_PAYLOAD_REVIEW < 0 || \
    A9TAS_NAL_ACTION_PAYLOAD_REVIEW > 2
#error "A9TAS_NAL_ACTION_PAYLOAD_REVIEW must be 0, 1, or 2"
#endif

namespace a9tas::natural_action_lifecycle_elf_v1 {

#if A9TAS_NAL_ACTION_PAYLOAD_REVIEW == 0
constexpr char kPayloadBasename[] =
    "liba9tas_natural_action_callback_lifecycle_v1_build_only.so";
constexpr std::uint8_t kExpectedSha256[32] = {
    0x60, 0xa7, 0x26, 0x17, 0x4a, 0xc2, 0xa6, 0x13,
    0xd6, 0x09, 0x8d, 0xb7, 0x7f, 0x9e, 0x82, 0xbb,
    0xf8, 0x19, 0x92, 0x3f, 0xfd, 0x4e, 0x79, 0x8d,
    0x1f, 0x66, 0xed, 0xa9, 0xdb, 0x45, 0x4a, 0x41,
};
constexpr std::uint8_t kExpectedBuildId[20] = {
    0x39, 0xfb, 0xc2, 0x46, 0xac, 0x28, 0x63, 0x5a, 0x77, 0x46,
    0x08, 0x8f, 0xaf, 0x55, 0x6c, 0xc8, 0x5f, 0x5a, 0x26, 0x05,
};
#elif A9TAS_NAL_ACTION_PAYLOAD_REVIEW == 1
constexpr char kActionPayloadBasename[] =
    "liba9tas_natural_action_scheduler_v1_review_only.so";
constexpr std::uint8_t kExpectedActionSha256[32] = {
    0xd7, 0x18, 0xa1, 0x35, 0x78, 0xe7, 0xe3, 0x7a,
    0x3d, 0x2a, 0x02, 0x40, 0xf9, 0x4b, 0x12, 0x56,
    0x20, 0x85, 0x8d, 0x96, 0x57, 0xd6, 0x9a, 0x4f,
    0xc7, 0x99, 0xb8, 0xc5, 0x26, 0x3b, 0x7f, 0x9e,
};
constexpr std::uint8_t kExpectedActionBuildId[20] = {
    0xb1, 0x72, 0xed, 0x22, 0xad, 0xeb, 0xa6, 0x93, 0x67, 0x3a,
    0x5b, 0xf6, 0x55, 0xc3, 0x9d, 0xb7, 0x9f, 0x0b, 0x0f, 0x87,
};
constexpr const auto& kPayloadBasename = kActionPayloadBasename;
constexpr const auto& kExpectedSha256 = kExpectedActionSha256;
constexpr const auto& kExpectedBuildId = kExpectedActionBuildId;
#else
constexpr char kReplayPayloadBasename[] =
    "liba9tas_natural_action_replay_v1_review_only.so";
constexpr std::uint8_t kExpectedReplaySha256[32] = {
    0xe6, 0x10, 0x82, 0x0b, 0xed, 0x80, 0x2f, 0x19,
    0xf7, 0xae, 0x09, 0xe2, 0xf8, 0x89, 0xdd, 0x82,
    0x60, 0x49, 0xef, 0x50, 0xf0, 0xcb, 0x46, 0x73,
    0xd6, 0xea, 0xb4, 0x59, 0x05, 0x12, 0x47, 0x8f,
};
constexpr std::uint8_t kExpectedReplayBuildId[20] = {
    0x61, 0x95, 0xc6, 0x1b, 0x73, 0x50, 0x5c, 0x94, 0x04, 0xdc,
    0x11, 0xca, 0xff, 0x3b, 0xc6, 0xbb, 0x0a, 0x7e, 0x16, 0x9a,
};
constexpr const auto& kPayloadBasename = kReplayPayloadBasename;
constexpr const auto& kExpectedSha256 = kExpectedReplaySha256;
constexpr const auto& kExpectedBuildId = kExpectedReplayBuildId;
#endif

using FileMapping = a9tas::fc1_payload_elf_v1::FileMapping;
namespace common = a9tas::fc1_payload_elf_v1::detail;

struct Layout {
  std::uintptr_t load_bias{};
  std::uintptr_t bootstrap{};
  std::uintptr_t persistent_consumer{};
  std::uintptr_t shadow{};
  std::uintptr_t control{};
  std::uintptr_t evidence{};
  std::uintptr_t mailbox{};
  std::uintptr_t dedicated_object{};
  std::uintptr_t dedicated_vptr{};
  std::uintptr_t fail_safe_callback{};
  std::uint64_t control_size{};
  std::uint64_t evidence_size{};
  std::uint64_t mailbox_size{};
  std::uint8_t file_sha256[32]{};
  std::uint8_t build_id[20]{};
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
  std::uintptr_t bootstrap{};
  std::uintptr_t persistent_consumer{};
  std::uintptr_t shadow_data{};
  std::uintptr_t control_data{};
  std::uintptr_t evidence_data{};
  std::uintptr_t mailbox_data{};
  std::uintptr_t dedicated_object_data{};
  std::uintptr_t dedicated_vtable_data{};
  std::uintptr_t control_size_data{};
  std::uintptr_t evidence_size_data{};
  std::uintptr_t mailbox_size_data{};
  std::uintptr_t shadow_rva{};
  std::uintptr_t control_rva{};
  std::uintptr_t evidence_rva{};
  std::uintptr_t mailbox_rva{};
  std::uintptr_t dedicated_object_rva{};
  std::uintptr_t dedicated_vptr_rva{};
  std::uintptr_t fail_safe_rva{};
};

inline bool VerifyBuildId(int fd, std::uint64_t file_size,
                          const std::vector<Elf64_Shdr>& sections) {
  std::uint32_t matches = 0;
  for (const auto& section : sections) {
    if (section.sh_type != SHT_NOTE) continue;
    if (section.sh_offset > file_size || section.sh_size > file_size - section.sh_offset ||
        section.sh_size > 64 * 1024)
      return false;
    std::uint64_t cursor = 0;
    while (cursor < section.sh_size) {
      if (section.sh_size - cursor < sizeof(Elf64_Nhdr)) return false;
      Elf64_Nhdr note{};
      if (!common::ReadAt(fd, section.sh_offset + cursor, &note, sizeof(note)))
        return false;
      cursor += sizeof(note);
      const std::uint64_t namesz = (static_cast<std::uint64_t>(note.n_namesz) + 3u) & ~3ull;
      const std::uint64_t descsz = (static_cast<std::uint64_t>(note.n_descsz) + 3u) & ~3ull;
      if (namesz > section.sh_size - cursor) return false;
      char name[8]{};
      if (note.n_namesz > sizeof(name) ||
          (note.n_namesz != 0 && !common::ReadAt(fd, section.sh_offset + cursor,
                                                name, note.n_namesz)))
        return false;
      cursor += namesz;
      if (descsz > section.sh_size - cursor) return false;
      if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
          std::memcmp(name, "GNU", 4) == 0) {
        std::uint8_t build_id[sizeof(kExpectedBuildId)]{};
        if (note.n_descsz != sizeof(build_id) ||
            !common::ReadAt(fd, section.sh_offset + cursor, build_id,
                            sizeof(build_id)) ||
            std::memcmp(build_id, kExpectedBuildId, sizeof(build_id)) != 0)
          return false;
        ++matches;
      }
      cursor += descsz;
    }
  }
  return matches == 1;
}

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
        program.p_offset > file_size || program.p_filesz > file_size - program.p_offset ||
        program.p_vaddr > UINTPTR_MAX ||
        (program.p_offset & 0xFFFu) != (program.p_vaddr & 0xFFFu))
      return false;
    const std::uint64_t page_offset = common::PageDown(program.p_offset);
    const std::uint64_t page_vaddr = common::PageDown(program.p_vaddr);
    if (page_vaddr > UINTPTR_MAX - load_bias) return false;
    const auto expected_begin = load_bias + static_cast<std::uintptr_t>(page_vaddr);
    bool exact_mapping = false;
    for (const auto& mapping : maps) {
      if (mapping.begin == expected_begin && mapping.file_offset == page_offset &&
          mapping.path == mapped_path) {
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
                      sections.size() * sizeof(sections.front())) ||
      !VerifyBuildId(fd, file_size, sections))
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
      !common::ReadAt(fd, strings_header.sh_offset, strings.data(), strings.size()))
    return false;

  struct Wanted {
    const char* name;
    unsigned char type;
    std::uintptr_t* address;
    std::uint32_t matches;
    std::uint32_t symbol_index;
  } wanted[] = {
      {"a9tas_natural_action_registration_bootstrap_v1", STT_FUNC,
       &output->bootstrap, 0, 0},
      {"a9tas_natural_action_persistent_consumer_v1", STT_FUNC,
       &output->persistent_consumer, 0, 0},
      {"a9tas_natural_action_lifecycle_shadow_data_v1", STT_OBJECT,
       &output->shadow_data, 0, 0},
      {"a9tas_natural_action_lifecycle_control_data_v1", STT_OBJECT,
       &output->control_data, 0, 0},
      {"a9tas_natural_action_lifecycle_evidence_data_v1", STT_OBJECT,
       &output->evidence_data, 0, 0},
      {"a9tas_natural_action_lifecycle_mailbox_data_v1", STT_OBJECT,
       &output->mailbox_data, 0, 0},
      {"a9tas_natural_action_lifecycle_object_data_v1", STT_OBJECT,
       &output->dedicated_object_data, 0, 0},
      {"a9tas_natural_action_lifecycle_vtable_data_v1", STT_OBJECT,
       &output->dedicated_vtable_data, 0, 0},
      {"a9tas_natural_action_lifecycle_control_size_v1", STT_OBJECT,
       &output->control_size_data, 0, 0},
      {"a9tas_natural_action_lifecycle_evidence_size_v1", STT_OBJECT,
       &output->evidence_size_data, 0, 0},
      {"a9tas_natural_action_lifecycle_mailbox_size_v1", STT_OBJECT,
       &output->mailbox_size_data, 0, 0},
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
          (item.type == STT_FUNC && symbol.st_size < 8) ||
          symbol.st_value > UINTPTR_MAX - load_bias)
        return false;
      *item.address = load_bias + static_cast<std::uintptr_t>(symbol.st_value);
      item.symbol_index = static_cast<std::uint32_t>(index);
      ++item.matches;
    }
  }
  for (const auto& item : wanted)
    if (item.matches != 1 || *item.address == 0) return false;

  std::uint32_t consumer_symbol_index = 0;
  for (const auto& item : wanted)
    if (std::strcmp(item.name, "a9tas_natural_action_persistent_consumer_v1") == 0)
      consumer_symbol_index = item.symbol_index;
  if (consumer_symbol_index == 0) return false;

  struct Relocation {
    std::uint32_t type{};
    std::uint32_t symbol{};
    std::int64_t addend{};
    std::uint32_t matches{};
  };
  const auto find_relocation = [&](std::uintptr_t target, Relocation* result) {
    for (const auto& section : sections) {
      if (section.sh_type != SHT_RELA) continue;
      if (section.sh_entsize != sizeof(Elf64_Rela) ||
          section.sh_size % sizeof(Elf64_Rela) != 0 ||
          section.sh_offset > file_size || section.sh_size > file_size - section.sh_offset)
        return false;
      const std::size_t count = section.sh_size / sizeof(Elf64_Rela);
      for (std::size_t index = 0; index < count; ++index) {
        Elf64_Rela relocation{};
        if (!common::ReadAt(fd, section.sh_offset + index * sizeof(relocation),
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
  const auto symbol_rva = [&](std::uintptr_t address) { return address - load_bias; };
  const std::uintptr_t pointer_locators[] = {
      output->shadow_data, output->control_data, output->evidence_data,
      output->mailbox_data, output->dedicated_object_data,
      output->dedicated_vtable_data,
  };
  std::uintptr_t pointer_targets[6]{};
  for (std::size_t index = 0; index < 6; ++index) {
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
  output->mailbox_rva = pointer_targets[3];
  output->dedicated_object_rva = pointer_targets[4];
  output->dedicated_vptr_rva = pointer_targets[5];

  Relocation object_vptr{}, slot_zero{}, slot_eight{}, consumer_slot{};
  if (!find_relocation(output->dedicated_object_rva, &object_vptr) ||
      !find_relocation(output->dedicated_vptr_rva, &slot_zero) ||
      !find_relocation(output->dedicated_vptr_rva + 8, &slot_eight) ||
      !find_relocation(output->dedicated_vptr_rva + 16, &consumer_slot) ||
      object_vptr.matches != 1 || object_vptr.type != R_AARCH64_RELATIVE ||
      object_vptr.symbol != 0 ||
      object_vptr.addend != static_cast<std::int64_t>(output->dedicated_vptr_rva) ||
      slot_zero.matches != 1 || slot_zero.type != R_AARCH64_RELATIVE ||
      slot_zero.symbol != 0 || slot_zero.addend <= 0 ||
      slot_eight.matches != 1 || slot_eight.type != R_AARCH64_RELATIVE ||
      slot_eight.symbol != 0 || slot_eight.addend != slot_zero.addend ||
      consumer_slot.matches != 1 || consumer_slot.type != R_AARCH64_ABS64 ||
      consumer_slot.symbol != consumer_symbol_index || consumer_slot.addend != 0)
    return false;
  output->fail_safe_rva = static_cast<std::uintptr_t>(slot_zero.addend);

  const auto in_load = [&](std::uintptr_t rva, std::size_t size,
                           std::uint32_t required_flags,
                           bool require_file_backed) {
    for (const auto& program : programs) {
      if (program.p_type != PT_LOAD ||
          (program.p_flags & required_flags) != required_flags ||
          rva < program.p_vaddr)
        continue;
      const std::uint64_t relative = rva - program.p_vaddr;
      const std::uint64_t extent =
          require_file_backed ? program.p_filesz : program.p_memsz;
      if (relative <= extent && size <= extent - relative)
        return true;
    }
    return false;
  };
  const auto runtime_rva = [&](std::uintptr_t address) { return address - load_bias; };
  if (!in_load(runtime_rva(output->bootstrap), 24, PF_R | PF_X, true) ||
      !in_load(runtime_rva(output->persistent_consumer), 24, PF_R | PF_X,
               true) ||
      !in_load(output->fail_safe_rva, 8, PF_R | PF_X, true) ||
      !in_load(output->shadow_rva, 0x908, PF_R | PF_W, false) ||
      !in_load(output->control_rva, 128, PF_R | PF_W, false) ||
      !in_load(output->evidence_rva, 256, PF_R | PF_W, false) ||
      !in_load(output->mailbox_rva, 192, PF_R | PF_W, false) ||
      !in_load(output->dedicated_object_rva, 8, PF_R | PF_W, false) ||
      output->dedicated_vptr_rva < 16 ||
      !in_load(output->dedicated_vptr_rva - 16, 40, PF_R | PF_W, false))
    return false;
  return true;
}

}  // namespace detail

inline bool Resolve(pid_t pid, int process_mem, Layout* output,
                    const char** failure_stage = nullptr) {
  if (failure_stage) *failure_stage = "invalid_arguments";
  const auto fail = [&](const char* stage) {
    if (failure_stage) *failure_stage = stage;
    return false;
  };
  if (pid <= 0 || process_mem < 0 || output == nullptr) return false;
  std::vector<FileMapping> maps;
  std::string path;
  std::uintptr_t load_bias = 0;
  if (!detail::ReadPayloadMappings(pid, &maps, &path, &load_bias))
    return fail("mapped_segments");
  const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (file < 0) return fail("mapped_file_open");
  struct stat info {};
  std::uint8_t hash[32]{};
  const bool file_ok = fstat(file, &info) == 0 && info.st_size >= 4096 &&
                       info.st_size <= 8 * 1024 * 1024 &&
                       common::HashFile(file, hash) &&
                       std::memcmp(hash, kExpectedSha256, sizeof(hash)) == 0;
  detail::SymbolResult symbols{};
  const bool symbols_ok = file_ok && detail::ResolveSymbols(
      file, static_cast<std::uint64_t>(info.st_size), maps, path, load_bias,
      &symbols);
  close(file);
  if (!file_ok) return fail("mapped_file_identity");
  if (!symbols_ok) return fail("elf_symbols_relocations");

  Layout layout{};
  layout.load_bias = load_bias;
  layout.bootstrap = symbols.bootstrap;
  layout.persistent_consumer = symbols.persistent_consumer;
  const std::uintptr_t locator_addresses[] = {
      symbols.shadow_data, symbols.control_data, symbols.evidence_data,
      symbols.mailbox_data, symbols.dedicated_object_data,
      symbols.dedicated_vtable_data, symbols.control_size_data,
      symbols.evidence_size_data, symbols.mailbox_size_data,
  };
  for (const std::uintptr_t address : locator_addresses) {
    const auto* mapping = common::MappingAt(maps, address, 8);
    if (!mapping || mapping->path != path || mapping->perms[0] != 'r')
      return fail("locator_mapping");
  }
  for (const std::uintptr_t address : {layout.bootstrap,
                                       layout.persistent_consumer}) {
    const auto* mapping = common::MappingAt(maps, address, 24);
    if (!mapping || mapping->path != path || mapping->perms[0] != 'r' ||
        mapping->perms[1] == 'w')
      return fail("function_mapping");
  }

  if (!common::ReadAt(process_mem, symbols.shadow_data, &layout.shadow, 8) ||
      !common::ReadAt(process_mem, symbols.control_data, &layout.control, 8) ||
      !common::ReadAt(process_mem, symbols.evidence_data, &layout.evidence, 8) ||
      !common::ReadAt(process_mem, symbols.mailbox_data, &layout.mailbox, 8) ||
      !common::ReadAt(process_mem, symbols.dedicated_object_data,
                      &layout.dedicated_object, 8) ||
      !common::ReadAt(process_mem, symbols.dedicated_vtable_data,
                      &layout.dedicated_vptr, 8) ||
      !common::ReadAt(process_mem, symbols.control_size_data,
                      &layout.control_size, 8) ||
      !common::ReadAt(process_mem, symbols.evidence_size_data,
                      &layout.evidence_size, 8) ||
      !common::ReadAt(process_mem, symbols.mailbox_size_data,
                      &layout.mailbox_size, 8) ||
      layout.control_size != 128 || layout.evidence_size != 256 ||
      layout.mailbox_size != 192 || (layout.shadow & 63u) != 0 ||
      (layout.control & 63u) != 0 || (layout.evidence & 63u) != 0 ||
      (layout.mailbox & 63u) != 0 || (layout.dedicated_object & 63u) != 0 ||
      (layout.dedicated_vptr & 7u) != 0 || (layout.bootstrap & 3u) != 0 ||
      (layout.persistent_consumer & 3u) != 0)
    return fail("runtime_locator_abi");

  std::uintptr_t runtime_object_vptr = 0;
  std::uintptr_t runtime_slots[3]{};
  std::uint64_t runtime_prefix[2]{1, 1};
  if (!common::ReadAt(process_mem, layout.dedicated_object,
                      &runtime_object_vptr, 8) ||
      layout.dedicated_vptr < 16 ||
      !common::ReadAt(process_mem, layout.dedicated_vptr - 16,
                      runtime_prefix, sizeof(runtime_prefix)) ||
      !common::ReadAt(process_mem, layout.dedicated_vptr, runtime_slots,
                      sizeof(runtime_slots)) ||
      runtime_object_vptr != layout.dedicated_vptr ||
      runtime_prefix[0] != 0 || runtime_prefix[1] != 0 ||
      runtime_slots[0] == 0 || runtime_slots[0] != runtime_slots[1] ||
      runtime_slots[2] != layout.persistent_consumer)
    return fail("runtime_object_vtable");
  layout.fail_safe_callback = runtime_slots[0];

  const auto expected_runtime = [&](std::uintptr_t rva) { return load_bias + rva; };
  if (layout.shadow != expected_runtime(symbols.shadow_rva) ||
      layout.control != expected_runtime(symbols.control_rva) ||
      layout.evidence != expected_runtime(symbols.evidence_rva) ||
      layout.mailbox != expected_runtime(symbols.mailbox_rva) ||
      layout.dedicated_object != expected_runtime(symbols.dedicated_object_rva) ||
      layout.dedicated_vptr != expected_runtime(symbols.dedicated_vptr_rva) ||
      layout.fail_safe_callback != expected_runtime(symbols.fail_safe_rva))
    return fail("runtime_relocation_identity");

  struct Region { std::uintptr_t address; std::size_t size; } writable[] = {
      {layout.shadow, 0x908}, {layout.control, 128}, {layout.evidence, 256},
      {layout.mailbox, 192}, {layout.dedicated_object, 8},
      {layout.dedicated_vptr - 16, 40},
  };
  for (const auto& region : writable) {
    const auto* mapping = common::MappingAt(maps, region.address, region.size);
    if (!mapping || mapping->path != path || mapping->perms[0] != 'r' ||
        mapping->perms[1] != 'w')
      return fail("writable_region_mapping");
  }
  const auto* fail_safe_map = common::MappingAt(maps, layout.fail_safe_callback, 8);
  if (!fail_safe_map || fail_safe_map->path != path ||
      fail_safe_map->perms[0] != 'r' || fail_safe_map->perms[1] == 'w')
    return fail("failsafe_mapping");

  std::memcpy(layout.file_sha256, hash, sizeof(hash));
  std::memcpy(layout.build_id, kExpectedBuildId, sizeof(kExpectedBuildId));
  std::memcpy(layout.mapped_path, path.c_str(), path.size() + 1);
  *output = layout;
  if (failure_stage) *failure_stage = nullptr;
  return true;
}

}  // namespace a9tas::natural_action_lifecycle_elf_v1
