#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::physics_interval_v1 {

inline constexpr char kMagic[8] = {
    'A', '9', 'P', 'H', 'Y', '1', '\0', '\0',
};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kRequiredFlags = 0x7;
inline constexpr std::uint8_t kSupportedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

// Static identities for the supported Android build.
inline constexpr std::uintptr_t kPhysicsContextVtableRva = 0x8103830;
inline constexpr std::uintptr_t kPhysicsContextExecuteTokenRva = 0x38B74DC;
inline constexpr std::uintptr_t kPhysicsContextGetIntervalRva = 0x38B77C0;
inline constexpr std::uintptr_t kDefaultStepOptionsVtableRva = 0x81039A0;
inline constexpr std::uintptr_t kDefaultStepOptionsGetIntervalRva = 0x38B7C5C;
inline constexpr std::uintptr_t kBackendFixedSubstepRva = 0x4CC75F0;
inline constexpr std::uintptr_t kStepOptionsOffset = 0x170;
inline constexpr std::uintptr_t kInlineIntervalOffset = 0x178;

enum MetadataFlag : std::uint32_t {
    kRawFloatBits = 1u << 0,
    kRequireInlineDefaultOptions = 1u << 1,
    kRestoreOriginalBits = 1u << 2,
};

#pragma pack(push, 1)
struct RecordingMetadataV1 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t size;
    std::uint32_t flags;
    std::uint8_t build_id[20];
    std::uint64_t context_vtable_rva;
    std::uint64_t getter_rva;
    std::uint32_t physics_interval_bits;
    std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(RecordingMetadataV1) == 64,
              "A9PHY1 metadata ABI");

struct ObservationV1 {
    std::uintptr_t library_base;
    std::uintptr_t physics_context;
    std::uintptr_t context_vptr;
    std::uintptr_t step_options;
    std::uint32_t current_interval_bits;
};

enum class PlanStatus : std::uint32_t {
    kReady = 0,
    kAlreadyExact = 1,
    kInvalidMetadata = 2,
    kInvalidIdentity = 3,
    kAlternateStepOptionsActive = 4,
    kInvalidCurrentInterval = 5,
    kAddressOverflow = 6,
};

struct WritePlanV1 {
    PlanStatus status;
    std::uintptr_t address;
    std::uint32_t before_bits;
    std::uint32_t desired_bits;
    bool write_required;
    bool restore_required;
};

bool ValidateMetadata(const RecordingMetadataV1& metadata) noexcept;
WritePlanV1 BuildWritePlan(const RecordingMetadataV1& metadata,
                           const ObservationV1& observation) noexcept;
bool VerifyApplied(const WritePlanV1& plan,
                   std::uint32_t observed_bits) noexcept;
bool VerifyRestored(const WritePlanV1& plan,
                    std::uint32_t observed_bits) noexcept;

}  // namespace a9tas::physics_interval_v1
