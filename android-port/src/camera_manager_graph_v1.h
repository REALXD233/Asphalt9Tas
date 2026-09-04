#pragma once

// Bounded, read-only inspection of the concrete camera-manager object.
// Static analysis proves that sub_3A78448 allocates exactly 0x118 bytes and
// sub_3A784D0 installs the primary vptr at +0x0, the blended-camera wrapper at
// +0x100, a float scalar at +0x108, and the secondary interface at +0x110.
// No interface method is invoked and no unproven state offset is interpreted.

#include "camera_active_state_resolver_v1.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace a9tas::camera_manager_graph_v1 {

namespace active = camera_active_state_resolver_v1;

inline constexpr std::size_t kManagerSize = 0x118;
inline constexpr std::size_t kWrapperProofSize = 0x98;
inline constexpr std::uintptr_t kExpectedPrimaryVptrRva = 0x8176180;
inline constexpr std::uintptr_t kExpectedSecondaryVptrRva = 0x8176298;
inline constexpr std::uintptr_t kExpectedWrapperVptrRva = 0x9DF3FE8;
inline constexpr std::uintptr_t kExpectedSourceVptrRva = 0x9D5B768;
inline constexpr std::uintptr_t kExpectedCallbackNodeVptrRva = 0x81766D8;
inline constexpr std::uintptr_t kExpectedCallbackRva = 0x3A787D8;
struct Profile {
  std::uintptr_t primary_vptr_rva{};
  std::uintptr_t secondary_vptr_rva{};
  std::uintptr_t wrapper_vptr_rva{};
  std::uintptr_t source_vptr_rva{};
  std::uintptr_t callback_node_vptr_rva{};
  std::uintptr_t callback_rva{};
};
inline constexpr Profile ReferenceProfile() noexcept {
  return {kExpectedPrimaryVptrRva, kExpectedSecondaryVptrRva,
          kExpectedWrapperVptrRva, kExpectedSourceVptrRva,
          kExpectedCallbackNodeVptrRva, kExpectedCallbackRva};
}
inline constexpr std::uintptr_t kBlendedWrapperOffset = 0x100;
inline constexpr std::uintptr_t kScalarOffset = 0x108;
inline constexpr std::uintptr_t kSecondaryOffset = 0x110;
inline constexpr std::uintptr_t kWrapperOwnerOffsetA = 0x88;
inline constexpr std::uintptr_t kWrapperOwnerOffsetB = 0x90;

// +0xD8, +0xE8, +0xF0 and +0xF8 are constructor-written object/interface
// references. +0x100 is the constructor-written blended wrapper.
inline constexpr std::array<std::uintptr_t, 5> kPointerFieldOffsets = {
    0xD8, 0xE8, 0xF0, 0xF8, 0x100,
};
inline constexpr std::array<std::uintptr_t, 9> kReportedFieldOffsets = {
    0x0, 0x8, 0xD8, 0xE8, 0xF0, 0xF8, 0x100, 0x108, 0x110,
};
inline constexpr std::size_t kMaximumPointerDereferences =
    kPointerFieldOffsets.size();

static_assert(kManagerSize == 280);
static_assert(kSecondaryOffset + sizeof(std::uintptr_t) == kManagerSize);
static_assert(kWrapperOwnerOffsetB + sizeof(std::uintptr_t) ==
              kWrapperProofSize);

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
  kManagerMappingInvalid = -3,
  kAddressOverflow = -4,
  kReadFailed = -5,
  kPrimaryVptrMismatch = -6,
  kSecondaryVptrMismatch = -7,
  kBlendedWrapperMismatch = -8,
  kWrapperMappingInvalid = -9,
  kWrapperVptrMismatch = -10,
  kWrapperOwnerMismatch = -11,
  kScalarInvalid = -12,
};

struct FieldNode {
  std::uintptr_t offset{};
  std::uintptr_t value{};
  std::uint32_t value_mapping_flags{};
  std::uintptr_t value_image_rva{};
  bool constructor_pointer_field{};
  std::uintptr_t pointee_head{};
  std::uint32_t pointee_head_mapping_flags{};
  std::uintptr_t pointee_head_image_rva{};
};

