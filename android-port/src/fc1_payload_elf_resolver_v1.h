#pragma once

// Read-only resolver for the preloaded FC-0 ARM64 payload.  It hashes and
// parses the mapped ELF file, resolves dynamic symbols, then dereferences only
// payload-owned data exports through /proc/PID/mem.  It never invokes guest
// code and never writes target memory.

#include <elf.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace a9tas::fc1_payload_elf_v1 {

constexpr char kPayloadBasename[] =
    "liba9tas_frame_callback_bootstrap_v1_build_only.so";
constexpr std::uint8_t kExpectedSha256[32] = {
    0x12, 0x23, 0xa5, 0x10, 0x4a, 0x05, 0x68, 0x8c,
    0x8d, 0xda, 0x4e, 0xc7, 0xf5, 0x45, 0x4c, 0xc4,
    0x2f, 0xe2, 0x3a, 0x6f, 0x21, 0x88, 0x6f, 0xcc,
    0xf5, 0x28, 0xa3, 0x2a, 0x21, 0x63, 0x32, 0x98,
};

struct Layout {
  std::uintptr_t load_bias{};
  std::uintptr_t wrapper{};
  std::uintptr_t shadow{};
  std::uintptr_t control{};
  std::uintptr_t evidence{};
  std::uint64_t control_size{};
  std::uint64_t evidence_size{};
  std::uint8_t file_sha256[32]{};
  char mapped_path[1024]{};
};

struct FileMapping {
  std::uintptr_t begin{};
  std::uintptr_t end{};
  std::uint64_t file_offset{};
  char perms[5]{};
  std::string path;
};

namespace detail {

constexpr std::uint32_t RotateRight(std::uint32_t value,
                                    std::uint32_t count) {
  return (value >> count) | (value << (32u - count));
}

class Sha256 {
 public:
  Sha256()
      : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

  void Update(const std::uint8_t* data, std::size_t size) {
    total_bytes_ += size;
    while (size != 0) {
      const std::size_t take = std::min(size, block_.size() - used_);
      std::memcpy(block_.data() + used_, data, take);
      used_ += take;
      data += take;
      size -= take;
      if (used_ == block_.size()) {
        Transform(block_.data());
        used_ = 0;
      }
    }
  }

