#pragma once

// Read-only resolver for RaceView and its camera-attachment wrapper. The
// structural RaceView -> manager -> +0x100 route is live-proven, but the
// +0x100 object is a 0x98 asynchronous attachment wrapper, not a proven camera
// state object. The historical +0x28/+0xF0 state hypotheses below are retained
// only for evidence compatibility and must not be used by a replay writer.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace a9tas::camera_active_state_resolver_v1 {

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
  // C++ gameplay objects and their owning pointers are process-private.  A
  // shared IPC mapping must never be treated as an object arena.
  bool private_mapping{true};
};

inline constexpr std::uintptr_t kRaceViewPrimaryVptrRva = 0x8174F40;
inline constexpr std::uintptr_t kRaceViewSecondaryVptrRva = 0x8175108;
inline constexpr std::uintptr_t kRaceViewSecondaryOffset = 0x4A0;
inline constexpr std::uintptr_t kCameraManagerOffset = 0x4B8;
// Legacy name: static analysis now identifies this as the manager's
// asynchronous camera-attachment wrapper field.
inline constexpr std::uintptr_t kBlendedCameraOffset = 0x100;
inline constexpr std::uintptr_t kCameraVptrMinimumRva = 0x7EDF608;
inline constexpr std::uintptr_t kCameraVptrMaximumRva = 0xA4F9680;

// Build-dependent identities only. Object addresses remain lifecycle-owned
// and are rediscovered for every race.
struct Profile {
  std::uintptr_t race_view_primary_vptr_rva{};
  std::uintptr_t race_view_secondary_vptr_rva{};
  std::uintptr_t camera_vptr_minimum_rva{};
  std::uintptr_t camera_vptr_maximum_rva{};
};

inline constexpr Profile ReferenceProfile() noexcept {
  return {kRaceViewPrimaryVptrRva, kRaceViewSecondaryVptrRva,
          kCameraVptrMinimumRva, kCameraVptrMaximumRva};
}
inline constexpr std::uintptr_t kActualBasePointerOffset = 0x28;
inline constexpr std::uintptr_t kControllerStateOffset = 0xF0;
inline constexpr std::uintptr_t kCompactStateOffset = 0x30;
inline constexpr std::uintptr_t kPositionOffset = 0x38;
inline constexpr std::uintptr_t kRotationOffset = 0x44;
inline constexpr std::uintptr_t kUpstreamFovOffset = 0xF0;
inline constexpr std::uintptr_t kUpstreamAspectOffset = 0xF8;

inline constexpr std::size_t kScanChunkSize = 1u << 20;
// LDPlayer 9's NativeBridge process can legitimately exceed 4096 VMAs.  The
// live 2026-08-22 snapshot contained 6771 sorted, non-overlapping mappings.
// Keep a finite metadata bound, but let the independent byte budget below
// remain the effective limit on memory scanning.
inline constexpr std::size_t kMaximumMappings = 16384;
inline constexpr std::uint64_t kMaximumScanBytes =
    8ULL * 1024 * 1024 * 1024;

struct Resolution {
  std::uintptr_t race_view{};
  std::uintptr_t primary_vptr{};
  std::uintptr_t secondary_vptr{};
  std::uintptr_t camera_manager{};
  // Legacy field name: contains the 0x98 attachment wrapper, not a proven
  // camera transform/controller object.
  std::uintptr_t blended_camera{};
  std::uintptr_t camera_vptr{};
  // Historical hypothesis read from wrapper +0x28. The 2026-08-22 live graph
  // observed 0x1c05090000000000 here, disproving it as a pointer for this
  // Android build. Do not consume this field in replay code.
  std::uintptr_t actual_base_candidate{};
  std::uint64_t bytes_scanned{};
  std::uint32_t structural_candidates{};
  std::uint32_t reserved{};
  std::uint64_t failure_mapping_index{UINT64_MAX};
  std::uintptr_t failure_address{};
  std::uint64_t failure_size{};
};

struct CameraState {
  float fov{};
  float aspect{};
  float position[3]{};
  float rotation_xyzw[4]{};
};

static_assert(sizeof(CameraState) == 36);

struct StateSamples {
  CameraState controller_relative{};
  CameraState actual_base_compact{};
  CameraState actual_base_upstream{};
  std::uint32_t valid_mask{};
  std::uint32_t plausible_mask{};
};

inline constexpr std::uint32_t kControllerRelativeValid = 1u << 0;
inline constexpr std::uint32_t kActualBaseCompactValid = 1u << 1;
inline constexpr std::uint32_t kActualBaseUpstreamValid = 1u << 2;

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kMappingSnapshotInvalid = -2,
  kAddressOverflow = -3,
  kReadFailed = -4,
  kNotFound = -5,
  kNotUnique = -6,
  kStateUnavailable = -7,
};

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

