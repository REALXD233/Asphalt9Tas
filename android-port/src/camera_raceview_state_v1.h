#pragma once

// Exact read-only RaceView camera layout proven from sub_3A784D0 and
// sub_3A787D8. The callback updates the embedded transform first and stores FOV
// to +0x108 last. The +0xE8 shape is decoded by camera_shape_state_v1.

#include "camera_shape_state_v1.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace a9tas::camera_raceview_state_v1 {

namespace active = camera_active_state_resolver_v1;
namespace shape = camera_shape_state_v1;

inline constexpr std::size_t kManagerSize = 0x118;
inline constexpr std::uintptr_t kExpectedPrimaryVptrRva = 0x8176180;
inline constexpr std::uintptr_t kEmbeddedVptrOffset = 0x08;
inline constexpr std::uintptr_t kExpectedEmbeddedVptrRva = 0x7F0F8A8;
inline constexpr std::uintptr_t kLocalStateOffset = 0x10;
inline constexpr std::uintptr_t kWorldStateOffset = 0x2C;
inline constexpr std::uintptr_t kShapePointerOffset = 0xE8;
inline constexpr std::uintptr_t kFovOffset = 0x108;
inline constexpr std::uintptr_t kSecondaryVptrOffset = 0x110;
inline constexpr std::uintptr_t kExpectedSecondaryVptrRva = 0x8176298;

static_assert(kLocalStateOffset + sizeof(shape::Transform28) ==
              kWorldStateOffset);
static_assert(kFovOffset + sizeof(float) <= kSecondaryVptrOffset);
static_assert(kSecondaryVptrOffset + sizeof(std::uintptr_t) == kManagerSize);

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kMappingSnapshotInvalid = -2,
  kManagerMappingInvalid = -3,
  kAddressOverflow = -4,
  kReadFailed = -5,
  kPrimaryVptrMismatch = -6,
  kEmbeddedVptrMismatch = -7,
  kSecondaryVptrMismatch = -8,
  kShapePointerMismatch = -9,
  kManagerStateInvalid = -10,
  kFovInvalid = -11,
  kShapeReadFailed = -12,
};

struct Snapshot {
  std::uintptr_t manager{};
  std::uintptr_t primary_vptr{};
  std::uintptr_t embedded_vptr{};
  shape::Transform28 local_state{};
  shape::Transform28 world_state{};
  std::uintptr_t shape_object{};
  float fov_radians{};
  std::uintptr_t secondary_vptr{};
  shape::Snapshot shape_state{};
};

inline bool PlausibleFov(float value) noexcept {
  return std::isfinite(value) && value > 0.05f && value < 3.13f;
}

inline Result ReadSnapshot(const active::Backend& backend,
                           const active::Mapping* mappings,
                           std::size_t mapping_count,
                           std::uintptr_t game_base,
                           std::uintptr_t manager,
                           std::uintptr_t expected_shape,
                           Snapshot* output) {
  if (backend.context == nullptr || backend.read == nullptr || game_base == 0 ||
      manager == 0 || expected_shape == 0 || output == nullptr)
    return Result::kInvalidArgument;
  *output = {};
  if (!active::MappingSnapshotValid(mappings, mapping_count))
    return Result::kMappingSnapshotInvalid;
  if (!active::ObjectMapping(active::MappingAt(
          mappings, mapping_count, manager, kManagerSize)))
    return Result::kManagerMappingInvalid;

  std::uintptr_t expected_primary = 0;
  std::uintptr_t expected_embedded = 0;
  std::uintptr_t expected_secondary = 0;
  if (!active::Add(game_base, kExpectedPrimaryVptrRva, &expected_primary) ||
      !active::Add(game_base, kExpectedEmbeddedVptrRva, &expected_embedded) ||
      !active::Add(game_base, kExpectedSecondaryVptrRva,
                   &expected_secondary))
    return Result::kAddressOverflow;

  std::array<std::uint8_t, kManagerSize> bytes{};
  if (!backend.read(backend.context, manager, bytes.data(), bytes.size()))
    return Result::kReadFailed;

  Snapshot snapshot{};
  snapshot.manager = manager;
  std::memcpy(&snapshot.primary_vptr, bytes.data(),
              sizeof(snapshot.primary_vptr));
  std::memcpy(&snapshot.embedded_vptr, bytes.data() + kEmbeddedVptrOffset,
              sizeof(snapshot.embedded_vptr));
  std::memcpy(&snapshot.local_state, bytes.data() + kLocalStateOffset,
              sizeof(snapshot.local_state));
  std::memcpy(&snapshot.world_state, bytes.data() + kWorldStateOffset,
              sizeof(snapshot.world_state));
  std::memcpy(&snapshot.shape_object, bytes.data() + kShapePointerOffset,
              sizeof(snapshot.shape_object));
  std::memcpy(&snapshot.fov_radians, bytes.data() + kFovOffset,
              sizeof(snapshot.fov_radians));
  std::memcpy(&snapshot.secondary_vptr,
              bytes.data() + kSecondaryVptrOffset,
              sizeof(snapshot.secondary_vptr));

  if (snapshot.primary_vptr != expected_primary)
    return Result::kPrimaryVptrMismatch;
  if (snapshot.embedded_vptr != expected_embedded)
    return Result::kEmbeddedVptrMismatch;
  if (snapshot.secondary_vptr != expected_secondary)
    return Result::kSecondaryVptrMismatch;
  if (snapshot.shape_object != expected_shape)
    return Result::kShapePointerMismatch;
  if (!shape::FiniteTransform(snapshot.local_state) ||
      !shape::PlausibleQuaternion(snapshot.local_state) ||
      !shape::FiniteTransform(snapshot.world_state) ||
      !shape::PlausibleQuaternion(snapshot.world_state))
    return Result::kManagerStateInvalid;
  if (!PlausibleFov(snapshot.fov_radians)) return Result::kFovInvalid;

  const shape::Result shape_result = shape::ReadSnapshot(
      backend, mappings, mapping_count, game_base, snapshot.shape_object,
      &snapshot.shape_state);
  if (shape_result != shape::Result::kOk) return Result::kShapeReadFailed;
  *output = snapshot;
  return Result::kOk;
}

}  // namespace a9tas::camera_raceview_state_v1
