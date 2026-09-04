#pragma once

// Read-only decoding of the exact 0xF0 transform object held by the proven
// camera manager at +0xE8. Static construction at sub_37420EC allocates 0xF0;
// sub_3764AAC installs vptrs 0x7F225E0/+0 and 0x7F22728/+0xE8; and
// sub_37666AC copies 28-byte position+quaternion states to +0x40, +0x5C and
// +0x90. This component observes those fields only and never invokes methods.

#include "camera_active_state_resolver_v1.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace a9tas::camera_shape_state_v1 {

namespace active = camera_active_state_resolver_v1;

inline constexpr std::size_t kObjectSize = 0xF0;
inline constexpr std::uintptr_t kExpectedPrimaryVptrRva = 0x7F225E0;
inline constexpr std::uintptr_t kExpectedSecondaryVptrRva = 0x7F22728;
inline constexpr std::uintptr_t kStateAOffset = 0x40;
inline constexpr std::uintptr_t kStateBOffset = 0x5C;
inline constexpr std::uintptr_t kMotionPairOffset = 0x78;
inline constexpr std::uintptr_t kStateCOffset = 0x90;
inline constexpr std::uintptr_t kSecondaryVptrOffset = 0xE8;

struct Transform28 {
  float position[3]{};
  float quaternion_xyzw[4]{};
};

struct MotionPair24 {
  float first[3]{};
  float second[3]{};
};

static_assert(sizeof(Transform28) == 0x1C);
static_assert(sizeof(MotionPair24) == 0x18);
static_assert(kStateAOffset + sizeof(Transform28) == kStateBOffset);
static_assert(kStateBOffset + sizeof(Transform28) == kMotionPairOffset);
static_assert(kMotionPairOffset + sizeof(MotionPair24) == kStateCOffset);
static_assert(kSecondaryVptrOffset + sizeof(std::uintptr_t) == kObjectSize);

inline constexpr std::uint32_t kStateAValid = 1u << 0;
inline constexpr std::uint32_t kStateBValid = 1u << 1;
inline constexpr std::uint32_t kMotionPairValid = 1u << 2;
inline constexpr std::uint32_t kStateCValid = 1u << 3;

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kMappingSnapshotInvalid = -2,
  kObjectMappingInvalid = -3,
  kAddressOverflow = -4,
  kReadFailed = -5,
  kPrimaryVptrMismatch = -6,
  kSecondaryVptrMismatch = -7,
};

struct Snapshot {
  std::uintptr_t object{};
  std::uintptr_t primary_vptr{};
  Transform28 state_a{};
  Transform28 state_b{};
  MotionPair24 motion_pair{};
  Transform28 state_c{};
  std::uintptr_t secondary_vptr{};
  std::uint32_t finite_mask{};
  std::uint32_t quaternion_plausible_mask{};
};

inline bool FiniteTransform(const Transform28& state) noexcept {
  for (float value : state.position) {
    if (!std::isfinite(value)) return false;
  }
  for (float value : state.quaternion_xyzw) {
    if (!std::isfinite(value)) return false;
  }
  return true;
}

inline bool PlausibleQuaternion(const Transform28& state) noexcept {
  if (!FiniteTransform(state)) return false;
  float norm = 0.0f;
  for (float value : state.quaternion_xyzw) norm += value * value;
  return norm > 0.25f && norm < 4.0f;
}

inline bool FiniteMotion(const MotionPair24& motion) noexcept {
  for (float value : motion.first) {
    if (!std::isfinite(value)) return false;
  }
  for (float value : motion.second) {
    if (!std::isfinite(value)) return false;
  }
  return true;
}

inline Result ReadSnapshot(const active::Backend& backend,
                           const active::Mapping* mappings,
                           std::size_t mapping_count,
                           std::uintptr_t game_base,
                           std::uintptr_t object, Snapshot* output) {
  if (backend.context == nullptr || backend.read == nullptr ||
      game_base == 0 || object == 0 || output == nullptr)
    return Result::kInvalidArgument;
  *output = {};
  if (!active::MappingSnapshotValid(mappings, mapping_count))
    return Result::kMappingSnapshotInvalid;
  if (!active::ObjectMapping(active::MappingAt(
          mappings, mapping_count, object, kObjectSize)))
    return Result::kObjectMappingInvalid;
  std::uintptr_t expected_primary = 0;
  std::uintptr_t expected_secondary = 0;
  if (!active::Add(game_base, kExpectedPrimaryVptrRva, &expected_primary) ||
      !active::Add(game_base, kExpectedSecondaryVptrRva,
                   &expected_secondary))
    return Result::kAddressOverflow;
  std::array<std::uint8_t, kObjectSize> bytes{};
  if (!backend.read(backend.context, object, bytes.data(), bytes.size()))
    return Result::kReadFailed;

  Snapshot snapshot{};
  snapshot.object = object;
  std::memcpy(&snapshot.primary_vptr, bytes.data(),
              sizeof(snapshot.primary_vptr));
  std::memcpy(&snapshot.state_a, bytes.data() + kStateAOffset,
              sizeof(snapshot.state_a));
  std::memcpy(&snapshot.state_b, bytes.data() + kStateBOffset,
              sizeof(snapshot.state_b));
  std::memcpy(&snapshot.motion_pair, bytes.data() + kMotionPairOffset,
              sizeof(snapshot.motion_pair));
  std::memcpy(&snapshot.state_c, bytes.data() + kStateCOffset,
              sizeof(snapshot.state_c));
  std::memcpy(&snapshot.secondary_vptr,
              bytes.data() + kSecondaryVptrOffset,
              sizeof(snapshot.secondary_vptr));
  if (snapshot.primary_vptr != expected_primary)
    return Result::kPrimaryVptrMismatch;
  if (snapshot.secondary_vptr != expected_secondary)
    return Result::kSecondaryVptrMismatch;
  if (FiniteTransform(snapshot.state_a)) snapshot.finite_mask |= kStateAValid;
  if (FiniteTransform(snapshot.state_b)) snapshot.finite_mask |= kStateBValid;
  if (FiniteMotion(snapshot.motion_pair))
    snapshot.finite_mask |= kMotionPairValid;
  if (FiniteTransform(snapshot.state_c)) snapshot.finite_mask |= kStateCValid;
  if (PlausibleQuaternion(snapshot.state_a))
    snapshot.quaternion_plausible_mask |= kStateAValid;
  if (PlausibleQuaternion(snapshot.state_b))
    snapshot.quaternion_plausible_mask |= kStateBValid;
  if (PlausibleQuaternion(snapshot.state_c))
    snapshot.quaternion_plausible_mask |= kStateCValid;
  *output = snapshot;
  return Result::kOk;
}

inline bool Different(const Transform28& left,
                      const Transform28& right) noexcept {
  return std::memcmp(&left, &right, sizeof(left)) != 0;
}

inline bool Different(const MotionPair24& left,
                      const MotionPair24& right) noexcept {
  return std::memcmp(&left, &right, sizeof(left)) != 0;
}

}  // namespace a9tas::camera_shape_state_v1