  void Final(std::uint8_t output[32]) {
    const std::uint64_t bits = total_bytes_ * 8u;
    block_[used_++] = 0x80;
    if (used_ > 56) {
      std::memset(block_.data() + used_, 0, block_.size() - used_);
      Transform(block_.data());
      used_ = 0;
    }
    std::memset(block_.data() + used_, 0, 56 - used_);
    for (int index = 0; index < 8; ++index)
      block_[56 + index] =
          static_cast<std::uint8_t>(bits >> (56 - index * 8));
    Transform(block_.data());
    for (std::size_t index = 0; index < state_.size(); ++index) {
      output[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24);
      output[index * 4 + 1] =
          static_cast<std::uint8_t>(state_[index] >> 16);
      output[index * 4 + 2] =
          static_cast<std::uint8_t>(state_[index] >> 8);
      output[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
    }
  }

 private:
  void Transform(const std::uint8_t block[64]) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };
    std::uint32_t words[64]{};
    for (int index = 0; index < 16; ++index) {
      words[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                     (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                     (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                     static_cast<std::uint32_t>(block[index * 4 + 3]);
    }
    for (int index = 16; index < 64; ++index) {
      const std::uint32_t s0 = RotateRight(words[index - 15], 7) ^
                               RotateRight(words[index - 15], 18) ^
                               (words[index - 15] >> 3);
      const std::uint32_t s1 = RotateRight(words[index - 2], 17) ^
                               RotateRight(words[index - 2], 19) ^
                               (words[index - 2] >> 10);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int index = 0; index < 64; ++index) {
      const std::uint32_t s1 =
          RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
      const std::uint32_t choose = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = h + s1 + choose + k[index] + words[index];
      const std::uint32_t s0 =
          RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = s0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> block_{};
  std::size_t used_{};
  std::uint64_t total_bytes_{};
};

inline bool ReadAt(int fd, std::uint64_t offset, void* output,
                   std::size_t size) {
  auto* bytes = static_cast<std::uint8_t*>(output);
  std::size_t done = 0;
  while (done < size) {
    const ssize_t count =
        pread(fd, bytes + done, size - done,
              static_cast<off_t>(offset + done));
    if (count <= 0) return false;
    done += static_cast<std::size_t>(count);
  }
  return true;
}

inline bool HashFile(int fd, std::uint8_t output[32]) {
  if (lseek(fd, 0, SEEK_SET) < 0) return false;
  Sha256 hash;
  std::array<std::uint8_t, 65536> buffer{};
  for (;;) {
    const ssize_t count = read(fd, buffer.data(), buffer.size());
    if (count < 0) return false;
    if (count == 0) break;
    hash.Update(buffer.data(), static_cast<std::size_t>(count));
  }
  hash.Final(output);
  return true;
}

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
  // Houdini maps ARM64 guest .text read-only in the host process and executes
  // translated code from its own cache.  Host 'x' is therefore neither
  // expected nor authoritative; executable provenance is checked against the
  // ELF PT_LOAD flags below.
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

inline const FileMapping* MappingAt(const std::vector<FileMapping>& maps,
                                    std::uintptr_t address,
                                    std::size_t size) {
  if (size == 0 || address > UINTPTR_MAX - size) return nullptr;
  const std::uintptr_t end = address + size;
  for (const auto& mapping : maps)
    if (address >= mapping.begin && end <= mapping.end) return &mapping;
  return nullptr;
}

constexpr std::uint64_t PageDown(std::uint64_t value) {
  return value & ~std::uint64_t{0xFFF};
}

struct SymbolResult {
  std::uintptr_t wrapper{};
  std::uintptr_t shadow_data{};
  std::uintptr_t control_data{};
  std::uintptr_t evidence_data{};
  std::uintptr_t control_size_data{};
  std::uintptr_t evidence_size_data{};
};

inline bool ResolveSymbols(int fd, std::uint64_t file_size,
                           const std::vector<FileMapping>& maps,
                           const std::string& mapped_path,
                           std::uintptr_t load_bias, SymbolResult* output) {
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

  // Prove that the selected bias describes every file-backed PT_LOAD rather
  // than merely trusting the lowest offset-zero /proc/maps line.  Android's
  // linker may map multiple segments from the same file page, so the segment
  // page offset and virtual page must both match exactly.
  std::vector<Elf64_Phdr> programs(header.e_phnum);
  if (!ReadAt(fd, header.e_phoff, programs.data(),
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
    const std::uint64_t page_offset = PageDown(program.p_offset);
    const std::uint64_t page_vaddr = PageDown(program.p_vaddr);
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
  std::vector<Elf64_Sym> symbols(
      static_cast<std::size_t>(dynsym->sh_size / sizeof(Elf64_Sym)));
  std::vector<char> strings(static_cast<std::size_t>(strings_header.sh_size));
  if (!ReadAt(fd, dynsym->sh_offset, symbols.data(), dynsym->sh_size) ||
      !ReadAt(fd, strings_header.sh_offset, strings.data(), strings.size()))
    return false;

  struct Wanted {
    const char* name;
    unsigned char type;
    std::uintptr_t* address;
    std::uint32_t matches;
  } wanted[] = {
      {"a9tas_fc0_frame_callback_passthrough_v1", STT_FUNC,
       &output->wrapper, 0},
      {"a9tas_fc0_frame_callback_shadow_storage_data_v1", STT_OBJECT,
       &output->shadow_data, 0},
      {"a9tas_fc0_frame_callback_control_storage_data_v1", STT_OBJECT,
       &output->control_data, 0},
      {"a9tas_fc0_frame_callback_evidence_storage_data_v1", STT_OBJECT,
       &output->evidence_data, 0},
      {"a9tas_fc0_frame_callback_control_size_data_v1", STT_OBJECT,
       &output->control_size_data, 0},
      {"a9tas_fc0_frame_callback_evidence_size_data_v1", STT_OBJECT,
       &output->evidence_size_data, 0},
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
      *item.address = load_bias + static_cast<std::uintptr_t>(symbol.st_value);
      ++item.matches;
    }
  }
  for (const auto& item : wanted)
    if (item.matches != 1 || *item.address == 0) return false;

  // Runtime host permissions do not express ARM64 guest executability under
  // Houdini.  Prove code/data provenance from the hash-pinned ELF instead.
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
  if (!in_file_load(output->wrapper, 24, PF_R | PF_X) ||
      !in_file_load(output->shadow_data, 8, PF_R | PF_W) ||
      !in_file_load(output->control_data, 8, PF_R | PF_W) ||
      !in_file_load(output->evidence_data, 8, PF_R | PF_W) ||
      !in_file_load(output->control_size_data, 8, PF_R | PF_W) ||
      !in_file_load(output->evidence_size_data, 8, PF_R | PF_W))
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
                       detail::HashFile(file, hash) &&
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
  const std::uintptr_t locator_addresses[] = {
      symbols.shadow_data,       symbols.control_data,
      symbols.evidence_data,     symbols.control_size_data,
      symbols.evidence_size_data,
  };
  const auto* wrapper_map = detail::MappingAt(maps, layout.wrapper, 24);
  if (!wrapper_map || wrapper_map->path != path ||
      wrapper_map->perms[0] != 'r' || wrapper_map->perms[1] == 'w')
    return false;
  for (const std::uintptr_t address : locator_addresses) {
    const auto* mapping = detail::MappingAt(maps, address, 8);
    if (!mapping || mapping->path != path || mapping->perms[0] != 'r')
      return false;
  }
  if (!detail::ReadAt(process_mem, symbols.shadow_data, &layout.shadow,
                      sizeof(layout.shadow)) ||
      !detail::ReadAt(process_mem, symbols.control_data, &layout.control,
                      sizeof(layout.control)) ||
      !detail::ReadAt(process_mem, symbols.evidence_data, &layout.evidence,
                      sizeof(layout.evidence)) ||
      !detail::ReadAt(process_mem, symbols.control_size_data,
                      &layout.control_size, sizeof(layout.control_size)) ||
      !detail::ReadAt(process_mem, symbols.evidence_size_data,
                      &layout.evidence_size, sizeof(layout.evidence_size)) ||
      layout.control_size != 64 || layout.evidence_size != 128 ||
      (layout.shadow & 63u) != 0 || (layout.control & 63u) != 0 ||
      (layout.evidence & 63u) != 0 || (layout.wrapper & 3u) != 0)
    return false;

  const auto* shadow_map = detail::MappingAt(maps, layout.shadow, 0x908);
  const auto* control_map = detail::MappingAt(maps, layout.control, 64);
  const auto* evidence_map = detail::MappingAt(maps, layout.evidence, 128);
  if (!shadow_map || !control_map || !evidence_map ||
      shadow_map->path != path || control_map->path != path ||
      evidence_map->path != path ||
      shadow_map->perms[0] != 'r' || shadow_map->perms[1] != 'w' ||
      control_map->perms[0] != 'r' || control_map->perms[1] != 'w' ||
      evidence_map->perms[0] != 'r' || evidence_map->perms[1] != 'w')
    return false;
  std::memcpy(layout.file_sha256, hash, sizeof(hash));
  std::memcpy(layout.mapped_path, path.c_str(), path.size() + 1);
  *output = layout;
  return true;
}

}  // namespace a9tas::fc1_payload_elf_v1
