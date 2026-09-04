#include "a9_arm64_profile_signature_catalog_v1.h"
#include "fc1_payload_elf_resolver_v1.h"
#include "g8_runtime_build_profile_v1.h"

#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace catalog = a9tas::arm64_profile_catalog_v1;
namespace profile_abi = a9tas::g8_runtime_build_profile_v1;
namespace hash_detail = a9tas::fc1_payload_elf_v1::detail;

constexpr std::uint64_t kMaxRoleLocalDrift = 0x1000;

bool RoleSignatureMatches(const catalog::Role& role,
                          const std::uint8_t* candidate) {
  std::array<std::uint32_t,7> masks{};
  if (std::strcmp(role.name, "logic_dispatcher") == 0) {
    masks = {0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu,
             0x9f00001fu,0xffc003ffu,0xffffffffu};
  } else if (std::strcmp(role.name, "barrel_random_bool") == 0) {
    masks = {0xffffffffu,0xffffffffu,0xfc000000u,0xfc000000u,
             0xffffffffu,0xffffffffu,0xffffffffu};
  } else if (std::strcmp(role.name, "barrel_random_lerp") == 0) {
    masks = {0xffffffffu,0xffffffffu,0xfc000000u,0xfc000000u,
             0xffffffffu,0xffffffffu,0u};
  } else {
    return std::memcmp(candidate, role.signature.data(), role.size) == 0;
  }
  if ((role.size & 3u) != 0 || role.size > masks.size() * 4) return false;
  for (std::size_t offset = 0; offset < role.size; offset += 4) {
    std::uint32_t observed{}, expected{};
    std::memcpy(&observed, candidate + offset, 4);
    std::memcpy(&expected, role.signature.data() + offset, 4);
    const std::uint32_t mask = masks[offset / 4];
    if ((observed & mask) != (expected & mask)) return false;
  }
  return true;
}

bool SelectRoleHit(const std::vector<std::uint64_t>& hits,
                   std::uint64_t expected, std::uint64_t* selected) {
  if (std::find(hits.begin(), hits.end(), expected) != hits.end()) {
    *selected = expected;
    return true;
  }
  std::uint64_t nearby = 0;
  std::size_t nearby_count = 0;
  for (const std::uint64_t hit : hits) {
    const std::uint64_t distance = hit > expected ? hit - expected : expected - hit;
    if (distance <= kMaxRoleLocalDrift) {
      nearby = hit;
      ++nearby_count;
    }
  }
  if (nearby_count != 1) return false;
  *selected = nearby;
  return true;
}

struct Load {
  std::uint64_t offset{}, vaddr{}, filesz{}, memsz{};
  std::uint32_t flags{};
};

class Elf {
 public:
  ~Elf() {
    if (data_ != MAP_FAILED) munmap(data_, size_);
    if (fd_ >= 0) close(fd_);
  }

  bool Open(const char* path, std::string* error) {
    fd_ = open(path, O_RDONLY | O_CLOEXEC);
    struct stat info{};
    if (fd_ < 0 || fstat(fd_, &info) != 0 || info.st_size < 4096 ||
        info.st_size > (1LL << 31)) return Fail(error, "candidate file is unavailable");
    size_ = static_cast<std::size_t>(info.st_size);
    data_ = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (data_ == MAP_FAILED) return Fail(error, "candidate mmap failed");
    if (size_ < sizeof(Elf64_Ehdr)) return Fail(error, "ELF is truncated");
    const auto* header = reinterpret_cast<const Elf64_Ehdr*>(data_);
    if (std::memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
        header->e_ident[EI_CLASS] != ELFCLASS64 ||
        header->e_ident[EI_DATA] != ELFDATA2LSB || header->e_machine != EM_AARCH64 ||
        header->e_phentsize != sizeof(Elf64_Phdr) || header->e_phnum == 0 ||
        header->e_phoff > size_ ||
        header->e_phnum > (size_ - header->e_phoff) / sizeof(Elf64_Phdr))
      return Fail(error, "expected a valid little-endian AArch64 ELF");
    const auto* programs = reinterpret_cast<const Elf64_Phdr*>(
        Bytes() + header->e_phoff);
    for (std::size_t index = 0; index < header->e_phnum; ++index) {
      const auto& item = programs[index];
      if (item.p_offset > size_ || item.p_filesz > size_ - item.p_offset)
        return Fail(error, "ELF program segment exceeds file");
      if (item.p_type == PT_LOAD)
        loads_.push_back({item.p_offset, item.p_vaddr, item.p_filesz,
                          item.p_memsz, item.p_flags});
      else if (item.p_type == PT_NOTE && !ReadNotes(item, error)) return false;
    }
    if (loads_.empty() || build_id_.size() != 20)
      return Fail(error, "ELF load segments or GNU Build ID are unavailable");
    if (!hash_detail::HashFile(fd_, sha_.data()))
      return Fail(error, "candidate SHA-256 failed");
    return true;
  }

