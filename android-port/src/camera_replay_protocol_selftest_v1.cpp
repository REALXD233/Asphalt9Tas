#include "camera_replay_protocol_v1.h"

#include <cstring>

namespace camera = a9tas::camera_replay_v1;

int main() {
  camera::HeaderV1 header{};
  std::memcpy(header.magic, camera::kMagic, sizeof(camera::kMagic));
  header.version = camera::kVersion;
  header.header_size = sizeof(header);
  header.frame_size = sizeof(camera::FrameV1);
  header.frame_count = 900;
  header.fixed_interval_us = 16667;

  camera::FrameV1 frame{};
  frame.tick = 0;
  frame.rotation_xyzw[3] = 1.0f;
  frame.fov_radians = 1.0f;
  frame.aspect_ratio = 16.0f / 9.0f;
  if (!camera::HeaderValid(header) || !camera::FrameValid(frame, 0)) return 1;

  frame.tick = 1;
  if (camera::FrameValid(frame, 0)) return 2;
  frame.tick = 0;
  frame.rotation_xyzw[3] = 0.0f;
  if (camera::FrameValid(frame, 0)) return 3;
  frame.rotation_xyzw[3] = 1.0f;
  frame.override_flags = 0x80000000u;
  if (camera::FrameValid(frame, 0)) return 4;
  frame.override_flags = 0;
  frame.look_backwards = 2;
  if (camera::FrameValid(frame, 0)) return 5;
  return 0;
}
