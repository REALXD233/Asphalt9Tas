#pragma once

// Hash-pinned, read-only resolver for the build-only BarrelYaw tail payload.
// The final RW PT_LOAD is deliberately split by Android's linker: its first
// pages are file-backed and its large BSS tail is a contiguous private
// anonymous mapping.  Both halves are proved against the ELF program headers;
// no adjacent anonymous memory outside that PT_LOAD is accepted.

#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "barrel_yaw_tail_payload_protocol_v1.h"
#include "fc1_payload_elf_resolver_v1.h"

namespace a9tas::barrel_yaw_tail_elf_v1 {

namespace protocol = a9tas::barrel_yaw_tail_payload_v1;

inline constexpr char kPayloadBasename[] =
    "liba9tas_barrel_yaw_tail_v1_build_only.so";
inline constexpr std::uint8_t kExpectedSha256[32] = {
    0x96, 0x5e, 0x6b, 0xa9, 0xce, 0x81, 0x9e, 0xe4,
    0xb6, 0xc1, 0xa3, 0x8b, 0xf6, 0x63, 0xf6, 0x8e,
    0x0c, 0x76, 0x84, 0x1d, 0xfa, 0x7b, 0xfa, 0xd0,
    0x92, 0x4a, 0xd6, 0x7b, 0xee, 0x67, 0x7e, 0xa9,
};

// Hash-pinned exported locator RVAs.
inline constexpr std::uintptr_t kBoundaryRva = 0x1DD0;
inline constexpr std::size_t kBoundarySize = 0x484;
inline constexpr std::uintptr_t kShadowLocatorRva = 0x50C0;
inline constexpr std::uintptr_t kControlLocatorRva = 0x50C8;
inline constexpr std::uintptr_t kTargetsLocatorRva = 0x50D0;
inline constexpr std::uintptr_t kAuditsLocatorRva = 0x50D8;
inline constexpr std::uintptr_t kEvidenceLocatorRva = 0x50E0;

// Exact current final RW PT_LOAD and the five pointed-to objects.  Program
// headers are still parsed and must equal these values after the complete file
// hash succeeds; these constants make any silent layout drift fail closed.
inline constexpr std::uint64_t kFinalRwOffset = 0x1F40;
inline constexpr std::uintptr_t kFinalRwVaddr = 0x4F40;
inline constexpr std::uint64_t kFinalRwFileSize = 0x380;
inline constexpr std::uint64_t kFinalRwMemorySize = 0x7EC90;
inline constexpr std::uintptr_t kFinalRwFileEndRva = 0x52C0;
inline constexpr std::uintptr_t kFinalRwMemoryEndRva = 0x83BD0;
inline constexpr std::uintptr_t kControlStorageRva = 0x4F40;
inline constexpr std::uintptr_t kEvidenceStorageRva = 0x5000;
inline constexpr std::uintptr_t kShadowStorageRva = 0x5100;
inline constexpr std::uintptr_t kAuditsStorageRva = 0x52C0;
inline constexpr std::uintptr_t kTargetsStorageRva = 0x75AC0;
inline constexpr std::uint64_t kPayloadFileSize = 0x3A68;
static_assert(kFinalRwVaddr + kFinalRwFileSize == kFinalRwFileEndRva);
static_assert(kFinalRwVaddr + kFinalRwMemorySize == kFinalRwMemoryEndRva);
static_assert(kControlStorageRva + sizeof(protocol::Control) ==
              kEvidenceStorageRva);
static_assert(kEvidenceStorageRva + sizeof(protocol::Evidence) <=
              kShadowStorageRva);
static_assert(kShadowStorageRva + protocol::kShadowSize ==
              kAuditsStorageRva);
static_assert(kAuditsStorageRva +
                  protocol::kMaximumTransactions *
                      sizeof(protocol::TransactionAudit) ==
              kTargetsStorageRva);
static_assert(kTargetsStorageRva +
                  protocol::kMaximumFrames * sizeof(protocol::FrameTarget) <=
              kFinalRwMemoryEndRva);

struct Mapping {
  std::uintptr_t begin{};
  std::uintptr_t end{};
  std::uint64_t offset{};
  std::uint32_t dev_major{};
  std::uint32_t dev_minor{};
  std::uint64_t inode{};
  char perms[5]{};
  std::string path;
};

struct FileIdentity {
  std::uint32_t dev_major{};
  std::uint32_t dev_minor{};
  std::uint64_t inode{};
  std::string path;
};

struct LoadPlan {
  std::vector<Elf64_Phdr> loads;
  Elf64_Phdr final_rw{};
  std::uintptr_t file_page_begin_rva{};
  std::uintptr_t file_page_end_rva{};
  std::uintptr_t logical_end_rva{};
  std::uintptr_t memory_page_end_rva{};
};

struct Layout {
  std::uintptr_t load_bias{};
  std::uintptr_t boundary{};
  std::uintptr_t shadow{};
  std::uintptr_t control{};
  std::uintptr_t targets{};
  std::uintptr_t audits{};
  std::uintptr_t evidence{};
  std::uint8_t file_sha256[32]{};
  char mapped_path[1024]{};
};

namespace detail {

using a9tas::fc1_payload_elf_v1::detail::HashFile;
using a9tas::fc1_payload_elf_v1::detail::ReadAt;

inline constexpr std::uint64_t PageDown(std::uint64_t value) {
  return value & ~std::uint64_t{0xFFF};
}

inline bool PageUp(std::uint64_t value, std::uint64_t* output) {
  if (output == nullptr || value > UINT64_MAX - 0xFFFu) return false;
  *output = (value + 0xFFFu) & ~std::uint64_t{0xFFF};
  return true;
}

inline bool Add(std::uintptr_t base, std::uintptr_t rva,
                std::uintptr_t* output) {
  if (output == nullptr || base > UINTPTR_MAX - rva) return false;
  *output = base + rva;
  return true;
}

inline bool EndsWithPayload(const std::string& path) {
  const std::size_t length = sizeof(kPayloadBasename) - 1;
  if (path.size() < length) return false;
  const std::size_t offset = path.size() - length;
  return path.compare(offset, length, kPayloadBasename) == 0 &&
         (offset == 0 || path[offset - 1] == '/');
}

inline bool ValidPermissions(const char perms[5]) {
  return perms != nullptr && (perms[0] == 'r' || perms[0] == '-') &&
         (perms[1] == 'w' || perms[1] == '-') &&
         (perms[2] == 'x' || perms[2] == '-') &&
         (perms[3] == 'p' || perms[3] == 's') && perms[4] == '\0';
}

inline bool ParseMappingLine(const char* line, Mapping* output) {
  if (line == nullptr || output == nullptr) return false;
  unsigned long long begin = 0, end = 0, offset = 0, inode = 0;
  unsigned int dev_major = 0, dev_minor = 0;
  char perms[5]{};
  char path[1024]{};
  const int fields = std::sscanf(
      line, "%llx-%llx %4s %llx %x:%x %llu %1023[^\n]", &begin, &end,
      perms, &offset, &dev_major, &dev_minor, &inode, path);
  if ((fields != 7 && fields != 8) || begin >= end ||
      begin > UINTPTR_MAX || end > UINTPTR_MAX || !ValidPermissions(perms))
    return false;
  std::string clean = fields == 8 ? std::string(path) : std::string();
  while (!clean.empty() && clean.front() == ' ') clean.erase(0, 1);
  Mapping result{static_cast<std::uintptr_t>(begin),
                 static_cast<std::uintptr_t>(end), offset,
                 static_cast<std::uint32_t>(dev_major),
                 static_cast<std::uint32_t>(dev_minor), inode, {}, clean};
  std::memcpy(result.perms, perms, sizeof(result.perms));
  *output = std::move(result);
  return true;
}

inline bool SameFile(const Mapping& mapping, const FileIdentity& identity) {
  return mapping.dev_major == identity.dev_major &&
         mapping.dev_minor == identity.dev_minor &&
         mapping.inode == identity.inode && mapping.path == identity.path;
}

inline bool PrivatePermissions(const Mapping& mapping, bool writable,
                               bool executable) {
  return mapping.perms[0] == 'r' &&
         mapping.perms[1] == (writable ? 'w' : '-') &&
         mapping.perms[2] == (executable ? 'x' : '-') &&
         mapping.perms[3] == 'p';
}

inline bool PrivateAnonymousRw(const Mapping& mapping) {
  const bool accepted_name =
      mapping.path.empty() || mapping.path == "[anon:.bss]";
  return accepted_name && mapping.dev_major == 0 &&
         mapping.dev_minor == 0 && mapping.inode == 0 &&
         mapping.offset == 0 && PrivatePermissions(mapping, true, false);
}

inline bool SameIdentity(const FileIdentity& left,
                         const FileIdentity& right) {
  return left.dev_major == right.dev_major &&
         left.dev_minor == right.dev_minor && left.inode == right.inode &&
         left.path == right.path;
}

inline bool ParseProcessStartTime(const char* line, std::uint64_t* output) {
  if (line == nullptr || output == nullptr) return false;
  const char* cursor = std::strrchr(line, ')');
  if (cursor == nullptr) return false;
  ++cursor;
  for (unsigned field = 3; field <= 22; ++field) {
    while (*cursor == ' ') ++cursor;
    if (*cursor == '\0' || *cursor == '\n') return false;
    const char* token = cursor;
    while (*cursor != '\0' && *cursor != '\n' && *cursor != ' ') ++cursor;
    if (field != 22) continue;
    if (token == cursor) return false;
    std::uint64_t value = 0;
    for (const char* digit = token; digit != cursor; ++digit) {
      if (*digit < '0' || *digit > '9' ||
          value > (UINT64_MAX - static_cast<unsigned>(*digit - '0')) / 10)
        return false;
      value = value * 10 + static_cast<unsigned>(*digit - '0');
    }
    if (value == 0) return false;
    *output = value;
    return true;
  }
  return false;
}

inline bool ReadProcessStartTime(pid_t pid, std::uint64_t* output) {
  if (pid <= 0 || output == nullptr) return false;
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
  FILE* file = std::fopen(path, "re");
  if (file == nullptr) return false;
  char line[4096]{};
  const bool read_ok = std::fgets(line, sizeof(line), file) != nullptr;
  const bool complete = read_ok && std::strchr(line, '\n') != nullptr;
  const bool close_ok = std::fclose(file) == 0;
  return complete && close_ok && ParseProcessStartTime(line, output);
}

inline bool SelectPayloadFile(const std::vector<Mapping>& maps,
                              FileIdentity* output) {
  if (output == nullptr) return false;
  FileIdentity selected{};
  std::uint32_t matches = 0;
  for (const auto& mapping : maps) {
    const bool mentions_payload =
        mapping.path.find(kPayloadBasename) != std::string::npos;
    if (!EndsWithPayload(mapping.path)) {
      if (mentions_payload) return false;
      continue;
    }
    if (mapping.path.empty() || mapping.path.front() != '/' ||
        mapping.path.size() >= sizeof(Layout::mapped_path) ||
        mapping.inode == 0 ||
        (mapping.dev_major == 0 && mapping.dev_minor == 0))
      return false;
    if (matches == 0) {
      selected = {mapping.dev_major, mapping.dev_minor, mapping.inode,
                  mapping.path};
    } else if (!SameFile(mapping, selected)) {
      return false;
    }
    ++matches;
  }
  if (matches < 2) return false;
  *output = std::move(selected);
  return true;
}

inline bool ReadMappings(pid_t pid, std::vector<Mapping>* output,
                         FileIdentity* identity) {
  if (pid <= 0 || output == nullptr || identity == nullptr) return false;
  char maps_path[64]{};
  std::snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps",
                static_cast<int>(pid));
  FILE* file = std::fopen(maps_path, "re");
  if (!file) return false;
  std::vector<Mapping> maps;
  char line[2048]{};
  while (std::fgets(line, sizeof(line), file)) {
    Mapping mapping{};
    if (!ParseMappingLine(line, &mapping)) {
      std::fclose(file);
      return false;
    }
    if (!maps.empty() && mapping.begin < maps.back().end) {
      std::fclose(file);
      return false;
    }
    maps.push_back(std::move(mapping));
  }
  std::fclose(file);
  FileIdentity selected{};
  if (!SelectPayloadFile(maps, &selected)) return false;
  *output = std::move(maps);
  *identity = std::move(selected);
  return true;
}

inline const Mapping* At(const std::vector<Mapping>& maps,
                         std::uintptr_t address, std::size_t size) {
  if (size == 0 || address > UINTPTR_MAX - size) return nullptr;
  const std::uintptr_t end = address + size;
  for (const auto& mapping : maps)
    if (address >= mapping.begin && end <= mapping.end) return &mapping;
  return nullptr;
}

inline bool InFileBytes(const Elf64_Phdr& load, std::uintptr_t rva,
                        std::size_t size) {
  if (rva < load.p_vaddr) return false;
  const std::uint64_t relative = rva - load.p_vaddr;
  return relative <= load.p_filesz && size <= load.p_filesz - relative;
}

inline bool BuildLoadPlan(const Elf64_Ehdr& header,
                          const std::vector<Elf64_Phdr>& programs,
                          std::uint64_t file_size, LoadPlan* output) {
  if (output == nullptr || std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
      header.e_ident[EI_CLASS] != ELFCLASS64 ||
      header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_type != ET_DYN ||
      header.e_ident[EI_VERSION] != EV_CURRENT ||
      header.e_machine != EM_AARCH64 || header.e_version != EV_CURRENT ||
      header.e_ehsize != sizeof(Elf64_Ehdr) || file_size != kPayloadFileSize ||
      header.e_phentsize != sizeof(Elf64_Phdr) || header.e_phnum == 0 ||
      header.e_phnum > 64 || programs.size() != header.e_phnum ||
      header.e_phoff > file_size ||
      static_cast<std::uint64_t>(header.e_phnum) * sizeof(Elf64_Phdr) >
          file_size - header.e_phoff)
    return false;

  LoadPlan result{};
  std::uint64_t previous_end = 0;
  bool have_previous = false;
  for (const auto& program : programs) {
    if (program.p_type != PT_LOAD) continue;
    if (program.p_filesz == 0 || program.p_filesz > program.p_memsz ||
        program.p_align != 0x1000 || (program.p_flags & PF_R) == 0 ||
        (program.p_flags & (PF_W | PF_X)) == (PF_W | PF_X) ||
        program.p_offset > file_size ||
        program.p_filesz > file_size - program.p_offset ||
        (program.p_offset & 0xFFFu) != (program.p_vaddr & 0xFFFu) ||
        program.p_vaddr > UINTPTR_MAX ||
        program.p_memsz > UINTPTR_MAX - program.p_vaddr)
      return false;
    const std::uint64_t end = program.p_vaddr + program.p_memsz;
    if (have_previous && program.p_vaddr < previous_end) return false;
    previous_end = end;
    have_previous = true;
    result.loads.push_back(program);
  }
  if (result.loads.size() != 4) return false;
  struct ExpectedLoad {
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t vaddr;
    std::uint64_t filesz;
    std::uint64_t memsz;
  };
  constexpr ExpectedLoad expected_loads[] = {
      {PF_R, 0x0000, 0x0000, 0x0D64, 0x0D64},
      {PF_R | PF_X, 0x0D70, 0x1D70, 0x0F90, 0x0F90},
      {PF_R | PF_W, 0x1D00, 0x3D00, 0x0228, 0x0300},
      {PF_R | PF_W, kFinalRwOffset, kFinalRwVaddr,
       kFinalRwFileSize, kFinalRwMemorySize},
  };
  for (std::size_t index = 0; index < std::size(expected_loads); ++index) {
    const auto& observed = result.loads[index];
    const auto& expected = expected_loads[index];
    if (observed.p_flags != expected.flags ||
        observed.p_offset != expected.offset ||
        observed.p_vaddr != expected.vaddr ||
        observed.p_paddr != expected.vaddr ||
        observed.p_filesz != expected.filesz ||
        observed.p_memsz != expected.memsz)
      return false;
  }
  result.final_rw = result.loads.back();
  const Elf64_Phdr& rw = result.final_rw;
  if (rw.p_flags != (PF_R | PF_W) || rw.p_offset != kFinalRwOffset ||
      rw.p_vaddr != kFinalRwVaddr || rw.p_filesz != kFinalRwFileSize ||
      rw.p_memsz != kFinalRwMemorySize ||
      rw.p_vaddr + rw.p_filesz != kFinalRwFileEndRva ||
      rw.p_vaddr + rw.p_memsz != kFinalRwMemoryEndRva)
    return false;

  bool boundary_in_rx = false;
  for (const auto& load : result.loads)
    if ((load.p_flags & (PF_R | PF_X)) == (PF_R | PF_X) &&
        (load.p_flags & PF_W) == 0 &&
        InFileBytes(load, kBoundaryRva, kBoundarySize))
      boundary_in_rx = true;
  if (!boundary_in_rx) return false;
  constexpr std::uintptr_t locators[] = {
      kShadowLocatorRva, kControlLocatorRva, kTargetsLocatorRva,
      kAuditsLocatorRva, kEvidenceLocatorRva,
  };
  for (const std::uintptr_t locator : locators)
    if (!InFileBytes(rw, locator, sizeof(std::uintptr_t))) return false;

  std::uint64_t file_page_end = 0;
  std::uint64_t memory_page_end = 0;
  if (!PageUp(kFinalRwFileEndRva, &file_page_end) ||
      !PageUp(kFinalRwMemoryEndRva, &memory_page_end) ||
      file_page_end >= memory_page_end || memory_page_end > UINTPTR_MAX)
    return false;
  result.file_page_begin_rva =
      static_cast<std::uintptr_t>(PageDown(rw.p_vaddr));
  result.file_page_end_rva = static_cast<std::uintptr_t>(file_page_end);
  result.logical_end_rva = kFinalRwMemoryEndRva;
  result.memory_page_end_rva =
      static_cast<std::uintptr_t>(memory_page_end);
  *output = std::move(result);
  return true;
}

inline bool ReadLoadPlan(int file, std::uint64_t file_size,
                         LoadPlan* output) {
  if (file < 0 || output == nullptr) return false;
  Elf64_Ehdr header{};
  if (!ReadAt(file, 0, &header, sizeof(header)) || header.e_phnum == 0 ||
      header.e_phnum > 64 || header.e_phentsize != sizeof(Elf64_Phdr) ||
      header.e_phoff > file_size ||
      static_cast<std::uint64_t>(header.e_phnum) * sizeof(Elf64_Phdr) >
          file_size - header.e_phoff)
    return false;
  std::vector<Elf64_Phdr> programs(header.e_phnum);
  if (!ReadAt(file, header.e_phoff, programs.data(),
              programs.size() * sizeof(programs.front())))
    return false;
  return BuildLoadPlan(header, programs, file_size, output);
}

inline bool FileOffsetAt(const Mapping& mapping, std::uintptr_t address,
                         std::uint64_t* output) {
  if (output == nullptr || address < mapping.begin || address >= mapping.end ||
      address - mapping.begin > UINT64_MAX - mapping.offset)
    return false;
  *output = mapping.offset + (address - mapping.begin);
  return true;
}

inline bool DeriveAndValidateLoadBias(const std::vector<Mapping>& maps,
                                      const FileIdentity& identity,
                                      const LoadPlan& plan,
                                      std::uintptr_t* output) {
  if (output == nullptr || plan.loads.empty()) return false;
  std::uintptr_t bias = UINTPTR_MAX;
  for (const auto& mapping : maps) {
    if (!SameFile(mapping, identity) || mapping.offset != 0) continue;
    if (mapping.begin < PageDown(plan.loads.front().p_vaddr)) continue;
    const std::uintptr_t candidate =
        mapping.begin - static_cast<std::uintptr_t>(
                            PageDown(plan.loads.front().p_vaddr));
    if ((candidate & 0xFFFu) == 0) bias = std::min(bias, candidate);
  }
  if (bias == UINTPTR_MAX) return false;

  for (const auto& load : plan.loads) {
    const std::uintptr_t page_vaddr =
        static_cast<std::uintptr_t>(PageDown(load.p_vaddr));
    const std::uint64_t page_offset = PageDown(load.p_offset);
    std::uintptr_t expected = 0;
    if (!Add(bias, page_vaddr, &expected)) return false;
    const Mapping* mapping = At(maps, expected, 1);
    std::uint64_t observed_offset = 0;
    if (!mapping || !SameFile(*mapping, identity) ||
        mapping->perms[0] != 'r' || mapping->perms[3] != 'p' ||
        !FileOffsetAt(*mapping, expected, &observed_offset) ||
        observed_offset != page_offset)
      return false;
  }

  // Every mapping carrying this exact dev/inode/path must be one of the
  // hash-pinned PT_LOAD file-page intervals at this single bias.
  for (const auto& mapping : maps) {
    if (!SameFile(mapping, identity)) continue;
    bool covered = false;
    for (const auto& load : plan.loads) {
      std::uint64_t page_end_rva = 0;
      if (!PageUp(load.p_vaddr + load.p_filesz, &page_end_rva)) return false;
      std::uintptr_t begin = 0, end = 0;
      if (!Add(bias, static_cast<std::uintptr_t>(PageDown(load.p_vaddr)),
               &begin) ||
          !Add(bias, static_cast<std::uintptr_t>(page_end_rva), &end))
        return false;
      if (mapping.begin < begin || mapping.end > end) continue;
      std::uint64_t observed_offset = 0;
      if (!FileOffsetAt(mapping, mapping.begin, &observed_offset) ||
          observed_offset != PageDown(load.p_offset) +
                                 (mapping.begin - begin))
        continue;
      covered = true;
      break;
    }
    if (!covered) return false;
  }
  *output = bias;
  return true;
}

inline bool ValidateFinalRwSplit(const std::vector<Mapping>& maps,
                                 const FileIdentity& identity,
                                 std::uintptr_t bias,
                                 const LoadPlan& plan) {
  std::uintptr_t file_begin = 0, file_end = 0, memory_page_end = 0;
  if (!Add(bias, plan.file_page_begin_rva, &file_begin) ||
      !Add(bias, plan.file_page_end_rva, &file_end) ||
      !Add(bias, plan.memory_page_end_rva, &memory_page_end))
    return false;
  std::uintptr_t cursor = file_begin;
  const std::uint64_t page_offset = PageDown(plan.final_rw.p_offset);
  while (cursor < file_end) {
    const Mapping* mapping = At(maps, cursor, 1);
    std::uint64_t observed_offset = 0;
    if (!mapping || !SameFile(*mapping, identity) ||
        !PrivatePermissions(*mapping, true, false) ||
        mapping->end > file_end ||
        !FileOffsetAt(*mapping, cursor, &observed_offset) ||
        observed_offset != page_offset + (cursor - file_begin))
      return false;
    cursor = mapping->end;
  }
  if (cursor != file_end) return false;
  std::uint32_t anonymous_mappings = 0;
  while (cursor < memory_page_end) {
    const Mapping* mapping = At(maps, cursor, 1);
    if (!mapping || mapping->begin != cursor ||
        !PrivateAnonymousRw(*mapping) || mapping->end > memory_page_end)
      return false;
    cursor = mapping->end;
    ++anonymous_mappings;
  }
  return cursor == memory_page_end && anonymous_mappings != 0;
}

inline bool WritableRange(const std::vector<Mapping>& maps,
                          std::uintptr_t address, std::size_t size,
                          const FileIdentity& identity,
                          std::uintptr_t bias, const LoadPlan& plan) {
  if (size == 0 || address > UINTPTR_MAX - size) return false;
  std::uintptr_t logical_begin = 0, logical_end = 0, file_page_end = 0;
  if (!Add(bias, static_cast<std::uintptr_t>(plan.final_rw.p_vaddr),
           &logical_begin) ||
      !Add(bias, plan.logical_end_rva, &logical_end) ||
      !Add(bias, plan.file_page_end_rva, &file_page_end))
    return false;
  const std::uintptr_t end = address + size;
  if (address < logical_begin || end > logical_end) return false;
  std::uintptr_t cursor = address;
  while (cursor < end) {
    const Mapping* mapping = At(maps, cursor, 1);
    if (!mapping || !PrivatePermissions(*mapping, true, false)) return false;
    if (cursor < file_page_end) {
      if (!SameFile(*mapping, identity) || mapping->end > file_page_end)
        return false;
    } else if (!PrivateAnonymousRw(*mapping)) {
      return false;
    }
    cursor = std::min(end, mapping->end);
  }
  return cursor == end;
}

inline bool ValidateStorageLayout(const std::uintptr_t storage[5],
                                  std::uintptr_t bias,
                                  const LoadPlan& plan) {
  if (storage == nullptr) return false;
  constexpr std::uintptr_t expected_rvas[5] = {
      kShadowStorageRva, kControlStorageRva, kTargetsStorageRva,
      kAuditsStorageRva, kEvidenceStorageRva,
  };
  for (std::size_t index = 0; index < std::size(expected_rvas); ++index) {
    std::uintptr_t expected = 0;
    if (!Add(bias, expected_rvas[index], &expected) ||
        storage[index] != expected || (storage[index] & 63u) != 0)
      return false;
  }
  struct Range {
    std::uintptr_t begin;
    std::size_t size;
  } ranges[] = {
      {storage[1], sizeof(protocol::Control)},
      {storage[4], sizeof(protocol::Evidence)},
      {storage[0], protocol::kShadowSize},
      {storage[3], protocol::kMaximumTransactions *
                       sizeof(protocol::TransactionAudit)},
      {storage[2], protocol::kMaximumFrames *
                       sizeof(protocol::FrameTarget)},
  };
  std::uintptr_t rw_begin = 0, rw_end = 0;
  if (!Add(bias, static_cast<std::uintptr_t>(plan.final_rw.p_vaddr),
           &rw_begin) ||
      !Add(bias, plan.logical_end_rva, &rw_end))
    return false;
  std::uintptr_t previous_end = rw_begin;
  for (const auto& range : ranges) {
    if (range.begin < previous_end || range.begin > UINTPTR_MAX - range.size ||
        range.begin + range.size > rw_end)
      return false;
    previous_end = range.begin + range.size;
  }
  return true;
}

inline bool FileBackedRwRange(const std::vector<Mapping>& maps,
                              std::uintptr_t address, std::size_t size,
                              const FileIdentity& identity,
                              std::uintptr_t bias, const LoadPlan& plan) {
  if (size == 0 || address > UINTPTR_MAX - size) return false;
  std::uintptr_t begin = 0, end = 0;
  if (!Add(bias, static_cast<std::uintptr_t>(plan.final_rw.p_vaddr), &begin) ||
      !Add(bias, kFinalRwFileEndRva, &end) || address < begin ||
      address + size > end)
    return false;
  const Mapping* mapping = At(maps, address, size);
  return mapping && SameFile(*mapping, identity) &&
         PrivatePermissions(*mapping, true, false);
}

inline bool FileOffsetForRva(const LoadPlan& plan, std::uintptr_t rva,
                             std::size_t size, std::uint64_t* output);

inline bool ExecutableRange(const std::vector<Mapping>& maps,
                            std::uintptr_t address, std::size_t size,
                            const FileIdentity& identity,
                            std::uintptr_t bias, const LoadPlan& plan) {
  if (address < bias) return false;
  const std::uintptr_t rva = address - bias;
  bool in_rx = false;
  for (const auto& load : plan.loads)
    if ((load.p_flags & (PF_R | PF_X)) == (PF_R | PF_X) &&
        (load.p_flags & PF_W) == 0 && InFileBytes(load, rva, size))
      in_rx = true;
  const Mapping* mapping = At(maps, address, size);
  std::uint64_t expected_offset = 0;
  std::uint64_t observed_offset = 0;
  // Houdini may expose translated ARM guest text as host r--p rather than
  // r-xp. Executability is proved by the hash-pinned ELF PF_X bit; the host
  // view must remain private, readable, non-writable, and exact-file-backed.
  return in_rx && mapping && SameFile(*mapping, identity) &&
         mapping->perms[0] == 'r' && mapping->perms[1] == '-' &&
         (mapping->perms[2] == '-' || mapping->perms[2] == 'x') &&
         mapping->perms[3] == 'p' &&
         FileOffsetForRva(plan, rva, size, &expected_offset) &&
         FileOffsetAt(*mapping, address, &observed_offset) &&
         observed_offset == expected_offset;
}

inline bool FileOffsetForRva(const LoadPlan& plan, std::uintptr_t rva,
                             std::size_t size, std::uint64_t* output) {
  if (output == nullptr) return false;
  std::uint32_t matches = 0;
  std::uint64_t resolved = 0;
  for (const auto& load : plan.loads) {
    if (!InFileBytes(load, rva, size)) continue;
    const std::uint64_t relative = rva - load.p_vaddr;
    if (load.p_offset > UINT64_MAX - relative) return false;
    resolved = load.p_offset + relative;
    ++matches;
  }
  if (matches != 1) return false;
  *output = resolved;
  return true;
}

inline bool VerifyLiveFileBytes(int file, int process_mem,
                                std::uintptr_t bias,
                                const LoadPlan& plan,
                                std::uintptr_t rva, std::size_t size) {
  if (file < 0 || process_mem < 0 || size == 0 ||
      bias > UINTPTR_MAX - rva)
    return false;
  std::uint64_t file_offset = 0;
  if (!FileOffsetForRva(plan, rva, size, &file_offset)) return false;
  std::vector<std::uint8_t> disk(size);
  std::vector<std::uint8_t> live(size);
  return ReadAt(file, file_offset, disk.data(), disk.size()) &&
         ReadAt(process_mem, bias + rva, live.data(), live.size()) &&
         std::memcmp(disk.data(), live.data(), size) == 0;
}

inline bool ResolveLocatorStorage(const std::uintptr_t observed[5],
                                  std::uintptr_t bias,
                                  std::uintptr_t storage[5]) {
  if (observed == nullptr || storage == nullptr) return false;
  constexpr std::uintptr_t expected_rvas[5] = {
      kShadowStorageRva, kControlStorageRva, kTargetsStorageRva,
      kAuditsStorageRva, kEvidenceStorageRva,
  };
  bool all_zero = true;
  bool all_relocated = true;
  for (std::size_t index = 0; index < std::size(expected_rvas); ++index) {
    std::uintptr_t expected = 0;
    if (!Add(bias, expected_rvas[index], &expected)) return false;
    storage[index] = expected;
    all_zero = all_zero && observed[index] == 0;
    all_relocated = all_relocated && observed[index] == expected;
  }
  // Native linker view: all five RELATIVE relocations are applied. Houdini
  // host view: all five locator words remain zero. Mixed/third states fail.
  return all_zero || all_relocated;
}

class ScopedFd {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) close(fd_);
  }
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

