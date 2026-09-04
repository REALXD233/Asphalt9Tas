#pragma once

// Bounded, read-only inspection of the live blended-camera JTL wrapper.
// Static analysis proves the concrete wrapper allocated by 0x595FE40 is 0x98
// bytes and installs primary vptr RVA 0x9DF3FE8.  This code deliberately
// reports a one-hop pointer graph; it does not guess camera-state offsets or
// invoke any interface method.

#include "camera_active_state_resolver_v1.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace a9tas::camera_wrapper_graph_v1 {

namespace active = camera_active_state_resolver_v1;

inline constexpr std::size_t kWrapperSize = 0x98;
inline constexpr std::size_t kWordCount = kWrapperSize / sizeof(std::uintptr_t);
inline constexpr std::size_t kMaximumPointerDereferences = kWordCount;
inline constexpr std::uintptr_t kExpectedPrimaryVptrRva = 0x9DF3FE8;
inline constexpr std::array<std::uintptr_t, 5> kKnownInterfaceVptrRvas = {
    0x9DF3FE8, 0x9DF4088, 0x9DF40D8, 0x9DF4128, 0x9DF4178,
};
inline constexpr std::array<std::uintptr_t, 4> kStaticCaptureOffsets = {
    0x78, 0x80, 0x88, 0x90,
};

static_assert(kWrapperSize == 152);
static_assert(kWordCount == 19);

enum MappingFlag : std::uint32_t {
  kMapped = 1u << 0,
  kReadable = 1u << 1,
  kWritable = 1u << 2,
  kExecutable = 1u << 3,
  kPrivate = 1u << 4,
};

enum class Result : std::int32_t {
  kOk = 0,
  kInvalidArgument = -1,
  kMappingSnapshotInvalid = -2,
  kWrapperMappingInvalid = -3,
  kPrimaryVptrMismatch = -4,
  kReadFailed = -5,
  kAddressOverflow = -6,
};

struct WordNode {
  std::uintptr_t offset{};
  std::uintptr_t value{};
  std::uint32_t value_mapping_flags{};
  bool value_is_known_interface_vptr{};
  bool statically_interesting{};
  std::uintptr_t value_image_rva{};
  std::uintptr_t pointee_head{};
  std::uint32_t pointee_head_mapping_flags{};
  std::uintptr_t pointee_head_image_rva{};
};

struct Graph {
  std::uintptr_t wrapper{};
  std::uintptr_t primary_vptr{};
  std::array<WordNode, kWordCount> words{};
  std::uint32_t mapped_pointer_count{};
  std::uint32_t private_object_pointer_count{};
  std::uint32_t known_interface_vptr_count{};
  std::uint32_t object_head_image_pointer_count{};
  std::uint32_t pointer_dereference_count{};
};

inline std::uint32_t MappingFlags(const active::Mapping* mapping) noexcept {
  if (mapping == nullptr) return 0;
  std::uint32_t flags = kMapped;
  if (mapping->readable) flags |= kReadable;
  if (mapping->writable) flags |= kWritable;
  if (mapping->executable) flags |= kExecutable;
  if (mapping->private_mapping) flags |= kPrivate;
  return flags;
}

inline bool ImageRva(std::uintptr_t value, std::uintptr_t game_base,
                     std::uintptr_t game_image_end,
                     std::uintptr_t* output) noexcept {
  if (output == nullptr || game_base == 0 || game_image_end <= game_base ||
      value < game_base || value >= game_image_end)
    return false;
  *output = value - game_base;
  return true;
}

inline bool IsKnownInterfaceVptr(std::uintptr_t value,
                                std::uintptr_t game_base) noexcept {
  for (const std::uintptr_t rva : kKnownInterfaceVptrRvas) {
    std::uintptr_t expected = 0;
    if (active::Add(game_base, rva, &expected) && value == expected)
      return true;
  }
  return false;
}

inline bool IsStaticCaptureOffset(std::uintptr_t offset) noexcept {
  for (const std::uintptr_t candidate : kStaticCaptureOffsets) {
    if (offset == candidate) return true;
  }
  return false;
}

