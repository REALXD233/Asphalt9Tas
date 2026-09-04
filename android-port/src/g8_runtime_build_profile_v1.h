#pragma once

#include "g3_boundary_adapter_v1.h"
#include "g4_multi_hook_runtime_v1.h"
#include "physics_interval_getter_core_v2.h"

#include <cstddef>
#include <cstdint>

namespace a9tas::g8_runtime_build_profile_v1 {

namespace g3 = g3_boundary_adapter_v1;
namespace g4 = g4_multi_hook_runtime_v1;
namespace interval = physics_interval_v2;

inline constexpr char kMagic[8] = {
    'A', '9', 'B', 'P', 'R', '1', '\0', '\0',
};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kHookCount = g4::kHookCount;
inline constexpr std::uint32_t kSetterCount = g4::kInstalledSetterCount;

enum Flag : std::uint32_t {
  kFunctionSignaturesResolved = 1u << 0,
  kVtablesStructurallyResolved = 1u << 1,
  kLifecycleTopologyResolved = 1u << 2,
  kLiveObjectGraphRequired = 1u << 3,
  kChannelLiteralsAbsent = 1u << 4,
};
inline constexpr std::uint32_t kRequiredFlags =
    kFunctionSignaturesResolved | kVtablesStructurallyResolved |
    kLifecycleTopologyResolved | kLiveObjectGraphRequired |
    kChannelLiteralsAbsent;

// This is the single build-dependent ABI consumed by the frozen G8 runtime.
// Object addresses remain lifecycle-owned Control fields and are deliberately
// excluded.  The host must reacquire them in each new race.
struct alignas(64) Profile {
  char magic[8]{};
  std::uint32_t version{};
  std::uint32_t size{};
  std::uint32_t flags{};
  std::uint32_t reserved0{};
  std::uint64_t image_size{};
  std::uint64_t hook_rvas[kHookCount]{};
  std::uint64_t main_time_source_vtable_rva{};
  std::uint64_t embedded_time_source_vtable_rva{};
  std::uint64_t physics_context_vtable_rva{};
  std::uint64_t physics_implementation_vtable_rva{};
  std::uint64_t step_options_vtable_rva{};
  std::uint64_t native_physics_body_vtable_rva{};
  std::uint64_t nitro_service_vtable_rva{};
  std::uint64_t nitro_dispatch_rva{};
  std::uint64_t vehicle_source_vtable_rva{};
  std::uint64_t car_physics_body_source_vtable_rva{};
  std::uint64_t adjusted_setter_vtable_rva{};
  // Only Brake and Steering have installed vtable replacements. Accelerator
  // is observed after-original and deliberately has no build-profile entry.
  std::uint64_t setter_original_rvas[g4::kInstalledSetterCount]{};
  std::uint64_t frame_event_scheduler_return_rva{};
  std::uint64_t lifecycle_phase_gate_rva{};
  std::uint64_t lifecycle_shared_enter_rva{};
  std::uint64_t lifecycle_derived_enter_rva{};
  std::uint64_t lifecycle_racing_store_rva{};
  std::uint64_t barrel_random_bool_rva{};
  std::uint64_t barrel_random_lerp_rva{};
  std::uint8_t native_sha256[32]{};
  std::uint8_t profile_sha256[32]{};
  std::uint8_t build_id[20]{};
  std::uint8_t reserved[44]{};
};

static_assert(sizeof(Profile) == 384, "G8 runtime build profile ABI");
static_assert(alignof(Profile) == 64, "G8 runtime profile alignment");

inline constexpr bool BytesEqual(const char* left, const char* right,
                                 std::size_t size) noexcept {
  for (std::size_t index = 0; index < size; ++index)
    if (left[index] != right[index]) return false;
  return true;
}

inline constexpr bool Nonzero(const std::uint8_t* bytes,
                              std::size_t size) noexcept {
  std::uint8_t combined = 0;
  for (std::size_t index = 0; index < size; ++index) combined |= bytes[index];
  return combined != 0;
}

inline constexpr bool RvaValid(std::uint64_t rva,
                               std::uint64_t image_size) noexcept {
  return rva >= 0x1000 && rva < image_size && (rva & 3u) == 0;
}

inline constexpr bool Valid(const Profile& profile) noexcept {
  if (!BytesEqual(profile.magic, kMagic, sizeof(kMagic)) ||
      profile.version != kVersion || profile.size != sizeof(Profile) ||
      profile.flags != kRequiredFlags || profile.reserved0 != 0 ||
      profile.image_size < 0x100000 ||
      !Nonzero(profile.native_sha256, sizeof(profile.native_sha256)) ||
      !Nonzero(profile.profile_sha256, sizeof(profile.profile_sha256)) ||
      !Nonzero(profile.build_id, sizeof(profile.build_id)))
    return false;
  for (const std::uint64_t rva : profile.hook_rvas)
    if (!RvaValid(rva, profile.image_size)) return false;
  const std::uint64_t identities[] = {
      profile.main_time_source_vtable_rva,
      profile.embedded_time_source_vtable_rva,
      profile.physics_context_vtable_rva,
      profile.physics_implementation_vtable_rva,
      profile.step_options_vtable_rva,
      profile.native_physics_body_vtable_rva,
      profile.nitro_service_vtable_rva,
      profile.nitro_dispatch_rva,
      profile.vehicle_source_vtable_rva,
      profile.car_physics_body_source_vtable_rva,
      profile.adjusted_setter_vtable_rva,
      profile.frame_event_scheduler_return_rva,
      profile.lifecycle_phase_gate_rva,
      profile.lifecycle_shared_enter_rva,
      profile.lifecycle_derived_enter_rva,
      profile.lifecycle_racing_store_rva,
      profile.barrel_random_bool_rva,
      profile.barrel_random_lerp_rva,
  };
  for (const std::uint64_t rva : identities)
    if (!RvaValid(rva, profile.image_size)) return false;
  for (const std::uint64_t rva : profile.setter_original_rvas)
    if (!RvaValid(rva, profile.image_size)) return false;
  for (const std::uint8_t value : profile.reserved)
    if (value != 0) return false;
  return profile.hook_rvas[g4::kFrameEventHook] + 4 < profile.image_size &&
         profile.adjusted_setter_vtable_rva + g4::kSetterSlotOffsets[1] + 8 <
             profile.image_size &&
         profile.nitro_service_vtable_rva + g4::kNitroDispatchSlot + 8 <
             profile.image_size;
}

inline constexpr Profile ReferenceProfile() noexcept {
  Profile profile{};
  for (std::size_t index = 0; index < sizeof(kMagic); ++index)
    profile.magic[index] = kMagic[index];
  profile.version = kVersion;
  profile.size = sizeof(Profile);
  profile.flags = kRequiredFlags;
  profile.image_size = 0xA5D6E3C;
  profile.hook_rvas[0] = g3::kPhysicsIntervalRva;
  profile.hook_rvas[1] = g3::kFinalWriterRva;
  profile.hook_rvas[2] = g3::kFrameEventRva;
  profile.hook_rvas[3] = g4::kNitroStateEntryRva;
  profile.hook_rvas[4] = g3::kPhysicsSubmitRva;
  profile.hook_rvas[5] = g4::kBarrelRollEntryRva;
  profile.hook_rvas[6] = g4::kBarrelYawEntryRva;
  profile.hook_rvas[7] = 0x37949C8;
  profile.main_time_source_vtable_rva = g4::kMainVtableRva;
  profile.embedded_time_source_vtable_rva = 0x7F3C400;
  profile.physics_context_vtable_rva = g3::kPhysicsContextVtableRva;
  profile.physics_implementation_vtable_rva =
      g3::kPhysicsImplementationVtableRva;
  profile.step_options_vtable_rva = interval::kStepOptionsVtableRva;
  profile.native_physics_body_vtable_rva =
      g4::kNativePhysicsBodyVtableRva;
  profile.nitro_service_vtable_rva = g4::kNitroServiceVtableRva;
  profile.nitro_dispatch_rva = g4::kNitroDispatchThunkRva;
  profile.vehicle_source_vtable_rva = g4::kVehicleSourceVtableRva;
  profile.car_physics_body_source_vtable_rva =
      g4::kCarPhysicsBodySourceVtableRva;
  profile.adjusted_setter_vtable_rva = g4::kAdjustedSetterVtableRva;
  profile.setter_original_rvas[0] = g4::kSetterOriginalRvas[0];
  profile.setter_original_rvas[1] = g4::kSetterOriginalRvas[1];
  profile.frame_event_scheduler_return_rva =
      g3::kFrameEventSchedulerReturnRva;
  profile.lifecycle_phase_gate_rva = 0x3A5955C;
  profile.lifecycle_shared_enter_rva = 0x3A59578;
  profile.lifecycle_derived_enter_rva = 0x382EB04;
  profile.lifecycle_racing_store_rva = 0x3A596C4;
  profile.barrel_random_bool_rva = 0x38B7A00;
  profile.barrel_random_lerp_rva = 0x38B7A3C;
  constexpr std::uint8_t native_sha[32] = {
      0x67,0x15,0x22,0xd4,0x61,0x4a,0xbc,0xce,
      0x5c,0x4d,0xa1,0x6f,0xf8,0xa1,0x77,0x42,
      0x3f,0xa6,0x7f,0x3e,0xac,0xe7,0xb6,0xf0,
      0x65,0x2e,0x97,0x54,0x40,0x30,0x08,0xf0,
  };
  constexpr std::uint8_t profile_sha[32] = {
      0x1b,0x35,0x30,0xd6,0x57,0xe2,0xcb,0x2a,
      0x88,0x60,0x04,0xa0,0x4f,0x1b,0xb6,0xc9,
      0xa8,0x32,0xe3,0x0b,0x9d,0x2c,0xd6,0x55,
      0x50,0x21,0xd2,0x31,0xdc,0x84,0xdd,0x2d,
  };
  constexpr std::uint8_t build_id[20] = {
      0xe5,0xdd,0x7e,0xf2,0x4f,0x52,0xdf,0xf0,0xe0,0x04,
      0x0d,0xc3,0xb1,0x32,0x0f,0x26,0x7a,0x3c,0x3b,0x3b,
  };
  for (std::size_t index = 0; index < 32; ++index) {
    profile.native_sha256[index] = native_sha[index];
    profile.profile_sha256[index] = profile_sha[index];
  }
  for (std::size_t index = 0; index < 20; ++index)
    profile.build_id[index] = build_id[index];
  return profile;
}

static_assert(Valid(ReferenceProfile()), "reference G8 build profile");

}  // namespace a9tas::g8_runtime_build_profile_v1