inline bool Readable(const Mapping* mapping) noexcept {
  return mapping != nullptr && mapping->readable;
}

inline bool ObjectMapping(const Mapping* mapping) noexcept {
  return Readable(mapping) && mapping->writable && !mapping->executable &&
         mapping->private_mapping;
}

inline bool AlignedPointer(std::uintptr_t value) noexcept {
  return value != 0 &&
         (value & (alignof(std::uintptr_t) - 1u)) == 0;
}

inline bool Plausible(const CameraState& state) noexcept {
  const float values[] = {
      state.fov,          state.aspect,          state.position[0],
      state.position[1], state.position[2],     state.rotation_xyzw[0],
      state.rotation_xyzw[1], state.rotation_xyzw[2],
      state.rotation_xyzw[3],
  };
  for (float value : values) {
    if (!std::isfinite(value)) return false;
  }
  const float norm =
      state.rotation_xyzw[0] * state.rotation_xyzw[0] +
      state.rotation_xyzw[1] * state.rotation_xyzw[1] +
      state.rotation_xyzw[2] * state.rotation_xyzw[2] +
      state.rotation_xyzw[3] * state.rotation_xyzw[3];
  return state.fov > 0.0f && state.fov < 200.0f &&
         state.aspect > 0.2f && state.aspect < 5.0f && norm > 0.25f &&
         norm < 4.0f;
}

inline Result Resolve(const Backend& backend, const Mapping* mappings,
                      std::size_t mapping_count, std::uintptr_t game_base,
                      const Profile& profile, Resolution* output) {
  if (backend.context == nullptr || backend.read == nullptr ||
      game_base == 0 || output == nullptr)
    return Result::kInvalidArgument;
  *output = {};
  if (!MappingSnapshotValid(mappings, mapping_count))
    return Result::kMappingSnapshotInvalid;
  std::uintptr_t expected_primary = 0;
  std::uintptr_t expected_secondary = 0;
  std::uintptr_t camera_vptr_minimum = 0;
  std::uintptr_t camera_vptr_maximum = 0;
  if (!Add(game_base, profile.race_view_primary_vptr_rva, &expected_primary) ||
      !Add(game_base, profile.race_view_secondary_vptr_rva,
           &expected_secondary) ||
      !Add(game_base, profile.camera_vptr_minimum_rva,
           &camera_vptr_minimum) ||
      !Add(game_base, profile.camera_vptr_maximum_rva,
           &camera_vptr_maximum))
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
        std::uintptr_t observed_primary = 0;
        std::memcpy(&observed_primary, buffer.data() + offset,
                    sizeof(observed_primary));
        if (observed_primary != expected_primary) continue;
        const std::uintptr_t candidate = cursor + offset;
        std::uintptr_t secondary_slot = 0;
        std::uintptr_t manager_slot = 0;
        if (!Add(candidate, kRaceViewSecondaryOffset, &secondary_slot) ||
            !Add(candidate, kCameraManagerOffset, &manager_slot))
          return Result::kAddressOverflow;
        if (!ObjectMapping(MappingAt(mappings, mapping_count, candidate,
                                     kCameraManagerOffset +
                                         sizeof(std::uintptr_t))))
          continue;
        std::uintptr_t secondary = 0;
        std::uintptr_t manager = 0;
        if (!backend.read(backend.context, secondary_slot, &secondary,
                          sizeof(secondary)) ||
            !backend.read(backend.context, manager_slot, &manager,
                          sizeof(manager))) {
          found.failure_mapping_index = map_index;
          found.failure_address = secondary_slot;
          found.failure_size = sizeof(secondary) + sizeof(manager);
          *output = found;
          return Result::kReadFailed;
        }
        if (secondary != expected_secondary || !AlignedPointer(manager) ||
            !ObjectMapping(MappingAt(mappings, mapping_count, manager,
                                     kBlendedCameraOffset +
                                         sizeof(std::uintptr_t))))
          continue;
        std::uintptr_t blended_slot = 0;
        if (!Add(manager, kBlendedCameraOffset, &blended_slot))
          return Result::kAddressOverflow;
        std::uintptr_t blended = 0;
        if (!backend.read(backend.context, blended_slot, &blended,
                          sizeof(blended))) {
          found.failure_mapping_index = map_index;
          found.failure_address = blended_slot;
          found.failure_size = sizeof(blended);
          *output = found;
          return Result::kReadFailed;
        }
        if (!AlignedPointer(blended) ||
            !ObjectMapping(MappingAt(mappings, mapping_count, blended,
                                     kActualBasePointerOffset +
                                         sizeof(std::uintptr_t))))
          continue;
        std::uintptr_t camera_vptr = 0;
        std::uintptr_t actual_base = 0;
        if (!backend.read(backend.context, blended, &camera_vptr,
                          sizeof(camera_vptr)) ||
            !backend.read(backend.context,
                          blended + kActualBasePointerOffset, &actual_base,
                          sizeof(actual_base))) {
          found.failure_mapping_index = map_index;
          found.failure_address = blended;
          found.failure_size = kActualBasePointerOffset + sizeof(actual_base);
          *output = found;
          return Result::kReadFailed;
        }
        if (camera_vptr < camera_vptr_minimum ||
            camera_vptr >= camera_vptr_maximum)
          continue;
        ++found.structural_candidates;
        if (found.structural_candidates > 1) {
          *output = found;
          return Result::kNotUnique;
        }
        found.race_view = candidate;
        found.primary_vptr = observed_primary;
        found.secondary_vptr = secondary;
        found.camera_manager = manager;
        found.blended_camera = blended;
        found.camera_vptr = camera_vptr;
        found.actual_base_candidate = actual_base;
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
  return Resolve(backend, mappings, mapping_count, game_base,
                 ReferenceProfile(), output);
}