  const std::uint8_t* Bytes() const {
    return static_cast<const std::uint8_t*>(data_);
  }
  const std::array<std::uint8_t,32>& Sha() const { return sha_; }
  const std::vector<std::uint8_t>& BuildId() const { return build_id_; }
  const std::vector<Load>& Loads() const { return loads_; }
  std::uint64_t ImageSize() const {
    std::uint64_t result = 0;
    for (const auto& load : loads_) result = std::max(result, load.vaddr + load.memsz);
    return result;
  }
  std::uint64_t ExecutableEnd() const {
    std::uint64_t result = 0;
    for (const auto& load : loads_)
      if (load.flags & PF_X) result = std::max(result, load.vaddr + load.memsz);
    return result;
  }
  const std::uint8_t* At(std::uint64_t rva, std::size_t size) const {
    for (const auto& load : loads_)
      if (rva >= load.vaddr && size <= load.filesz &&
          rva - load.vaddr <= load.filesz - size)
        return Bytes() + load.offset + (rva - load.vaddr);
    return nullptr;
  }
  bool Executable(std::uint64_t rva, std::size_t size) const {
    for (const auto& load : loads_)
      if ((load.flags & PF_X) && rva >= load.vaddr && size <= load.filesz &&
          rva - load.vaddr <= load.filesz - size) return true;
    return false;
  }

  std::vector<std::uint64_t> FindPair(std::uint64_t first,
                                      std::uint64_t second,
                                      std::uint64_t slot) const {
    std::vector<std::uint64_t> result;
    for (const auto& load : loads_) {
      if (!(load.flags & PF_R) || (load.flags & PF_X)) continue;
      for (std::uint64_t relative = 0; relative + 16 <= load.filesz; relative += 8) {
        const auto* words = reinterpret_cast<const std::uint64_t*>(
            Bytes() + load.offset + relative);
        if (words[0] == first && words[1] == second && load.vaddr + relative >= slot)
          result.push_back(load.vaddr + relative - slot);
      }
    }
    SortUnique(&result);
    return result;
  }

  std::unordered_map<std::uint64_t,std::vector<std::uint64_t>>
  PointerOccurrences(const std::set<std::uint64_t>& wanted) const {
    std::unordered_map<std::uint64_t,std::vector<std::uint64_t>> result;
    for (const auto& load : loads_) {
      if (!(load.flags & PF_R) || (load.flags & PF_X)) continue;
      for (std::uint64_t relative = 0; relative + 8 <= load.filesz; relative += 8) {
        std::uint64_t value{};
        std::memcpy(&value, Bytes() + load.offset + relative, 8);
        if (wanted.count(value)) result[value].push_back(load.vaddr + relative);
      }
    }
    return result;
  }

  std::vector<std::uint64_t> DirectCallers(std::uint64_t target) const {
    std::vector<std::uint64_t> result;
    for (const auto& load : loads_) {
      if (!(load.flags & PF_X)) continue;
      for (std::uint64_t relative = 0; relative + 4 <= load.filesz; relative += 4) {
        std::uint32_t word{};
        std::memcpy(&word, Bytes() + load.offset + relative, 4);
        if ((word >> 26) != 0b100101) continue;
        std::int64_t immediate = word & 0x03ffffffu;
        if (immediate & (1 << 25)) immediate -= (1 << 26);
        const std::uint64_t caller = load.vaddr + relative;
        if (static_cast<std::uint64_t>(
                static_cast<std::int64_t>(caller) + (immediate << 2)) == target)
          result.push_back(caller);
      }
    }
    return result;
  }

