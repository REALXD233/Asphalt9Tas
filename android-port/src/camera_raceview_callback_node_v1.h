#pragma once

// Exact read-only layout of the callback node created by sub_3A78B80 for
// RaceView's sub_3A787D8 update. The node is reachable through manager +0xE0.

#include "camera_active_state_resolver_v1.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace a9tas::camera_raceview_callback_node_v1 {

namespace active = camera_active_state_resolver_v1;

inline constexpr std::uintptr_t kManagerNodeOffset = 0xE0;
inline constexpr std::size_t kNodeSize = 0x68;
inline constexpr std::uintptr_t kExpectedNodeVptrRva = 0x81766D8;
inline constexpr std::uintptr_t kEnabledOffset = 0x40;
inline constexpr std::uintptr_t kOwnerOffset = 0x48;
inline constexpr std::uintptr_t kSelfOffset = 0x50;
inline constexpr std::uintptr_t kCallbackOffset = 0x58;
inline constexpr std::uintptr_t kExpectedCallbackRva = 0x3A787D8;
inline constexpr std::uintptr_t kContextOffset = 0x60;

static_assert(kContextOffset + sizeof(std::uintptr_t) == kNodeSize);

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kMappingSnapshotInvalid = -2,
  kManagerMappingInvalid = -3,
  kManagerReadFailed = -4,
  kNullNode = -5,
  kNodeMappingInvalid = -6,
  kNodeReadFailed = -7,
  kAddressOverflow = -8,
  kVptrMismatch = -9,
  kDisabled = -10,
  kOwnerMismatch = -11,
  kSelfMismatch = -12,
  kCallbackMismatch = -13,
  kContextMismatch = -14,
};

struct Snapshot {
  std::uintptr_t node{};
  std::uintptr_t vptr{};
  std::uint8_t enabled{};
  std::uintptr_t owner{};
  std::uintptr_t self{};
  std::uintptr_t callback{};
  std::uintptr_t context{};
};

inline Result ReadSnapshot(const active::Backend& backend,
                           const active::Mapping* mappings,
                           std::size_t mapping_count,
                           std::uintptr_t game_base,
                           std::uintptr_t manager,
                           Snapshot* output) {
  if (backend.context == nullptr || backend.read == nullptr || game_base == 0 ||
      manager == 0 || output == nullptr)
    return Result::kInvalidArgument;
  *output = {};
  if (!active::MappingSnapshotValid(mappings, mapping_count))
    return Result::kMappingSnapshotInvalid;

  std::uintptr_t node_slot = 0;
  if (!active::Add(manager, kManagerNodeOffset, &node_slot))
    return Result::kAddressOverflow;
  if (!active::ObjectMapping(active::MappingAt(
          mappings, mapping_count, node_slot, sizeof(std::uintptr_t))))
    return Result::kManagerMappingInvalid;
  std::uintptr_t node = 0;
  if (!backend.read(backend.context, node_slot, &node, sizeof(node)))
    return Result::kManagerReadFailed;
  if (node == 0) return Result::kNullNode;
  if (!active::ObjectMapping(
          active::MappingAt(mappings, mapping_count, node, kNodeSize)))
    return Result::kNodeMappingInvalid;

  std::array<std::uint8_t, kNodeSize> bytes{};
  if (!backend.read(backend.context, node, bytes.data(), bytes.size()))
    return Result::kNodeReadFailed;

  Snapshot snapshot{};
  snapshot.node = node;
  std::memcpy(&snapshot.vptr, bytes.data(), sizeof(snapshot.vptr));
  snapshot.enabled = bytes[kEnabledOffset];
  std::memcpy(&snapshot.owner, bytes.data() + kOwnerOffset,
              sizeof(snapshot.owner));
  std::memcpy(&snapshot.self, bytes.data() + kSelfOffset,
              sizeof(snapshot.self));
  std::memcpy(&snapshot.callback, bytes.data() + kCallbackOffset,
              sizeof(snapshot.callback));
  std::memcpy(&snapshot.context, bytes.data() + kContextOffset,
              sizeof(snapshot.context));

  std::uintptr_t expected_vptr = 0;
  std::uintptr_t expected_callback = 0;
  std::uintptr_t expected_self = 0;
  if (!active::Add(game_base, kExpectedNodeVptrRva, &expected_vptr) ||
      !active::Add(game_base, kExpectedCallbackRva, &expected_callback) ||
      !active::Add(node, kOwnerOffset, &expected_self))
    return Result::kAddressOverflow;
  if (snapshot.vptr != expected_vptr) return Result::kVptrMismatch;
  if (snapshot.enabled != 1) return Result::kDisabled;
  if (snapshot.owner != manager) return Result::kOwnerMismatch;
  if (snapshot.self != expected_self) return Result::kSelfMismatch;
  if (snapshot.callback != expected_callback)
    return Result::kCallbackMismatch;
  if (snapshot.context != 0) return Result::kContextMismatch;
  *output = snapshot;
  return Result::kOk;
}

}  // namespace a9tas::camera_raceview_callback_node_v1
