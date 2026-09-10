#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

namespace a9tas::race_lifecycle_v1 {

inline constexpr std::uintptr_t kImageSize = 0xA5D8168;
inline constexpr std::uintptr_t kPhaseGateRva = 0x3A5955C;
inline constexpr std::uintptr_t kSharedRaceEnterRva = 0x3A59578;
inline constexpr std::uintptr_t kDerivedRaceEnterRva = 0x382EB04;
inline constexpr std::uintptr_t kRacingStoreRva = 0x3A596C4;
inline constexpr std::uintptr_t kVptrMinimumRva = 0x7F3EE98;
inline constexpr std::uintptr_t kVptrMaximumRva = 0x80B7740;
inline constexpr std::uintptr_t kPhaseGateSlot = 0x1D8;
inline constexpr std::uintptr_t kPhaseEnterSlot = 0x1E0;
inline constexpr std::uintptr_t kPhaseStateOffset = 0x2D8;
inline constexpr std::uint32_t kCountdownState = 2;
inline constexpr std::uint32_t kRacingState = 3;

struct Mapping {
  std::uintptr_t begin{};
  std::uintptr_t end{};
  char perms[5]{};
  std::string path;
};

struct Candidate {
  std::uintptr_t object{};
  std::uintptr_t vptr{};
  std::uintptr_t phase_enter{};
  std::uintptr_t state_address{};
  std::uint32_t state{};
  std::uint8_t flags[5]{};
};

struct Resolution {
  std::uint64_t scanned_bytes{};
  std::uint32_t structurally_valid{};
  std::uint32_t countdown_candidates{};
  Candidate selected{};
};

struct Profile {
  std::uintptr_t image_size{};
  std::uintptr_t phase_gate_rva{};
  std::uintptr_t shared_race_enter_rva{};
  std::uintptr_t derived_race_enter_rva{};
  std::uintptr_t racing_store_rva{};
  const std::uintptr_t* lifecycle_vtable_rvas{};
  std::size_t lifecycle_vtable_count{};
  std::uintptr_t fallback_vptr_minimum_rva{};
  std::uintptr_t fallback_vptr_maximum_rva{};
};

inline constexpr Profile ReferenceProfile() noexcept {
  return {kImageSize, kPhaseGateRva, kSharedRaceEnterRva,
          kDerivedRaceEnterRva, kRacingStoreRva, nullptr, 0,
          kVptrMinimumRva, kVptrMaximumRva};
}

inline bool ProfileValid(const Profile& profile) noexcept {
  const auto aligned_rva = [&](std::uintptr_t rva) {
    return rva >= 0x1000 && rva < profile.image_size && (rva & 3u) == 0;
  };
  if (profile.image_size < 0x100000 ||
      !aligned_rva(profile.phase_gate_rva) ||
      !aligned_rva(profile.shared_race_enter_rva) ||
      !aligned_rva(profile.derived_race_enter_rva) ||
      !aligned_rva(profile.racing_store_rva))
    return false;
  if (profile.lifecycle_vtable_count != 0) {
    if (profile.lifecycle_vtable_rvas == nullptr ||
        profile.lifecycle_vtable_count > 4096)
      return false;
    for (std::size_t index = 0; index < profile.lifecycle_vtable_count;
         ++index) {
      const std::uintptr_t rva = profile.lifecycle_vtable_rvas[index];
      if (!aligned_rva(rva) ||
          (index != 0 && rva <= profile.lifecycle_vtable_rvas[index - 1]))
        return false;
    }
    return true;
  }
  return profile.fallback_vptr_minimum_rva >= 0x1000 &&
         profile.fallback_vptr_minimum_rva <=
             profile.fallback_vptr_maximum_rva &&
         profile.fallback_vptr_maximum_rva < profile.image_size;
}

inline bool VtableAllowed(const Profile& profile,
                          std::uintptr_t rva) noexcept {
  if (profile.lifecycle_vtable_count == 0)
    return rva >= profile.fallback_vptr_minimum_rva &&
           rva <= profile.fallback_vptr_maximum_rva;
  return std::binary_search(
      profile.lifecycle_vtable_rvas,
      profile.lifecycle_vtable_rvas + profile.lifecycle_vtable_count, rva);
}

inline constexpr std::uint8_t kPhaseGateBytes[] = {
    0x08, 0xD8, 0x42, 0xB9, 0x1F, 0x09, 0x00, 0x71,
    0x81, 0x00, 0x00, 0x54, 0x08, 0x00, 0x40, 0xF9,
    0x01, 0xF1, 0x40, 0xF9, 0x20, 0x00, 0x1F, 0xD6,
    0xC0, 0x03, 0x5F, 0xD6,
};

inline constexpr std::uint8_t kRacingStoreContextBytes[] = {
    0x60, 0x7E, 0x40, 0xF9, 0x61, 0x00, 0x80, 0x52,
    0x61, 0xDA, 0x02, 0xB9, 0x20, 0x02, 0x00, 0xB4,
};

inline bool ReadExact(int fd, std::uintptr_t address, void* output,
                      std::size_t size) {
  auto* cursor = static_cast<std::uint8_t*>(output);
  std::size_t done = 0;
  while (done < size) {
    const ssize_t amount =
        pread(fd, cursor + done, size - done,
              static_cast<off_t>(address + done));
    if (amount <= 0) return false;
    done += static_cast<std::size_t>(amount);
  }
  return true;
}

inline bool ReadMaps(pid_t pid, std::vector<Mapping>* output) {
  if (pid <= 0 || output == nullptr) return false;
  char path[64]{};
  std::snprintf(path, sizeof(path), "/proc/%d/maps", static_cast<int>(pid));
  FILE* file = std::fopen(path, "re");
  if (file == nullptr) return false;
  std::vector<Mapping> mappings;
  char line[2048]{};
  while (std::fgets(line, sizeof(line), file) != nullptr) {
    unsigned long long begin = 0, end = 0, offset = 0;
    char perms[5]{};
    char name[1400]{};
    const int fields = std::sscanf(
        line, "%llx-%llx %4s %llx %*s %*s %1399[^\n]", &begin, &end,
        perms, &offset, name);
    if (fields < 4 || begin >= end) continue;
    Mapping mapping{};
    mapping.begin = static_cast<std::uintptr_t>(begin);
    mapping.end = static_cast<std::uintptr_t>(end);
    std::memcpy(mapping.perms, perms, sizeof(mapping.perms));
    if (fields == 5) {
      const char* clean = name;
      while (*clean == ' ') ++clean;
      mapping.path = clean;
    }
    mappings.push_back(std::move(mapping));
  }
  std::fclose(file);
  *output = std::move(mappings);
  return !output->empty();
}

inline bool VerifyTargetBuild(int mem, std::uintptr_t base,
                              const Profile& profile) {
  if (!ProfileValid(profile) || base == 0 ||
      base > UINTPTR_MAX - profile.image_size)
    return false;
  std::uint8_t gate[sizeof(kPhaseGateBytes)]{};
  std::uint8_t transition[sizeof(kRacingStoreContextBytes)]{};
  return ReadExact(mem, base + profile.phase_gate_rva, gate, sizeof(gate)) &&
         ReadExact(mem, base + profile.racing_store_rva - 8, transition,
                   sizeof(transition)) &&
         std::memcmp(gate, kPhaseGateBytes, sizeof(gate)) == 0 &&
         std::memcmp(transition, kRacingStoreContextBytes,
                     sizeof(transition)) == 0;
}

inline bool ValidateCandidate(int mem, std::uintptr_t base,
                              std::uintptr_t object, std::uintptr_t vptr,
                              const Profile& profile,
                              Candidate* output) {
  if (output == nullptr || base == 0 || object == 0 ||
      vptr < base || !VtableAllowed(profile, vptr - base) ||
      ((vptr - base) & (alignof(std::uintptr_t) - 1)) != 0 ||
      object > UINTPTR_MAX - kPhaseStateOffset - 5)
    return false;
  std::uintptr_t gate = 0;
  std::uintptr_t enter = 0;
  std::uint32_t state = 0;
  std::uint8_t flags[5]{};
  if (!ReadExact(mem, vptr + kPhaseGateSlot, &gate, sizeof(gate)) ||
      !ReadExact(mem, vptr + kPhaseEnterSlot, &enter, sizeof(enter)) ||
      !ReadExact(mem, object + kPhaseStateOffset, &state, sizeof(state)) ||
      !ReadExact(mem, object + kPhaseStateOffset + 4, flags,
                 sizeof(flags)) ||
      gate != base + profile.phase_gate_rva ||
      (enter != base + profile.shared_race_enter_rva &&
       enter != base + profile.derived_race_enter_rva) ||
      state > 7)
    return false;
  Candidate candidate{};
  candidate.object = object;
  candidate.vptr = vptr;
  candidate.phase_enter = enter;
  candidate.state_address = object + kPhaseStateOffset;
  candidate.state = state;
  std::memcpy(candidate.flags, flags, sizeof(flags));
  *output = candidate;
  return true;
}

inline bool ResolveCountdownObject(pid_t pid, int mem, std::uintptr_t base,
                                   const Profile& profile,
                                   Resolution* output) {
  if (output == nullptr || !VerifyTargetBuild(mem, base, profile)) return false;
  std::vector<Mapping> mappings;
  if (!ReadMaps(pid, &mappings)) return false;
  constexpr std::size_t kChunkSize = 1u << 20;
  std::vector<std::uint8_t> buffer(kChunkSize);
  Resolution result{};
  for (const Mapping& mapping : mappings) {
    if (mapping.perms[0] != 'r' || mapping.perms[1] != 'w' ||
        mapping.perms[3] != 'p')
      continue;
    for (std::uintptr_t cursor = mapping.begin; cursor < mapping.end;) {
      const std::size_t size = static_cast<std::size_t>(
          std::min<std::uintptr_t>(kChunkSize, mapping.end - cursor));
      // One unreadable page in an advertised private mapping must not hide
      // every later race object (seen on virtualized Android memory layouts).
      // Retry at page granularity, then advance past just the unreadable page.
      std::size_t readable_size = size;
      if (!ReadExact(mem, cursor, buffer.data(), readable_size)) {
        readable_size = std::min<std::size_t>(4096, size);
        if (!ReadExact(mem, cursor, buffer.data(), readable_size)) {
          cursor += readable_size;
          continue;
        }
      }
      result.scanned_bytes += readable_size;
      for (std::size_t offset = 0;
           offset + sizeof(std::uintptr_t) <= readable_size;
           offset += sizeof(std::uintptr_t)) {
        std::uintptr_t vptr = 0;
        std::memcpy(&vptr, buffer.data() + offset, sizeof(vptr));
        if (vptr < base || !VtableAllowed(profile, vptr - base))
          continue;
        Candidate candidate{};
        if (!ValidateCandidate(mem, base, cursor + offset, vptr, profile,
                               &candidate))
          continue;
        ++result.structurally_valid;
        if (candidate.state != kCountdownState) continue;
        ++result.countdown_candidates;
        if (result.countdown_candidates == 1) result.selected = candidate;
      }
      cursor += readable_size;
    }
  }
  *output = result;
  return result.countdown_candidates == 1;
}

inline bool VerifyTargetBuild(int mem, std::uintptr_t base) {
  return VerifyTargetBuild(mem, base, ReferenceProfile());
}

inline bool ValidateCandidate(int mem, std::uintptr_t base,
                              std::uintptr_t object, std::uintptr_t vptr,
                              Candidate* output) {
  return ValidateCandidate(mem, base, object, vptr, ReferenceProfile(),
                           output);
}

inline bool ResolveCountdownObject(pid_t pid, int mem, std::uintptr_t base,
                                   Resolution* output) {
  return ResolveCountdownObject(pid, mem, base, ReferenceProfile(), output);
}

}  // namespace a9tas::race_lifecycle_v1