  bool HasDirectBranchTo(std::uint64_t target) const {
    for (const auto& load : loads_) {
      if (!(load.flags & PF_X)) continue;
      for (std::uint64_t relative = 0; relative + 4 <= load.filesz;
           relative += 4) {
        std::uint32_t word{};
        std::memcpy(&word, Bytes() + load.offset + relative, 4);
        std::int64_t immediate = 0;
        unsigned bits = 0;
        if ((word & 0x7c000000u) == 0x14000000u) {
          immediate = word & 0x03ffffffu;
          bits = 26;
        } else if ((word & 0xff000010u) == 0x54000000u ||
                   (word & 0x7e000000u) == 0x34000000u) {
          immediate = (word >> 5) & 0x7ffffu;
          bits = 19;
        } else if ((word & 0x7e000000u) == 0x36000000u) {
          immediate = (word >> 5) & 0x3fffu;
          bits = 14;
        } else {
          continue;
        }
        if (immediate & (std::int64_t{1} << (bits - 1)))
          immediate -= std::int64_t{1} << bits;
        const std::int64_t source = static_cast<std::int64_t>(
            load.vaddr + relative);
        if (source + (immediate << 2) == static_cast<std::int64_t>(target))
          return true;
      }
    }
    return false;
  }

 private:
  static void SortUnique(std::vector<std::uint64_t>* value) {
    std::sort(value->begin(), value->end());
    value->erase(std::unique(value->begin(), value->end()), value->end());
  }
  static bool Fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
  }
  bool ReadNotes(const Elf64_Phdr& note, std::string* error) {
    std::size_t cursor = static_cast<std::size_t>(note.p_offset);
    const std::size_t end = cursor + static_cast<std::size_t>(note.p_filesz);
    while (cursor + sizeof(Elf64_Nhdr) <= end) {
      Elf64_Nhdr header{};
      std::memcpy(&header, Bytes() + cursor, sizeof(header));
      cursor += sizeof(header);
      const std::size_t name_end = cursor + header.n_namesz;
      const std::size_t desc_start = (name_end + 3) & ~std::size_t(3);
      const std::size_t desc_end = desc_start + header.n_descsz;
      const std::size_t next = (desc_end + 3) & ~std::size_t(3);
      if (next > end) return Fail(error, "ELF note is truncated");
      if (header.n_type == NT_GNU_BUILD_ID && header.n_namesz >= 3 &&
          std::memcmp(Bytes() + cursor, "GNU", 3) == 0) {
        std::vector<std::uint8_t> value(Bytes() + desc_start, Bytes() + desc_end);
        if (!build_id_.empty() && build_id_ != value)
          return Fail(error, "conflicting GNU Build IDs");
        build_id_ = std::move(value);
      }
      cursor = next;
    }
    return true;
  }

  int fd_{-1};
  void* data_{MAP_FAILED};
  std::size_t size_{};
  std::vector<Load> loads_;
  std::array<std::uint8_t,32> sha_{};
  std::vector<std::uint8_t> build_id_;
};

struct Resolution {
  std::vector<std::uint64_t> roles;
  std::vector<std::uint64_t> vtables;
  std::array<std::int64_t,3> deltas{};
  std::uint64_t adjusted_vtable{};
  std::uint64_t scheduler_return{};
  std::vector<std::uint64_t> lifecycle_vtables;
};

int RoleIndex(const char* name) {
  for (std::size_t index = 0; index < catalog::kRoles.size(); ++index)
    if (std::strcmp(catalog::kRoles[index].name, name) == 0)
      return static_cast<int>(index);
  return -1;
}

bool ValidateRandomPatchWindows(const Elf& elf, const Resolution& resolution,
                                std::string* error) {
  for (const char* role_name : {"barrel_random_bool", "barrel_random_lerp"}) {
    const int role_index = RoleIndex(role_name);
    if (role_index < 0 ||
        static_cast<std::size_t>(role_index) >= resolution.roles.size()) {
      *error = "random-hook role is unavailable";
      return false;
    }
    const std::uint64_t target = resolution.roles[role_index];
    const auto* bytes = elf.At(target, 16);
    std::uint32_t fourth{};
    if (!bytes || !elf.Executable(target, 16)) {
      *error = "random-hook 16-byte patch window is unavailable";
      return false;
    }
    std::memcpy(&fourth, bytes + 12, sizeof(fourth));
    if ((fourth & 0xfc000000u) != 0x94000000u) {
      *error = "random-hook fourth instruction is not BL";
      return false;
    }
    if (elf.HasDirectBranchTo(target + 12)) {
      *error = "random-hook fourth instruction has a direct branch entry";
      return false;
    }
  }
  return true;
}
int VtableIndex(const char* name) {
  for (std::size_t index = 0; index < catalog::kVtables.size(); ++index)
    if (std::strcmp(catalog::kVtables[index].name, name) == 0)
      return static_cast<int>(index);
  return -1;
}

