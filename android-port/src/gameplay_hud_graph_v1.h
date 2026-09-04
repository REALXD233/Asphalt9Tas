#pragma once

// Bounded read-only resolver for supported exact-build gameplay HUD instances.
// Static analysis proves:
//   * national 671522d4 ConstructGameplayHudScreen installs RVA 0x83FB0F0;
//   * Huawei 439fd7f9 ConstructGameplayHudScreen installs RVA 0x8433D78;
//   * the complete object occupies at least 0x4C0 bytes;
//   * +0x118 and +0x230 contain the same GUI owner;
//   * construction registers an adjusted HUD interface at owner +0x20.
// No method is called and this resolver has no write backend.

#include "camera_active_state_resolver_v1.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace a9tas::gameplay_hud_graph_v1 {

namespace active = camera_active_state_resolver_v1;

inline constexpr std::array<std::uintptr_t, 2> kGameplayHudVptrRvas{
    0x83FB0F0, 0x8433D78};
inline constexpr std::size_t kGameplayHudObjectSize = 0x4C0;
inline constexpr std::uintptr_t kGuiOwnerOffset = 0x118;
inline constexpr std::uintptr_t kGuiOwnerMirrorOffset = 0x230;
inline constexpr std::uintptr_t kRegisteredScreenOffset = 0x20;

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kMappingSnapshotInvalid = -2,
  kAddressOverflow = -3,
  kReadFailed = -4,
  kNotFound = -5,
  kNotUnique = -6,
};

struct Resolution {
  std::uintptr_t hud{};
  std::uintptr_t hud_vptr{};
  std::uintptr_t matched_hud_vptr_rva{};
  std::uintptr_t gui_owner{};
  std::uintptr_t gui_owner_vptr{};
  std::uintptr_t registered_screen{};
  std::uintptr_t registered_screen_vptr{};
  std::uintptr_t registered_screen_offset{UINTPTR_MAX};
  bool registered_screen_within_hud{};
  std::uint64_t bytes_scanned{};
  std::uint32_t structural_candidates{};
  std::uint64_t failure_mapping_index{UINT64_MAX};
  std::uintptr_t failure_address{};
  std::uint64_t failure_size{};
};

inline bool InGameImage(std::uintptr_t value, std::uintptr_t game_base,
                        std::uintptr_t game_end) noexcept {
  return value >= game_base && value < game_end;
}