struct Graph {
  std::uintptr_t manager{};
  std::uintptr_t primary_vptr{};
  std::uintptr_t secondary_interface{};
  std::uintptr_t secondary_vptr{};
  std::uintptr_t blended_wrapper{};
  std::uintptr_t wrapper_vptr{};
  std::uintptr_t wrapper_owner_a{};
  std::uintptr_t wrapper_owner_b{};
  float scalar_108{};
  std::array<FieldNode, kReportedFieldOffsets.size()> fields{};
  std::uint32_t mapped_constructor_pointer_count{};
  std::uint32_t private_object_pointer_count{};
  std::uint32_t object_head_image_pointer_count{};
  std::uint32_t pointer_dereference_count{};
};

struct DirectManagerDiagnostics {
  std::uint32_t manager_primary_matches{};
  std::uint32_t source_vptr_matches{};
  std::uint32_t callback_node_vptr_matches{};
  std::uint32_t callback_function_matches{};
  std::uint32_t unreadable_mapping_chunks{};
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

inline bool IsPointerField(std::uintptr_t offset) noexcept {
  for (const std::uintptr_t candidate : kPointerFieldOffsets) {
    if (offset == candidate) return true;
  }
  return false;
}

inline bool ExpectedAddress(std::uintptr_t game_base, std::uintptr_t rva,
                            std::uintptr_t* output) noexcept {
  return active::Add(game_base, rva, output);
}

inline Result Build(const active::Backend& backend,
                    const active::Mapping* mappings,
                    std::size_t mapping_count, std::uintptr_t game_base,
                    std::uintptr_t game_image_end,
                    const active::Resolution& resolution,
                    const Profile& profile, Graph* output) {
  if (backend.context == nullptr || backend.read == nullptr ||
      game_base == 0 || game_image_end <= game_base || output == nullptr ||
      resolution.camera_manager == 0 || resolution.blended_camera == 0)
    return Result::kInvalidArgument;
  *output = {};
  if (!active::MappingSnapshotValid(mappings, mapping_count))
    return Result::kMappingSnapshotInvalid;
  if (!active::ObjectMapping(active::MappingAt(
          mappings, mapping_count, resolution.camera_manager, kManagerSize)))
    return Result::kManagerMappingInvalid;

  std::uintptr_t expected_primary = 0;
  std::uintptr_t expected_secondary = 0;
  std::uintptr_t expected_wrapper_vptr = 0;
  std::uintptr_t expected_secondary_interface = 0;
  if (!ExpectedAddress(game_base, profile.primary_vptr_rva,
                        &expected_primary) ||
      !ExpectedAddress(game_base, profile.secondary_vptr_rva,
                        &expected_secondary) ||
      !ExpectedAddress(game_base, profile.wrapper_vptr_rva,
                        &expected_wrapper_vptr) ||
      !active::Add(resolution.camera_manager, kSecondaryOffset,
                   &expected_secondary_interface))
    return Result::kAddressOverflow;

  std::array<std::uint8_t, kManagerSize> manager_bytes{};
  if (!backend.read(backend.context, resolution.camera_manager,
                    manager_bytes.data(), manager_bytes.size()))
    return Result::kReadFailed;
  std::uintptr_t primary = 0;
  std::uintptr_t secondary = 0;
  std::uintptr_t blended = 0;
  float scalar = 0.0f;
  std::memcpy(&primary, manager_bytes.data(), sizeof(primary));
  std::memcpy(&blended, manager_bytes.data() + kBlendedWrapperOffset,
              sizeof(blended));
  std::memcpy(&scalar, manager_bytes.data() + kScalarOffset, sizeof(scalar));
  std::memcpy(&secondary, manager_bytes.data() + kSecondaryOffset,
              sizeof(secondary));
  if (primary != expected_primary) return Result::kPrimaryVptrMismatch;
  if (secondary != expected_secondary) return Result::kSecondaryVptrMismatch;
  if (blended != resolution.blended_camera)
    return Result::kBlendedWrapperMismatch;
  if (!std::isfinite(scalar)) return Result::kScalarInvalid;
  if (!active::ObjectMapping(active::MappingAt(
          mappings, mapping_count, blended, kWrapperProofSize)))
    return Result::kWrapperMappingInvalid;

  std::array<std::uint8_t, kWrapperProofSize> wrapper_bytes{};
  if (!backend.read(backend.context, blended, wrapper_bytes.data(),
                    wrapper_bytes.size()))
    return Result::kReadFailed;
  std::uintptr_t wrapper_vptr = 0;
  std::uintptr_t wrapper_owner_a = 0;
  std::uintptr_t wrapper_owner_b = 0;
  std::memcpy(&wrapper_vptr, wrapper_bytes.data(), sizeof(wrapper_vptr));
  std::memcpy(&wrapper_owner_a,
              wrapper_bytes.data() + kWrapperOwnerOffsetA,
              sizeof(wrapper_owner_a));
  std::memcpy(&wrapper_owner_b,
              wrapper_bytes.data() + kWrapperOwnerOffsetB,
              sizeof(wrapper_owner_b));
  if (wrapper_vptr != expected_wrapper_vptr ||
      resolution.camera_vptr != expected_wrapper_vptr)
    return Result::kWrapperVptrMismatch;
  if (wrapper_owner_a != expected_secondary_interface ||
      wrapper_owner_b != expected_secondary_interface)
    return Result::kWrapperOwnerMismatch;

  Graph graph{};
  graph.manager = resolution.camera_manager;
  graph.primary_vptr = primary;
  graph.secondary_interface = expected_secondary_interface;
  graph.secondary_vptr = secondary;
  graph.blended_wrapper = blended;
  graph.wrapper_vptr = wrapper_vptr;
  graph.wrapper_owner_a = wrapper_owner_a;
  graph.wrapper_owner_b = wrapper_owner_b;
  graph.scalar_108 = scalar;
  for (std::size_t index = 0; index < graph.fields.size(); ++index) {
    FieldNode& node = graph.fields[index];
    node.offset = kReportedFieldOffsets[index];
    std::memcpy(&node.value, manager_bytes.data() + node.offset,
                sizeof(node.value));
    node.constructor_pointer_field = IsPointerField(node.offset);
    const active::Mapping* value_mapping = nullptr;
    if (active::AlignedPointer(node.value)) {
      value_mapping = active::MappingAt(mappings, mapping_count, node.value,
                                        sizeof(std::uintptr_t));
    }
    node.value_mapping_flags = MappingFlags(value_mapping);
    ImageRva(node.value, game_base, game_image_end, &node.value_image_rva);
    if (!node.constructor_pointer_field || value_mapping == nullptr) continue;
    ++graph.mapped_constructor_pointer_count;
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

inline Result Build(const active::Backend& backend,
                    const active::Mapping* mappings,
                    std::size_t mapping_count, std::uintptr_t game_base,
                    std::uintptr_t game_image_end,
                    const active::Resolution& resolution, Graph* output) {
  return Build(backend, mappings, mapping_count, game_base, game_image_end,
               resolution, ReferenceProfile(), output);
}

// Some race modes use a different outer RaceView implementation while
// retaining the exact-build CameraManager and attachment-wrapper layout.  The
// installer only consumes the manager and its +0xE8 source object, so an exact
// manager graph is a stronger and less mode-dependent authority than the
// outer RaceView vptr.  This fallback scans only private RW object mappings and
// promotes a candidate only after Build() validates the complete proven graph.
inline active::Result ResolveDirectManager(
    const active::Backend& backend, const active::Mapping* mappings,
    std::size_t mapping_count, std::uintptr_t game_base,
    std::uintptr_t game_image_end, active::Resolution* output,
    DirectManagerDiagnostics* diagnostics, const Profile& profile) {
  if (backend.context == nullptr || backend.read == nullptr ||
      game_base == 0 || game_image_end <= game_base || output == nullptr ||
      diagnostics == nullptr)
    return active::Result::kInvalidArgument;
  *output = {};
  *diagnostics = {};
  if (!active::MappingSnapshotValid(mappings, mapping_count))
    return active::Result::kMappingSnapshotInvalid;

  std::uintptr_t expected_primary = 0;
  std::uintptr_t expected_source = 0;
  std::uintptr_t expected_node = 0;
  std::uintptr_t expected_callback = 0;
  if (!ExpectedAddress(game_base, profile.primary_vptr_rva,
                        &expected_primary) ||
      !ExpectedAddress(game_base, profile.source_vptr_rva,
                        &expected_source) ||
      !ExpectedAddress(game_base, profile.callback_node_vptr_rva,
                        &expected_node) ||
      !ExpectedAddress(game_base, profile.callback_rva,
                        &expected_callback))
    return active::Result::kAddressOverflow;

  active::Resolution found{};
  std::array<std::uint8_t, active::kScanChunkSize> buffer{};
  std::uint64_t total_scannable = 0;
  for (std::size_t map_index = 0; map_index < mapping_count; ++map_index) {
    const active::Mapping& mapping = mappings[map_index];
    // Count exact identities in every writable non-executable mapping for
    // diagnostics.  Promotion below remains restricted to private object
    // mappings; a shared hit is evidence, never write authority.
    if (!active::Readable(&mapping) || !mapping.writable ||
        mapping.executable)
      continue;
    const std::uint64_t mapping_size = mapping.end - mapping.begin;
    if (mapping_size > active::kMaximumScanBytes - total_scannable)
      return active::Result::kMappingSnapshotInvalid;
    total_scannable += mapping_size;
    for (std::uintptr_t cursor = mapping.begin; cursor < mapping.end;) {
      const std::size_t amount = static_cast<std::size_t>(
          std::min<std::uintptr_t>(buffer.size(), mapping.end - cursor));
      if (!backend.read(backend.context, cursor, buffer.data(), amount)) {
        ++diagnostics->unreadable_mapping_chunks;
        found.failure_mapping_index = map_index;
        found.failure_address = cursor;
        found.failure_size = amount;
        break;
      }
      found.bytes_scanned += amount;
      for (std::size_t offset = 0;
           offset + sizeof(std::uintptr_t) <= amount;
           offset += alignof(std::uintptr_t)) {
        std::uintptr_t observed_primary = 0;
        std::memcpy(&observed_primary, buffer.data() + offset,
                    sizeof(observed_primary));
        if (observed_primary == expected_source)
          ++diagnostics->source_vptr_matches;
        if (observed_primary == expected_node)
          ++diagnostics->callback_node_vptr_matches;
        if (observed_primary == expected_callback)
          ++diagnostics->callback_function_matches;
        if (observed_primary != expected_primary) continue;
        ++diagnostics->manager_primary_matches;
        const std::uintptr_t manager = cursor + offset;
        if (!active::ObjectMapping(active::MappingAt(
                mappings, mapping_count, manager, kManagerSize)))
          continue;
        std::uintptr_t wrapper_slot = 0;
        if (!active::Add(manager, kBlendedWrapperOffset, &wrapper_slot))
          return active::Result::kAddressOverflow;
        std::uintptr_t wrapper = 0;
        if (!backend.read(backend.context, wrapper_slot, &wrapper,
                          sizeof(wrapper))) {
          found.failure_mapping_index = map_index;
          found.failure_address = wrapper_slot;
          found.failure_size = sizeof(wrapper);
          *output = found;
          return active::Result::kReadFailed;
        }
        if (!active::AlignedPointer(wrapper) ||
            !active::ObjectMapping(active::MappingAt(
                mappings, mapping_count, wrapper, kWrapperProofSize)))
          continue;
        std::uintptr_t wrapper_vptr = 0;
        if (!backend.read(backend.context, wrapper, &wrapper_vptr,
                          sizeof(wrapper_vptr))) {
          found.failure_mapping_index = map_index;
          found.failure_address = wrapper;
          found.failure_size = sizeof(wrapper_vptr);
          *output = found;
          return active::Result::kReadFailed;
        }
        active::Resolution candidate{};
        candidate.camera_manager = manager;
        candidate.blended_camera = wrapper;
        candidate.camera_vptr = wrapper_vptr;
        Graph proof{};
        if (Build(backend, mappings, mapping_count, game_base,
                  game_image_end, candidate, profile, &proof) != Result::kOk)
          continue;
        ++found.structural_candidates;
        if (found.structural_candidates > 1) {
          *output = found;
          return active::Result::kNotUnique;
        }
        found.camera_manager = manager;
        found.blended_camera = wrapper;
        found.camera_vptr = wrapper_vptr;
      }
      cursor += amount;
    }
  }
  if (found.structural_candidates == 0) {
    *output = found;
    return diagnostics->unreadable_mapping_chunks == 0
               ? active::Result::kNotFound
               : active::Result::kReadFailed;
  }
  if (diagnostics->unreadable_mapping_chunks != 0) {
    *output = found;
    return active::Result::kReadFailed;
  }
  *output = found;
  return active::Result::kOk;
}

inline active::Result ResolveDirectManager(
    const active::Backend& backend, const active::Mapping* mappings,
    std::size_t mapping_count, std::uintptr_t game_base,
    std::uintptr_t game_image_end, active::Resolution* output,
    DirectManagerDiagnostics* diagnostics) {
  return ResolveDirectManager(backend, mappings, mapping_count, game_base,
                              game_image_end, output, diagnostics,
                              ReferenceProfile());
}

}  // namespace a9tas::camera_manager_graph_v1