inline bool SameMapping(const Mapping& left, const Mapping& right) {
  return left.begin == right.begin && left.end == right.end &&
         left.offset == right.offset && left.dev_major == right.dev_major &&
         left.dev_minor == right.dev_minor && left.inode == right.inode &&
         std::memcmp(left.perms, right.perms, sizeof(left.perms)) == 0 &&
         left.path == right.path;
}

inline bool SamePayloadCluster(const std::vector<Mapping>& before,
                               const std::vector<Mapping>& after,
                               std::uintptr_t bias,
                               const LoadPlan& plan) {
  std::uintptr_t cluster_end = 0;
  if (!Add(bias, plan.memory_page_end_rva, &cluster_end)) return false;
  std::vector<const Mapping*> before_cluster;
  std::vector<const Mapping*> after_cluster;
  for (const auto& mapping : before)
    if (mapping.end > bias && mapping.begin < cluster_end)
      before_cluster.push_back(&mapping);
  for (const auto& mapping : after)
    if (mapping.end > bias && mapping.begin < cluster_end)
      after_cluster.push_back(&mapping);
  if (before_cluster.size() != after_cluster.size() ||
      before_cluster.empty())
    return false;
  for (std::size_t index = 0; index < before_cluster.size(); ++index)
    if (!SameMapping(*before_cluster[index], *after_cluster[index]))
      return false;
  return true;
}

}  // namespace detail