bool ResolveRoles(const Elf& elf, Resolution* out, std::string* error) {
  out->roles.assign(catalog::kRoles.size(), 0);
  std::vector<std::vector<std::uint64_t>> hits(catalog::kRoles.size());
  std::unordered_map<std::uint64_t,std::vector<std::size_t>> prefixes;
  for (std::size_t index = 0; index < catalog::kRoles.size(); ++index) {
    std::uint64_t prefix{};
    std::memcpy(&prefix, catalog::kRoles[index].signature.data(), 8);
    prefixes[prefix].push_back(index);
  }
  for (const auto& load : elf.Loads()) {
    if (!(load.flags & PF_X)) continue;
    for (std::uint64_t relative = 0; relative + 28 <= load.filesz; relative += 4) {
      std::uint64_t prefix{};
      std::memcpy(&prefix, elf.Bytes() + load.offset + relative, 8);
      const auto found = prefixes.find(prefix);
      if (found == prefixes.end()) continue;
      for (const std::size_t index : found->second) {
        const auto& role = catalog::kRoles[index];
        if (RoleSignatureMatches(role, elf.Bytes() + load.offset + relative))
          hits[index].push_back(load.vaddr + relative);
      }
    }
  }
  for (std::uint8_t group = 0; group < 3; ++group) {
    std::map<std::int64_t,std::size_t> votes;
    for (std::size_t index = 0; index < catalog::kRoles.size(); ++index) {
      const auto& role = catalog::kRoles[index];
      if (role.group == group && hits[index].size() == 1)
        ++votes[static_cast<std::int64_t>(hits[index][0]) -
                static_cast<std::int64_t>(role.reference_rva)];
    }
    if (votes.empty()) { *error = "a semantic group has no unique signature anchor"; return false; }
    std::size_t best_count = 0; std::int64_t best_delta = 0; bool tied = false;
    for (const auto& [delta,count] : votes) {
      if (count > best_count) { best_count = count; best_delta = delta; tied = false; }
      else if (count == best_count) tied = true;
    }
    if (tied) { *error = "semantic group delta is ambiguous"; return false; }
    out->deltas[group] = best_delta;
    for (std::size_t index = 0; index < catalog::kRoles.size(); ++index) {
      const auto& role = catalog::kRoles[index];
      if (role.group != group) continue;
      const std::uint64_t expected = static_cast<std::uint64_t>(
          static_cast<std::int64_t>(role.reference_rva) + best_delta);
      std::uint64_t selected = 0;
      if (!SelectRoleHit(hits[index], expected, &selected)) {
        *error = std::string("signature group has no unique nearby role: ") + role.name;
        return false;
      }
      out->roles[index] = selected;
    }
  }
  return true;
}