inline Result Build(const active::Backend& backend,
                    const active::Mapping* mappings,
                    std::size_t mapping_count, std::uintptr_t game_base,
                    std::uintptr_t game_image_end,
                    const active::Resolution& resolution, Graph* output) {
  if (backend.context == nullptr || backend.read == nullptr ||
      game_base == 0 || game_image_end <= game_base || output == nullptr ||
      resolution.blended_camera == 0)
    return Result::kInvalidArgument;
  *output = {};
  if (!active::MappingSnapshotValid(mappings, mapping_count))
    return Result::kMappingSnapshotInvalid;
  const active::Mapping* wrapper_mapping = active::MappingAt(
      mappings, mapping_count, resolution.blended_camera, kWrapperSize);
  if (!active::ObjectMapping(wrapper_mapping))
    return Result::kWrapperMappingInvalid;
  std::uintptr_t expected_primary_vptr = 0;
  if (!active::Add(game_base, kExpectedPrimaryVptrRva,
                   &expected_primary_vptr))
    return Result::kAddressOverflow;
  if (resolution.camera_vptr != expected_primary_vptr)
    return Result::kPrimaryVptrMismatch;

  std::array<std::uint8_t, kWrapperSize> bytes{};
  if (!backend.read(backend.context, resolution.blended_camera, bytes.data(),
                    bytes.size()))
    return Result::kReadFailed;
  std::uintptr_t observed_primary_vptr = 0;
  std::memcpy(&observed_primary_vptr, bytes.data(),
              sizeof(observed_primary_vptr));
  if (observed_primary_vptr != expected_primary_vptr ||
      observed_primary_vptr != resolution.camera_vptr)
    return Result::kPrimaryVptrMismatch;

  Graph graph{};
  graph.wrapper = resolution.blended_camera;
  graph.primary_vptr = observed_primary_vptr;
  for (std::size_t index = 0; index < graph.words.size(); ++index) {
    WordNode& node = graph.words[index];
    node.offset = index * sizeof(std::uintptr_t);
    node.statically_interesting = IsStaticCaptureOffset(node.offset);
    std::memcpy(&node.value, bytes.data() + node.offset, sizeof(node.value));
    const active::Mapping* value_mapping = nullptr;
    if (active::AlignedPointer(node.value)) {
      value_mapping =
          active::MappingAt(mappings, mapping_count, node.value,
                            sizeof(std::uintptr_t));
    }
    node.value_mapping_flags = MappingFlags(value_mapping);
    if (value_mapping != nullptr) ++graph.mapped_pointer_count;
    node.value_is_known_interface_vptr =
        IsKnownInterfaceVptr(node.value, game_base);
    if (node.value_is_known_interface_vptr)
      ++graph.known_interface_vptr_count;
    ImageRva(node.value, game_base, game_image_end, &node.value_image_rva);

    // Only process-private writable targets can be owned/captured object
    // candidates.  Static image pointers, shared IPC, and arbitrary readable
    // data are never dereferenced by this bounded graph.
    if (!active::ObjectMapping(value_mapping)) continue;
    ++graph.private_object_pointer_count;
    ++graph.pointer_dereference_count;
    if (graph.pointer_dereference_count > kMaximumPointerDereferences)
      return Result::kInvalidArgument;
    if (!backend.read(backend.context, node.value, &node.pointee_head,
                      sizeof(node.pointee_head)))
      return Result::kReadFailed;
    const active::Mapping* head_mapping = nullptr;
    if (active::AlignedPointer(node.pointee_head)) {
      head_mapping = active::MappingAt(mappings, mapping_count,
                                       node.pointee_head,
                                       sizeof(std::uintptr_t));
    }
    node.pointee_head_mapping_flags = MappingFlags(head_mapping);
    if (ImageRva(node.pointee_head, game_base, game_image_end,
                 &node.pointee_head_image_rva))
      ++graph.object_head_image_pointer_count;
  }
  *output = graph;
  return Result::kOk;
}

}  // namespace a9tas::camera_wrapper_graph_v1
