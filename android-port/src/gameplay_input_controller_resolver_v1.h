#pragma once

// Read-only structural resolver for the live GameplayInputController.
// A bare vptr scan is insufficient: every candidate must also own a readable
// controller+0x48 source pointer whose vptr is the exact keyboard-source
// address point for the pinned game build.  Resolution succeeds only for one
// candidate across a sorted, non-overlapping mapping snapshot.

#include "controller_shadow_coordinator_protocol_v1.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace a9tas::gameplay_input_controller_resolver_v1 {

namespace protocol = a9tas::controller_shadow_coordinator_v1;

using ReadFn = bool (*)(void*, std::uintptr_t, void*, std::size_t);

struct Backend {
  void* context{};
  ReadFn read{};
};

struct Mapping {
  std::uintptr_t begin{};
  std::uintptr_t end{};
  bool readable{};
  bool writable{};
  bool executable{};
};

struct Resolution {
  std::uintptr_t controller{};
  std::uintptr_t source{};
  std::uintptr_t controller_vptr{};
  std::uintptr_t source_vptr{};
  std::uint64_t bytes_scanned{};
  std::uint32_t structural_candidates{};
  std::uint32_t reserved{};
  std::uint64_t failure_mapping_index{UINT64_MAX};
  std::uintptr_t failure_address{};
  std::uint64_t failure_size{};
};

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kMappingSnapshotInvalid = -2,
  kAddressOverflow = -3,
  kReadFailed = -4,
  kNotFound = -5,
  kNotUnique = -6,
};

inline constexpr std::size_t kScanChunkSize = 1u << 20;
inline constexpr std::size_t kMaximumMappings = 4096;
inline constexpr std::uint64_t kMaximumScanBytes = 8ULL * 1024 * 1024 * 1024;

inline bool Add(std::uintptr_t base, std::uintptr_t offset,
                std::uintptr_t* output) noexcept {
  if (output == nullptr || base > UINTPTR_MAX - offset) return false;
  *output = base + offset;
  return true;
}

inline bool MappingSnapshotValid(const Mapping* mappings,
                                 std::size_t count) noexcept {
  if (mappings == nullptr || count == 0 || count > kMaximumMappings)
    return false;
  std::uintptr_t previous_end = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const Mapping& mapping = mappings[index];
    if (mapping.begin == 0 || mapping.begin >= mapping.end ||
        (mapping.begin & (alignof(std::uintptr_t) - 1u)) != 0 ||
        (mapping.end & (alignof(std::uintptr_t) - 1u)) != 0 ||
        (index != 0 && mapping.begin < previous_end))
      return false;
    previous_end = mapping.end;
  }
  return true;
}

inline const Mapping* MappingAt(const Mapping* mappings, std::size_t count,
                                std::uintptr_t address,
                                std::size_t size) noexcept {
  if (mappings == nullptr || size == 0 || address > UINTPTR_MAX - size)
    return nullptr;
  const std::uintptr_t end = address + size;
  for (std::size_t index = 0; index < count; ++index) {
    const Mapping& mapping = mappings[index];
    if (address >= mapping.begin && end <= mapping.end) return &mapping;
    if (mapping.begin > address) break;
  }
  return nullptr;
}

inline bool ObjectMapping(const Mapping* mapping) noexcept {
  return mapping != nullptr && mapping->readable && mapping->writable &&
         !mapping->executable;
}

inline Result ResolveAtAddressPoint(const Backend& backend,
                                    const Mapping* mappings,
                                    std::size_t mapping_count,
                                    std::uintptr_t game_base,
                                    std::uintptr_t controller_address_point_rva,
                                    Resolution* output) {
  if (backend.context == nullptr || backend.read == nullptr ||
      game_base == 0 || controller_address_point_rva == 0 ||
      output == nullptr)
    return Result::kInvalidArgument;
  *output = {};
  if (!MappingSnapshotValid(mappings, mapping_count))
    return Result::kMappingSnapshotInvalid;
  std::uintptr_t expected_controller_vptr = 0;
  std::uintptr_t expected_source_vptr = 0;
  if (!Add(game_base, controller_address_point_rva,
           &expected_controller_vptr) ||
      !Add(game_base, protocol::kKeyboardSourceAddressPointRva,
           &expected_source_vptr))
    return Result::kAddressOverflow;

  Resolution found{};
  std::vector<std::uint8_t> buffer(kScanChunkSize);
  std::uint64_t total_scannable = 0;
  for (std::size_t map_index = 0; map_index < mapping_count; ++map_index) {
    const Mapping& mapping = mappings[map_index];
    if (!ObjectMapping(&mapping)) continue;
    const std::uint64_t mapping_size = mapping.end - mapping.begin;
    if (mapping_size > kMaximumScanBytes - total_scannable)
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
        if (observed_vptr != expected_controller_vptr) continue;
        const std::uintptr_t candidate = cursor + offset;
        std::uintptr_t source_slot = 0;
        if (!Add(candidate, protocol::kControllerSourceOffset,
                 &source_slot))
          return Result::kAddressOverflow;
        if (!ObjectMapping(MappingAt(mappings, mapping_count, candidate,
                                     protocol::kControllerSourceOffset +
                                         sizeof(std::uintptr_t))))
          continue;
        std::uintptr_t source = 0;
        if (!backend.read(backend.context, source_slot, &source,
                          sizeof(source))) {
          found.failure_mapping_index = map_index;
          found.failure_address = source_slot;
          found.failure_size = sizeof(source);
          *output = found;
          return Result::kReadFailed;
        }
        if (source == 0 || source == candidate ||
            !ObjectMapping(MappingAt(mappings, mapping_count, source,
                                     sizeof(std::uintptr_t))))
          continue;
        std::uintptr_t source_vptr = 0;
        if (!backend.read(backend.context, source, &source_vptr,
                          sizeof(source_vptr))) {
          found.failure_mapping_index = map_index;
          found.failure_address = source;
          found.failure_size = sizeof(source_vptr);
          *output = found;
          return Result::kReadFailed;
        }
        if (source_vptr != expected_source_vptr) continue;
        ++found.structural_candidates;
        if (found.structural_candidates > 1) {
          *output = found;
          return Result::kNotUnique;
        }
        found.controller = candidate;
        found.source = source;
        found.controller_vptr = observed_vptr;
        found.source_vptr = source_vptr;
      }
      cursor += amount;
    }
  }
  if (found.structural_candidates == 0) {
    *output = found;
    return Result::kNotFound;
  }
  *output = found;
  return Result::kOk;
}

inline Result Resolve(const Backend& backend, const Mapping* mappings,
                      std::size_t mapping_count, std::uintptr_t game_base,
                      Resolution* output) {
  return ResolveAtAddressPoint(
      backend, mappings, mapping_count, game_base,
      protocol::kControllerAddressPointRva, output);
}

}  // namespace a9tas::gameplay_input_controller_resolver_v1
