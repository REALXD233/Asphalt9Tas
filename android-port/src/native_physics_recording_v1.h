#pragma once

// Exact native-body final-correction recording ABI for the supported A9 CN
// build.  This is deliberately separate from the older A9PST1 diagnostic
// trace: the replay payload is the raw 64-byte native transform plus the raw
// 12-byte authoritative linear velocity, matching AluTasV2's final correction.

#include <cstddef>
#include <cstdint>

namespace a9tas::native_physics_v1 {

inline constexpr char kMagic[8] = {'A', '9', 'N', 'P', 'S', '1', '\0', '\0'};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kTransformOffset = 0x10;
inline constexpr std::uint32_t kLinearVelocityOffset = 0x150;
inline constexpr std::uint32_t kTransformSize = 64;
inline constexpr std::uint32_t kLinearVelocitySize = 12;

inline constexpr std::uint32_t kHeaderRawFloatBits = 1u << 0;
inline constexpr std::uint32_t kHeaderComponentFloatComparison = 1u << 1;
inline constexpr std::uint32_t kHeaderAllOrNothingCopy = 1u << 2;
inline constexpr std::uint32_t kRequiredHeaderFlags =
    kHeaderRawFloatBits | kHeaderComponentFloatComparison |
    kHeaderAllOrNothingCopy;

inline constexpr std::uint32_t kFrameCapturedAtCertifiedBoundary = 1u << 0;
inline constexpr std::uint32_t kRequiredFrameFlags =
    kFrameCapturedAtCertifiedBoundary;

inline constexpr std::uint8_t kSupportedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

#pragma pack(push, 1)
struct RecordingHeaderV1 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t frame_size;
    std::uint32_t frame_count;
    std::uint8_t build_id[20];
    std::uint32_t transform_offset;
    std::uint32_t linear_velocity_offset;
    std::uint32_t transform_size;
    std::uint32_t linear_velocity_size;
    std::uint32_t flags;
};

struct RecordingFrameV1 {
    std::uint64_t tick;
    std::uint64_t monotonic_ns;
    std::uint32_t transform_bits[16];
    std::uint32_t linear_velocity_bits[3];
    std::uint32_t flags;
};
#pragma pack(pop)

static_assert(sizeof(RecordingHeaderV1) == 64, "A9NPS1 header ABI");
static_assert(sizeof(RecordingFrameV1) == 96, "A9NPS1 frame ABI");
static_assert(offsetof(RecordingFrameV1, transform_bits) == 16,
              "A9NPS1 transform offset");
static_assert(offsetof(RecordingFrameV1, linear_velocity_bits) == 80,
              "A9NPS1 linear offset");

}  // namespace a9tas::native_physics_v1