inline bool ReadContiguousState(const Backend& backend,
                                const Mapping* mappings,
                                std::size_t mapping_count,
                                std::uintptr_t address,
                                CameraState* output) noexcept {
  return output != nullptr &&
         Readable(MappingAt(mappings, mapping_count, address,
                            sizeof(CameraState))) &&
         backend.read(backend.context, address, output, sizeof(CameraState));
}

inline Result ReadStateSamples(const Backend& backend,
                               const Mapping* mappings,
                               std::size_t mapping_count,
                               const Resolution& resolution,
                               StateSamples* output) {
  // Evidence-only historical probe. Live evidence disproved the wrapper as
  // the upstream CameraUpdate object; this function is not authoritative.
  if (backend.context == nullptr || backend.read == nullptr ||
      output == nullptr || resolution.blended_camera == 0)
    return Result::kInvalidArgument;
  *output = {};
  std::uintptr_t address = 0;
  if (Add(resolution.blended_camera, kControllerStateOffset, &address) &&
      ReadContiguousState(backend, mappings, mapping_count, address,
                          &output->controller_relative))
    output->valid_mask |= kControllerRelativeValid;
  if (AlignedPointer(resolution.actual_base_candidate) &&
      Add(resolution.actual_base_candidate, kCompactStateOffset, &address) &&
      ReadContiguousState(backend, mappings, mapping_count, address,
                          &output->actual_base_compact))
    output->valid_mask |= kActualBaseCompactValid;

  CameraState upstream{};
  std::uintptr_t position = 0;
  std::uintptr_t rotation = 0;
  std::uintptr_t fov = 0;
  std::uintptr_t aspect = 0;
  if (AlignedPointer(resolution.actual_base_candidate) &&
      Add(resolution.actual_base_candidate, kPositionOffset, &position) &&
      Add(resolution.actual_base_candidate, kRotationOffset, &rotation) &&
      Add(resolution.actual_base_candidate, kUpstreamFovOffset, &fov) &&
      Add(resolution.actual_base_candidate, kUpstreamAspectOffset, &aspect) &&
      Readable(MappingAt(mappings, mapping_count, position,
                         sizeof(upstream.position))) &&
      Readable(MappingAt(mappings, mapping_count, rotation,
                         sizeof(upstream.rotation_xyzw))) &&
      Readable(MappingAt(mappings, mapping_count, fov, sizeof(float))) &&
      Readable(MappingAt(mappings, mapping_count, aspect, sizeof(float))) &&
      backend.read(backend.context, position, upstream.position,
                   sizeof(upstream.position)) &&
      backend.read(backend.context, rotation, upstream.rotation_xyzw,
                   sizeof(upstream.rotation_xyzw)) &&
      backend.read(backend.context, fov, &upstream.fov, sizeof(float)) &&
      backend.read(backend.context, aspect, &upstream.aspect,
                   sizeof(float))) {
    output->actual_base_upstream = upstream;
    output->valid_mask |= kActualBaseUpstreamValid;
  }
  if ((output->valid_mask & kControllerRelativeValid) != 0 &&
      Plausible(output->controller_relative))
    output->plausible_mask |= kControllerRelativeValid;
  if ((output->valid_mask & kActualBaseCompactValid) != 0 &&
      Plausible(output->actual_base_compact))
    output->plausible_mask |= kActualBaseCompactValid;
  if ((output->valid_mask & kActualBaseUpstreamValid) != 0 &&
      Plausible(output->actual_base_upstream))
    output->plausible_mask |= kActualBaseUpstreamValid;
  // A successful pread only proves that bytes exist.  Do not promote a
  // readable but nonsensical layout to a usable camera state.
  return output->plausible_mask == 0 ? Result::kStateUnavailable : Result::kOk;
}

}  // namespace a9tas::camera_active_state_resolver_v1