bool ResolveVtables(const Elf& elf, Resolution* out, std::string* error) {
  const int brake = RoleIndex("adjusted_brake_setter");
  const int steering = RoleIndex("adjusted_steering_setter");
  auto adjusted = elf.FindPair(out->roles[brake], out->roles[steering], 0x2a8);
  if (adjusted.size() != catalog::kAdjustedCandidateCount) {
    *error = "adjusted-setter vtable topology changed"; return false;
  }
  out->adjusted_vtable = adjusted.front();
  const auto* candidate_adjusted = elf.At(out->adjusted_vtable,
                                          catalog::kAdjustedVtable.size() * 8);
  if (!candidate_adjusted) { *error = "adjusted vtable is not file-backed"; return false; }
  std::size_t pointer_pairs = 0, delta_pairs = 0;
  for (std::size_t index = 0; index < catalog::kAdjustedVtable.size(); ++index) {
    std::uint64_t value{}; std::memcpy(&value, candidate_adjusted + index * 8, 8);
    const std::uint64_t old = catalog::kAdjustedVtable[index];
    if (old >= 0x1000 && old < 0x10000000ULL && value >= 0x1000 &&
        value < elf.ExecutableEnd()) {
      ++pointer_pairs;
      if (static_cast<std::int64_t>(value) - static_cast<std::int64_t>(old) ==
          out->deltas[0]) ++delta_pairs;
    }
  }
  if (pointer_pairs < 100 || delta_pairs * 100 < pointer_pairs * 85) {
    *error = "adjusted vtable structural score is too weak"; return false;
  }

  struct Slot { std::uint16_t offset; std::uint64_t mapped; };
  std::vector<std::vector<Slot>> slots(catalog::kVtables.size());
  std::set<std::uint64_t> wanted;
  for (std::size_t table_index = 0; table_index < catalog::kVtables.size(); ++table_index) {
    const auto& table = catalog::kVtables[table_index];
    for (std::size_t slot = 0; slot < 64; ++slot) {
      if (!table.mapped[slot]) continue;
      std::vector<std::uint64_t> matches;
      for (const std::int64_t delta : out->deltas) {
        const std::uint64_t mapped = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(table.pointers[slot]) + delta);
        const auto* bytes = elf.At(mapped, 28);
        if (elf.Executable(mapped, 28) && bytes &&
            std::memcmp(bytes, table.signatures[slot].data(), 28) == 0)
          matches.push_back(mapped);
      }
      std::sort(matches.begin(), matches.end());
      matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
      if (matches.size() == 1) {
        slots[table_index].push_back({static_cast<std::uint16_t>(slot * 8), matches[0]});
        wanted.insert(matches[0]);
      }
    }
  }
  const auto occurrences = elf.PointerOccurrences(wanted);
  out->vtables.assign(catalog::kVtables.size(), 0);
  std::vector<std::vector<std::uint64_t>> candidates(catalog::kVtables.size());
  for (std::size_t index = 0; index < catalog::kVtables.size(); ++index) {
    std::map<std::uint64_t,std::size_t> votes;
    for (const auto& slot : slots[index]) {
      const auto found = occurrences.find(slot.mapped);
      if (found == occurrences.end()) continue;
      for (const std::uint64_t location : found->second)
        if (location >= slot.offset) ++votes[location - slot.offset];
    }
    if (votes.empty()) { *error = std::string("vtable has no candidate: ") + catalog::kVtables[index].name; return false; }
    std::size_t best = 0;
    for (const auto& [base,count] : votes) best = std::max(best, count);
    const bool practice_table =
        std::strncmp(catalog::kVtables[index].name, "practice_", 9) == 0;
    // Channel builds may relink a subset of the virtual functions referenced
    // by the five PracticeRace subobject tables.  In that case fewer function
    // pointers can be mapped by the three coarse code deltas, but every mapped
    // slot must still vote for the same table.  Keep a non-trivial floor here;
    // the exact relative layout of all five tables is checked below.
    const bool complete_practice_vote = practice_table && best >= 8 &&
        best == slots[index].size();
    if (best < catalog::kVtables[index].minimum && !complete_practice_vote) {
      *error = std::string("vtable score is too weak: ") +
          catalog::kVtables[index].name;
      return false;
    }
    for (const auto& [base,count] : votes) if (count == best) candidates[index].push_back(base);
    if (candidates[index].size() == 1) out->vtables[index] = candidates[index][0];
    else if (std::strcmp(catalog::kVtables[index].name, "physics_implementation") == 0 ||
             std::strcmp(catalog::kVtables[index].name, "vehicle_source") == 0) {
      const std::uint64_t expected = out->adjusted_vtable +
          catalog::kVtables[index].reference_rva - catalog::kAdjustedVtableRva;
      if (std::find(candidates[index].begin(), candidates[index].end(), expected) !=
          candidates[index].end()) out->vtables[index] = expected;
    }
  }
  const int backend = VtableIndex("physics_backend");
  const int native = VtableIndex("native_physics_body");
  if (out->vtables[native] == 0 && out->vtables[backend] != 0) {
    const std::int64_t data_delta = static_cast<std::int64_t>(out->vtables[backend]) -
        static_cast<std::int64_t>(catalog::kVtables[backend].reference_rva);
    std::uint64_t chosen = 0; std::uint64_t best_error = UINT64_MAX;
    for (const auto value : candidates[native]) {
      const std::int64_t observed = static_cast<std::int64_t>(value) -
          static_cast<std::int64_t>(catalog::kVtables[native].reference_rva);
      const std::uint64_t distance = static_cast<std::uint64_t>(
          observed > data_delta ? observed - data_delta : data_delta - observed);
      if (distance < best_error) { best_error = distance; chosen = value; }
    }
    if (best_error <= 0x1000) out->vtables[native] = chosen;
  }
  std::set<int> pending;
  for (std::size_t index = 0; index < catalog::kVtables.size(); ++index) {
    const std::string name = catalog::kVtables[index].name;
    if ((name.rfind("physics_interface_", 0) == 0 || name == "step_options") &&
        out->vtables[index] == 0) pending.insert(static_cast<int>(index));
  }
  while (!pending.empty()) {
    bool progressed = false;
    for (const int index : pending) {
      int anchor = -1; std::uint64_t distance = UINT64_MAX;
      for (std::size_t other = 0; other < catalog::kVtables.size(); ++other) {
        if (out->vtables[other] == 0) continue;
        const auto a = catalog::kVtables[index].reference_rva;
        const auto b = catalog::kVtables[other].reference_rva;
        const auto d = a > b ? a - b : b - a;
        if (d < distance) { distance = d; anchor = static_cast<int>(other); }
      }
      if (anchor < 0) continue;
      const std::uint64_t expected = catalog::kVtables[index].reference_rva +
          out->vtables[anchor] - catalog::kVtables[anchor].reference_rva;
      if (std::find(candidates[index].begin(), candidates[index].end(), expected) ==
          candidates[index].end()) continue;
      out->vtables[index] = expected; pending.erase(index); progressed = true; break;
    }
    if (!progressed) break;
  }
  const int practice_anchor = VtableIndex("practice_vptr0");
  if (practice_anchor < 0 || out->vtables[practice_anchor] == 0) {
    *error = "practice vtable anchor is unresolved";
    return false;
  }
  for (const char* name : {"practice_vptr588", "practice_vptr6c0",
                           "practice_vptr718", "practice_vptr748"}) {
    const int index = VtableIndex(name);
    if (index < 0 || out->vtables[index] == 0) {
      *error = std::string("practice vtable is unresolved: ") + name;
      return false;
    }
    const std::uint64_t expected = out->vtables[practice_anchor] +
        catalog::kVtables[index].reference_rva -
        catalog::kVtables[practice_anchor].reference_rva;
    if (out->vtables[index] != expected) {
      *error = std::string("practice vtable relative layout changed: ") + name;
      return false;
    }
  }
  for (std::size_t index = 0; index < catalog::kVtables.size(); ++index)
    if (out->vtables[index] == 0) {
      *error = std::string("vtable remains ambiguous: ") + catalog::kVtables[index].name;
      return false;
    }
  return true;
}

