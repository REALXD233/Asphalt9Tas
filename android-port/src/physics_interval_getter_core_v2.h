#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::physics_interval_v2 {

inline constexpr char kMagic[8] = {
    'A', '9', 'P', 'H', 'Y', '2', '\0', '\0',
};
inline constexpr std::uint32_t kVersion = 2;
inline constexpr std::uint8_t kSupportedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

inline constexpr std::uintptr_t kPhysicsContextVtableRva = 0x8103830;
inline constexpr std::uintptr_t kStepOptionsVtableRva = 0x7EED420;
inline constexpr std::uintptr_t kGetterAdjustorThunkRva = 0x3695740;
inline constexpr std::uintptr_t kGetterImplementationRva = 0x3695474;
inline constexpr std::intptr_t kOwnerAdjustment = -0x2A78;
inline constexpr std::uintptr_t kStepOptionsOffset = 0x170;
inline constexpr std::uintptr_t kGetterSlotOffset = 0x10;

enum MetadataFlag : std::uint32_t {
    kRawFloatBits = 1u << 0,
    kCallOriginalFirst = 1u << 1,
    kOverrideOutputAfterOriginal = 1u << 2,
    kCaptureFinalOutput = 1u << 3,
};
inline constexpr std::uint32_t kRequiredFlags =
    kRawFloatBits | kCallOriginalFirst | kOverrideOutputAfterOriginal |
    kCaptureFinalOutput;

#pragma pack(push, 1)
struct RecordingMetadataV2 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t flags;
    std::uint8_t build_id[20];
    std::uint64_t context_vtable_rva;
    std::uint64_t step_options_vtable_rva;
    std::uint64_t getter_thunk_rva;
    std::int64_t owner_adjustment;
    std::uint32_t physics_interval_bits;
    std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(RecordingMetadataV2) == 80, "A9PHY2 metadata ABI");

struct ObservationV2 {
    std::uintptr_t library_base;
    std::uintptr_t physics_context;
    std::uintptr_t context_vptr;
    std::uintptr_t step_options;
    std::uintptr_t step_options_vptr;
    std::uintptr_t getter_target;
    std::intptr_t owner_adjustment;
    std::uint32_t effective_interval_bits;
};

enum class PlanStatus : std::uint32_t {
    kReadyGetterDetour = 0,
    kInvalidMetadata = 1,
    kInvalidContextIdentity = 2,
    kMissingStepOptions = 3,
    kInvalidStepOptionsIdentity = 4,
    kInvalidGetterIdentity = 5,
    kInvalidOwnerAdjustment = 6,
    kInvalidEffectiveInterval = 7,
    kAddressOverflow = 8,
};

struct GetterDetourPlanV2 {
    PlanStatus status;
    std::uintptr_t hook_target;
    std::uintptr_t step_options;
    std::uintptr_t owner;
    std::uint32_t observed_interval_bits;
    bool call_original_first;
    bool override_after_original;
    bool capture_final_output;
    bool inline_field_write_allowed;
};

struct DetourResultV2 {
    bool valid;
    std::uint32_t final_output_bits;
    std::uint32_t metadata_after_call_bits;
};

bool ValidateMetadata(const RecordingMetadataV2& metadata) noexcept;
GetterDetourPlanV2 BuildGetterDetourPlan(
    const RecordingMetadataV2& metadata,
    const ObservationV2& observation) noexcept;
DetourResultV2 ApplyOriginalSemantics(
    const RecordingMetadataV2& metadata,
    std::uint32_t original_output_bits,
    bool override_enabled) noexcept;

}  // namespace a9tas::physics_interval_v2