inline Result Resolve(const active::Backend& backend,
                      const active::Mapping* mappings,
                      std::size_t mapping_count, std::uintptr_t game_base,
                      std::uintptr_t game_end, Resolution* output) {
  if (backend.context == nullptr || backend.read == nullptr ||
      game_base == 0 || game_end <= game_base || output == nullptr)
    return Result::kInvalidArgument;
  *output = {};
  if (!active::MappingSnapshotValid(mappings, mapping_count))
    return Result::kMappingSnapshotInvalid;
  std::array<std::uintptr_t, kGameplayHudVptrRvas.size()> expected_vptrs{};
  for (std::size_t index = 0; index < kGameplayHudVptrRvas.size(); ++index) {
    if (!active::Add(game_base, kGameplayHudVptrRvas[index],
                     &expected_vptrs[index]))
      return Result::kAddressOverflow;
  }

  Resolution found{};
  std::vector<std::uint8_t> buffer(active::kScanChunkSize);
  std::uint64_t total_scannable = 0;
  for (std::size_t map_index = 0; map_index < mapping_count; ++map_index) {
    const active::Mapping& mapping = mappings[map_index];
    if (!active::ObjectMapping(&mapping)) continue;
    const std::uint64_t mapping_size = mapping.end - mapping.begin;
    if (mapping_size > active::kMaximumScanBytes - total_scannable)
      return Result::kMappingSnapshotInvalid;
    total_scannable += mapping_size;
    for (std::uintptr_t cursor = mapping.begin; cursor < mapping.end;) {
      const std::size_t amount = static_cast<std::size_t>(
          std::min<std::uintptr_t>(buffer.size(), mapping.end - cursor));
      if (!backend.read(backend.context, cursor, buffer.data(), amount)) {
        found.failure_mapping_index = map_index;
        found.failure_address = cursor;
        found.failure_size = amount;
        *output = found;
        return Result::kReadFailed;
      }
      found.bytes_scanned += amount;
      for (std::size_t offset = 0;
           offset + sizeof(std::uintptr_t) <= amount;
           offset += alignof(std::uintptr_t)) {
        std::uintptr_t observed_vptr = 0;
        std::memcpy(&observed_vptr, buffer.data() + offset,
                    sizeof(observed_vptr));
        const auto expected = std::find(expected_vptrs.begin(),
                                        expected_vptrs.end(), observed_vptr);
        if (expected == expected_vptrs.end()) continue;
        const std::uintptr_t candidate = cursor + offset;
        if (!active::ObjectMapping(active::MappingAt(
                mappings, mapping_count, candidate, kGameplayHudObjectSize)))
          continue;
        std::uintptr_t owner = 0;
        std::uintptr_t owner_mirror = 0;
        if (!backend.read(backend.context, candidate + kGuiOwnerOffset,
                          &owner, sizeof(owner)) ||
            !backend.read(backend.context, candidate + kGuiOwnerMirrorOffset,
                          &owner_mirror, sizeof(owner_mirror))) {
          found.failure_mapping_index = map_index;
          found.failure_address = candidate;
          found.failure_size = kGameplayHudObjectSize;
          *output = found;
          return Result::kReadFailed;
        }
        if (owner == 0 || owner != owner_mirror ||
            !active::AlignedPointer(owner) ||
            !active::ObjectMapping(active::MappingAt(
                mappings, mapping_count, owner,
                kRegisteredScreenOffset + sizeof(std::uintptr_t))))
          continue;
        std::uintptr_t owner_vptr = 0;
        std::uintptr_t registered = 0;
        if (!backend.read(backend.context, owner, &owner_vptr,
                          sizeof(owner_vptr)) ||
            !backend.read(backend.context, owner + kRegisteredScreenOffset,
                          &registered, sizeof(registered))) {
          found.failure_mapping_index = map_index;
          found.failure_address = owner;
          found.failure_size = kRegisteredScreenOffset + sizeof(registered);
          *output = found;
          return Result::kReadFailed;
        }
        if (!InGameImage(owner_vptr, game_base, game_end) ||
            !active::AlignedPointer(registered) ||
            !active::ObjectMapping(active::MappingAt(
                mappings, mapping_count, registered,
                sizeof(std::uintptr_t))))
          continue;
        std::uintptr_t registered_vptr = 0;
        if (!backend.read(backend.context, registered, &registered_vptr,
                          sizeof(registered_vptr))) {
          found.failure_mapping_index = map_index;
          found.failure_address = registered;
          found.failure_size = sizeof(registered_vptr);
          *output = found;
          return Result::kReadFailed;
        }
        if (!InGameImage(registered_vptr, game_base, game_end)) continue;
        ++found.structural_candidates;
        if (found.structural_candidates > 1) {
          *output = found;
          return Result::kNotUnique;
        }
        found.hud = candidate;
        found.hud_vptr = observed_vptr;
        found.matched_hud_vptr_rva = kGameplayHudVptrRvas[
            static_cast<std::size_t>(expected - expected_vptrs.begin())];
        found.gui_owner = owner;
        found.gui_owner_vptr = owner_vptr;
        found.registered_screen = registered;
        found.registered_screen_vptr = registered_vptr;
        if (registered >= candidate &&
            registered < candidate + kGameplayHudObjectSize) {
          found.registered_screen_within_hud = true;
          found.registered_screen_offset = registered - candidate;
        }
      }
      cursor += amount;
    }
  }
  *output = found;
  return found.structural_candidates == 1 ? Result::kOk : Result::kNotFound;
}

}  // namespace a9tas::gameplay_hud_graph_v1