bool ResolveRelationships(const Elf& elf, Resolution* out, std::string* error) {
  const auto callers = elf.DirectCallers(out->roles[RoleIndex("frame_event")]);
  if (callers.size() != catalog::kSchedulerCallerCount ||
      catalog::kSchedulerCallerIndex >= callers.size()) {
    *error = "FrameEvent caller topology changed"; return false;
  }
  const std::uint64_t callsite = callers[catalog::kSchedulerCallerIndex];
  for (int index = 0; index < 9; ++index) {
    const auto* bytes = elf.At(callsite - 16 + index * 4, 4);
    if (!bytes) { *error = "FrameEvent caller context unavailable"; return false; }
    std::uint32_t word{}; std::memcpy(&word, bytes, 4);
    const std::uint32_t reference = catalog::kSchedulerContext[index];
    if ((word >> 26) == 0b100101 && (reference >> 26) == 0b100101) continue;
    if (word != reference) { *error = "FrameEvent caller context changed"; return false; }
  }
  out->scheduler_return = callsite + 4;
  auto first = elf.FindPair(out->roles[RoleIndex("lifecycle_phase_gate")],
                            out->roles[RoleIndex("lifecycle_shared_enter")], 0x1d8);
  auto second = elf.FindPair(out->roles[RoleIndex("lifecycle_phase_gate")],
                             out->roles[RoleIndex("lifecycle_derived_enter")], 0x1d8);
  first.insert(first.end(), second.begin(), second.end());
  std::sort(first.begin(), first.end());
  first.erase(std::unique(first.begin(), first.end()), first.end());
  if (first.size() != catalog::kLifecycleVtableCount) {
    *error = "lifecycle vtable topology changed"; return false;
  }
  out->lifecycle_vtables = std::move(first);
  return true;
}

std::uint64_t Role(const Resolution& value, const char* name) {
  return value.roles[RoleIndex(name)];
}
std::uint64_t Vtable(const Resolution& value, const char* name) {
  return value.vtables[VtableIndex(name)];
}