inline bool Resolve(pid_t pid, int process_mem, Layout* output) {
  if (pid <= 0 || process_mem < 0 || output == nullptr) return false;
  std::uint64_t start_time_before = 0;
  if (!detail::ReadProcessStartTime(pid, &start_time_before)) return false;
  std::vector<Mapping> maps;
  FileIdentity identity{};
  if (!detail::ReadMappings(pid, &maps, &identity)) return false;

  detail::ScopedFd file(open(identity.path.c_str(), O_RDONLY | O_CLOEXEC));
  if (file.get() < 0) return false;
  struct stat info {};
  std::uint8_t hash[32]{};
  LoadPlan plan{};
  const bool file_ok =
      fstat(file.get(), &info) == 0 && S_ISREG(info.st_mode) &&
      info.st_size >= 4096 && info.st_size <= 8 * 1024 * 1024 &&
      static_cast<std::uint64_t>(info.st_ino) == identity.inode &&
      static_cast<std::uint32_t>(major(info.st_dev)) == identity.dev_major &&
      static_cast<std::uint32_t>(minor(info.st_dev)) == identity.dev_minor &&
      detail::HashFile(file.get(), hash) &&
      std::memcmp(hash, kExpectedSha256, sizeof(hash)) == 0 &&
      detail::ReadLoadPlan(file.get(), static_cast<std::uint64_t>(info.st_size),
                           &plan);
  if (!file_ok) return false;

  std::uintptr_t bias = 0;
  if (!detail::DeriveAndValidateLoadBias(maps, identity, plan, &bias) ||
      !detail::ValidateFinalRwSplit(maps, identity, bias, plan))
    return false;

  constexpr std::uintptr_t rvas[] = {
      kBoundaryRva,       kShadowLocatorRva, kControlLocatorRva,
      kTargetsLocatorRva, kAuditsLocatorRva, kEvidenceLocatorRva,
  };
  std::uintptr_t addresses[6]{};
  for (std::size_t index = 0; index < std::size(rvas); ++index)
    if (!detail::Add(bias, rvas[index], &addresses[index])) return false;
  if (!detail::ExecutableRange(maps, addresses[0], kBoundarySize, identity,
                               bias, plan) ||
      !detail::VerifyLiveFileBytes(file.get(), process_mem, bias, plan,
                                   kBoundaryRva, kBoundarySize))
    return false;
  for (std::size_t index = 1; index < std::size(addresses); ++index)
    if (!detail::FileBackedRwRange(maps, addresses[index],
                                   sizeof(std::uintptr_t), identity, bias,
                                   plan))
      return false;

  std::uintptr_t observed_locators[5]{};
  std::uintptr_t storage[5]{};
  if (!detail::ReadAt(process_mem, addresses[1], observed_locators,
                      sizeof(observed_locators)) ||
      !detail::ResolveLocatorStorage(observed_locators, bias, storage))
    return false;
  if (!detail::ValidateStorageLayout(storage, bias, plan) ||
      !detail::WritableRange(maps, storage[0], protocol::kShadowSize,
                             identity, bias, plan) ||
      !detail::WritableRange(maps, storage[1], sizeof(protocol::Control),
                             identity, bias, plan) ||
      !detail::WritableRange(maps, storage[2],
                             protocol::kMaximumFrames *
                                 sizeof(protocol::FrameTarget),
                             identity, bias, plan) ||
      !detail::WritableRange(maps, storage[3],
                             protocol::kMaximumTransactions *
                                 sizeof(protocol::TransactionAudit),
                             identity, bias, plan) ||
      !detail::WritableRange(maps, storage[4], sizeof(protocol::Evidence),
                             identity, bias, plan))
    return false;

  protocol::Control control{};
  protocol::Evidence evidence{};
  if (!detail::ReadAt(process_mem, storage[1], &control, sizeof(control)) ||
      !detail::ReadAt(process_mem, storage[4], &evidence, sizeof(evidence)) ||
      std::memcmp(control.magic, protocol::kControlMagic, 8) != 0 ||
      control.version != protocol::kProtocolVersion ||
      control.size != sizeof(protocol::Control) ||
      std::memcmp(evidence.magic, protocol::kEvidenceMagic, 8) != 0 ||
      evidence.version != protocol::kProtocolVersion ||
      evidence.size != sizeof(protocol::Evidence))
    return false;

  std::uintptr_t observed_locators_after[5]{};
  std::uint8_t final_hash[32]{};
  struct stat final_info {};
  if (!detail::VerifyLiveFileBytes(file.get(), process_mem, bias, plan,
                                   kBoundaryRva, kBoundarySize) ||
      !detail::ReadAt(process_mem, addresses[1], observed_locators_after,
                      sizeof(observed_locators_after)) ||
      std::memcmp(observed_locators, observed_locators_after,
                  sizeof(observed_locators)) != 0 ||
      fstat(file.get(), &final_info) != 0 || !S_ISREG(final_info.st_mode) ||
      final_info.st_size != info.st_size || final_info.st_ino != info.st_ino ||
      final_info.st_dev != info.st_dev ||
      !detail::HashFile(file.get(), final_hash) ||
      std::memcmp(final_hash, kExpectedSha256, sizeof(final_hash)) != 0)
    return false;

  std::uint64_t start_time_after = 0;
  std::vector<Mapping> maps_after;
  FileIdentity identity_after{};
  std::uintptr_t bias_after = 0;
  if (!detail::ReadMappings(pid, &maps_after, &identity_after) ||
      !detail::ReadProcessStartTime(pid, &start_time_after) ||
      start_time_after != start_time_before ||
      !detail::SameIdentity(identity, identity_after) ||
      !detail::DeriveAndValidateLoadBias(maps_after, identity_after, plan,
                                         &bias_after) ||
      bias_after != bias ||
      !detail::ValidateFinalRwSplit(maps_after, identity_after, bias_after,
                                    plan) ||
      !detail::SamePayloadCluster(maps, maps_after, bias, plan))
    return false;

  Layout layout{};
  layout.load_bias = bias;
  layout.boundary = addresses[0];
  layout.shadow = storage[0];
  layout.control = storage[1];
  layout.targets = storage[2];
  layout.audits = storage[3];
  layout.evidence = storage[4];
  std::memcpy(layout.file_sha256, hash, sizeof(hash));
  std::snprintf(layout.mapped_path, sizeof(layout.mapped_path), "%s",
                identity.path.c_str());
  *output = layout;
  return true;
}

}  // namespace a9tas::barrel_yaw_tail_elf_v1
