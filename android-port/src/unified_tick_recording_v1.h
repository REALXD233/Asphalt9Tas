#pragma once

#include <cstddef>
#include <cstdint>

namespace a9tas::unified_tick_v1 {

inline constexpr char kMagic[8] = {
    'A', '9', 'U', 'T', 'K', '1', '\0', '\0',
};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaximumFrames = 36000;
inline constexpr std::uint32_t kMinimumFixedIntervalUs = 1000;
inline constexpr std::uint32_t kMaximumFixedIntervalUs = 100000;
inline constexpr std::uint8_t kSupportedBuildId[20] = {
    0xe5, 0xdd, 0x7e, 0xf2, 0x4f, 0x52, 0xdf, 0xf0, 0xe0, 0x04,
    0x0d, 0xc3, 0xb1, 0x32, 0x0f, 0x26, 0x7a, 0x3c, 0x3b, 0x3b,
};

inline constexpr std::uint32_t kTransformOffset = 0x10;
inline constexpr std::uint32_t kLinearVelocityOffset = 0x150;
inline constexpr std::uint32_t kAngularVelocityOffset = 0x160;
inline constexpr std::uint32_t kTransformSize = 64;
inline constexpr std::uint32_t kLinearVelocitySize = 12;
inline constexpr std::uint32_t kAngularVelocitySize = 12;

enum SkipOverride : std::uint32_t {
    kSkipSteer = 1u << 0,
    kSkipBrake = 1u << 1,
    kSkipNitroActivation = 1u << 2,
    kSkipAccelerator = 1u << 3,
    kSkipBarrelAngular = 1u << 4,
    kSkipBarrelRbx = 1u << 5,
    kSkipRespawnButton = 1u << 6,
    kSkipTransformForced = 1u << 7,
};
inline constexpr std::uint32_t kSupportedSkipMask = 0xff;

enum HeaderFlag : std::uint32_t {
    kRawFloatBits = 1u << 0,
    kComponentFloatComparison = 1u << 1,
    kAllOrNothingTransformLinear = 1u << 2,
    kFixedInterval = 1u << 3,
    kCertifiedFinalBoundary = 1u << 4,
};
inline constexpr std::uint32_t kRequiredHeaderFlags = 0x1f;

enum FrameFlag : std::uint32_t {
    kControlsValid = 1u << 0,
    kActionsValid = 1u << 1,
    kPhysicsValid = 1u << 2,
};
inline constexpr std::uint32_t kRequiredFrameFlags = 0x7;

#pragma pack(push, 1)
struct RecordingHeaderV1 {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint32_t frame_size;
    std::uint32_t frame_count;
    std::uint32_t fixed_interval_us;
    std::uint32_t flags;
    std::uint32_t supported_skip_mask;
    std::uint8_t build_id[20];
    std::uint32_t transform_offset;
    std::uint32_t linear_velocity_offset;
    std::uint32_t angular_velocity_offset;
    std::uint32_t transform_size;
    std::uint32_t linear_velocity_size;
    std::uint32_t angular_velocity_size;
    std::uint8_t reserved[16];
};

struct RecordingFrameV1 {
    std::uint64_t tick;
    std::uint64_t monotonic_ns;
    float steering;
    float brake;
    float accelerator;
    std::uint32_t nitro_activation_count;
    std::uint32_t skip_override_flags;
    std::uint8_t respawn_button_press;
    std::uint8_t padding[3];
    float barrel_angular_velocity[3];
    float barrel_rbx[2];
    std::uint8_t transform_bits[kTransformSize];
    std::uint8_t linear_velocity_bits[kLinearVelocitySize];
    std::uint32_t flags;
    std::uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(RecordingHeaderV1) == 96, "A9UTK1 header ABI");
static_assert(sizeof(RecordingFrameV1) == 144, "A9UTK1 frame ABI");
static_assert(offsetof(RecordingFrameV1, transform_bits) == 60,
              "A9UTK1 transform offset");
static_assert(offsetof(RecordingFrameV1, linear_velocity_bits) == 124,
              "A9UTK1 linear offset");

}  // namespace a9tas::unified_tick_v1