bool WriteProfile(const Elf& elf, const Resolution& r, const char* output,
                  std::array<std::uint8_t,32>* blob_sha, std::string* error) {
  profile_abi::Profile profile{};
  std::memcpy(profile.magic, profile_abi::kMagic, 8);
  profile.version = profile_abi::kVersion;
  profile.size = sizeof(profile);
  profile.flags = profile_abi::kRequiredFlags;
  profile.image_size = elf.ImageSize();
  const char* hooks[] = {"physics_interval","final_writer","frame_event","nitro_state",
                         "physics_submit","barrel_roll_tail","barrel_yaw_tail","logic_dispatcher"};
  for (std::size_t i = 0; i < 8; ++i) profile.hook_rvas[i] = Role(r, hooks[i]);
  profile.main_time_source_vtable_rva = Vtable(r,"main_time_source");
  profile.embedded_time_source_vtable_rva = Vtable(r,"embedded_time_source");
  profile.physics_context_vtable_rva = Vtable(r,"physics_context");
  profile.physics_implementation_vtable_rva = Vtable(r,"physics_implementation");
  profile.step_options_vtable_rva = Vtable(r,"step_options");
  profile.native_physics_body_vtable_rva = Vtable(r,"native_physics_body");
  profile.nitro_service_vtable_rva = Vtable(r,"nitro_service");
  profile.nitro_dispatch_rva = Role(r,"nitro_dispatch");
  profile.vehicle_source_vtable_rva = Vtable(r,"vehicle_source");
  profile.car_physics_body_source_vtable_rva = Vtable(r,"physics_implementation");
  profile.adjusted_setter_vtable_rva = r.adjusted_vtable;
  profile.setter_original_rvas[0] = Role(r,"adjusted_brake_setter");
  profile.setter_original_rvas[1] = Role(r,"adjusted_steering_setter");
  profile.frame_event_scheduler_return_rva = r.scheduler_return;
  profile.lifecycle_phase_gate_rva = Role(r,"lifecycle_phase_gate");
  profile.lifecycle_shared_enter_rva = Role(r,"lifecycle_shared_enter");
  profile.lifecycle_derived_enter_rva = Role(r,"lifecycle_derived_enter");
  profile.lifecycle_racing_store_rva = Role(r,"lifecycle_racing_store");
  profile.barrel_random_bool_rva = Role(r,"barrel_random_bool");
  profile.barrel_random_lerp_rva = Role(r,"barrel_random_lerp");
  std::memcpy(profile.native_sha256, elf.Sha().data(), 32);
  std::memcpy(profile.build_id, elf.BuildId().data(), 20);
  hash_detail::Sha256 profile_hash;
  profile_hash.Update(catalog::kCatalogSha.data(), catalog::kCatalogSha.size());
  profile_hash.Update(profile.native_sha256, 32);
  profile_hash.Update(profile.build_id, 20);
  profile_hash.Update(reinterpret_cast<const std::uint8_t*>(r.roles.data()),
                      r.roles.size() * sizeof(std::uint64_t));
  profile_hash.Update(reinterpret_cast<const std::uint8_t*>(r.vtables.data()),
                      r.vtables.size() * sizeof(std::uint64_t));
  profile_hash.Update(reinterpret_cast<const std::uint8_t*>(r.lifecycle_vtables.data()),
                      r.lifecycle_vtables.size() * sizeof(std::uint64_t));
  profile_hash.Final(profile.profile_sha256);
  if (!profile_abi::Valid(profile)) { *error = "generated A9BPR1 core is invalid"; return false; }

  std::vector<std::uint64_t> vehicle = {
      Vtable(r,"physics_interface_0"),Vtable(r,"physics_interface_1"),
      Vtable(r,"physics_interface_2"),Vtable(r,"physics_interface_3"),
      Vtable(r,"physics_interface_4"),Vtable(r,"physics_interface_5"),
      Vtable(r,"physics_interface_6"),Vtable(r,"physics_interface_7"),
      Vtable(r,"physics_interface_8"),Role(r,"vehicle_position_getter"),
      Role(r,"vehicle_rotation_getter")};
  const char* wrapper[] = {"40","48","58","60","68","88","90","98","A0"};
  for (const char* suffix : wrapper)
    vehicle.push_back(Role(r,(std::string("vehicle_wrapper_slot")+suffix).c_str()));
  for (const char* suffix : wrapper)
    vehicle.push_back(Role(r,(std::string("vehicle_delegate_slot")+suffix).c_str()));
  vehicle.push_back(Vtable(r,"vehicle_source"));
  vehicle.push_back(Role(r,"vehicle_source_update"));
  vehicle.push_back(Vtable(r,"physics_implementation"));
  vehicle.push_back(Role(r,"car_physics_body_update"));
  vehicle.push_back(Vtable(r,"physics_backend"));
  vehicle.push_back(Vtable(r,"native_physics_body"));
  const char* api[] = {"physics_set_pose","physics_set_position","physics_set_rotation",
                       "physics_set_linear","physics_set_angular","physics_get_linear",
                       "physics_get_angular"};
  for (const char* name : api) vehicle.push_back(Role(r,name));
  if (vehicle.size() != 42) { *error = "generated vehicle annex count drift"; return false; }
  struct Annex { char magic[8]; std::uint32_t version,header_size,vehicle_count,
                 lifecycle_count,total_size,reserved; };
  const std::size_t total = sizeof(profile) + sizeof(Annex) +
      8 * (vehicle.size() + r.lifecycle_vtables.size());
  std::vector<std::uint8_t> blob(total);
  std::memcpy(blob.data(), &profile, sizeof(profile));
  Annex annex{{'A','9','B','P','A','X','1','\0'},1,sizeof(Annex),
              static_cast<std::uint32_t>(vehicle.size()),
              static_cast<std::uint32_t>(r.lifecycle_vtables.size()),
              static_cast<std::uint32_t>(total),0};
  std::memcpy(blob.data()+sizeof(profile),&annex,sizeof(annex));
  std::memcpy(blob.data()+sizeof(profile)+sizeof(annex),vehicle.data(),vehicle.size()*8);
  std::memcpy(blob.data()+sizeof(profile)+sizeof(annex)+vehicle.size()*8,
              r.lifecycle_vtables.data(),r.lifecycle_vtables.size()*8);
  const int fd = open(output,O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC,0600);
  if (fd < 0) { *error = "output Profile cannot be created"; return false; }
  std::size_t done=0;
  while(done<blob.size()) { const ssize_t n=write(fd,blob.data()+done,blob.size()-done);
    if(n<=0){close(fd);*error="output Profile write failed";return false;} done+=n; }
  if (fsync(fd)!=0) { close(fd); *error="output Profile fsync failed"; return false; }
  close(fd);
  hash_detail::Sha256 digest; digest.Update(blob.data(),blob.size()); digest.Final(blob_sha->data());
  return true;
}

