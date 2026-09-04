#pragma once

// Lossless per-frame camera sidecar matching AluTasV2 CameraUpdate semantics.
// The Android port records the real game camera after its original update and
// may replay the recorded position/rotation/FOV at that same boundary.  It
// must never synthesize camera motion from the vehicle transform.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace a9tas::camera_replay_v1 {

inline constexpr char kMagic[8] = {'A', '9', 'C', 'A', 'M', '1', 0, 0};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::uint32_t kMaximumFrames = 36000;

enum OverrideFlag : std::uint32_t {
  kOverridePosition = 1u << 0,
  kOverrideRotation = 1u << 1,
  kOverrideFov = 1u << 2,
  kRelativeToCar = 1u << 3,
};
inline constexpr std::uint32_t kKnownOverrideFlags =
    kOverridePosition | kOverrideRotation | kOverrideFov | kRelativeToCar;

#pragma pack(push, 1)
struct HeaderV1 {
  char magic[8];
  std::uint32_t version;
  std::uint32_t header_size;
  std::uint32_t frame_size;
  std::uint32_t frame_count;
  std::uint32_t fixed_interval_us;
  std::uint32_t reserved;
  std::uint8_t source_recording_sha256[32];
};

struct FrameV1 {
  std::uint32_t tick;
  std::uint32_t override_flags;
  float position[3];
  float rotation_xyzw[4];
  float fov_radians;
  float aspect_ratio;
  float offset_relative_to_car[3];
  std::uint8_t look_backwards;
  std::uint8_t reserved[7];
};
#pragma pack(pop)

static_assert(sizeof(HeaderV1) == 64, "A9CAM1 header ABI");
static_assert(sizeof(FrameV1) == 64, "A9CAM1 frame ABI");

inline bool Finite(const float* values, std::size_t count) noexcept {
  if (values == nullptr) return false;
  for (std::size_t i = 0; i < count; ++i)
    if (!std::isfinite(values[i])) return false;
  return true;
}

inline bool HeaderValid(const HeaderV1& header) noexcept {
  return std::memcmp(header.magic, kMagic, sizeof(kMagic)) == 0 &&
         header.version == kVersion && header.header_size == sizeof(HeaderV1) &&
         header.frame_size == sizeof(FrameV1) && header.frame_count != 0 &&
         header.frame_count <= kMaximumFrames &&
         header.fixed_interval_us >= 1000 &&
         header.fixed_interval_us <= 100000 && header.reserved == 0;
}

inline bool FrameValid(const FrameV1& frame, std::uint32_t index) noexcept {
  if (frame.tick != index ||
      (frame.override_flags & ~kKnownOverrideFlags) != 0 ||
      frame.look_backwards > 1 || !Finite(frame.position, 3) ||
      !Finite(frame.rotation_xyzw, 4) ||
      !Finite(&frame.fov_radians, 1) || !Finite(&frame.aspect_ratio, 1) ||
      !Finite(frame.offset_relative_to_car, 3) ||
      frame.fov_radians <= 0.05f || frame.fov_radians >= 3.13f ||
      frame.aspect_ratio <= 0.1f || frame.aspect_ratio >= 10.0f)
    return false;
  for (std::uint8_t value : frame.reserved)
    if (value != 0) return false;
  const float q2 = frame.rotation_xyzw[0] * frame.rotation_xyzw[0] +
                   frame.rotation_xyzw[1] * frame.rotation_xyzw[1] +
                   frame.rotation_xyzw[2] * frame.rotation_xyzw[2] +
                   frame.rotation_xyzw[3] * frame.rotation_xyzw[3];
  return q2 >= 0.5f && q2 <= 1.5f;
}

}  // namespace a9tas::camera_replay_v1