void PrintHex(const std::uint8_t* value, std::size_t size) {
  for (std::size_t index=0; index<size; ++index) std::printf("%02x",value[index]);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr,"usage: a9tas_arm64_profile_autogen_v1 CANDIDATE OUTPUT\n");
    return 2;
  }
  Elf elf; Resolution resolution; std::string error;
  if (!elf.Open(argv[1],&error) || !ResolveRoles(elf,&resolution,&error) ||
      !ValidateRandomPatchWindows(elf,resolution,&error) ||
      !ResolveVtables(elf,&resolution,&error) ||
      !ResolveRelationships(elf,&resolution,&error)) {
    std::fprintf(stderr,"A9_PROFILE_AUTOGEN_V1 passed=0 error=%s\n",error.c_str());
    return 1;
  }
  std::array<std::uint8_t,32> profile_sha{};
  if (!WriteProfile(elf,resolution,argv[2],&profile_sha,&error)) {
    std::fprintf(stderr,"A9_PROFILE_AUTOGEN_V1 passed=0 error=%s\n",error.c_str());
    return 1;
  }
  std::printf("A9_PROFILE_AUTOGEN_V1 passed=1 native_sha256="); PrintHex(elf.Sha().data(),32);
  std::printf(" build_id="); PrintHex(elf.BuildId().data(),elf.BuildId().size());
  std::printf(" profile_sha256="); PrintHex(profile_sha.data(),32);
  std::printf(" vptr0=%llx vptr588=%llx vptr6c0=%llx vptr718=%llx vptr748=%llx catalog_sha256=",
      static_cast<unsigned long long>(Vtable(resolution,"practice_vptr0")),
      static_cast<unsigned long long>(Vtable(resolution,"practice_vptr588")),
      static_cast<unsigned long long>(Vtable(resolution,"practice_vptr6c0")),
      static_cast<unsigned long long>(Vtable(resolution,"practice_vptr718")),
      static_cast<unsigned long long>(Vtable(resolution,"practice_vptr748")));
  PrintHex(catalog::kCatalogSha.data(),catalog::kCatalogSha.size());
  std::printf("\n");
  return 0;
}
