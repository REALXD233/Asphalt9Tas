#pragma once

#include "camera_tool_runtime_protocol_v2.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace a9tas::camera_tool_runtime_core_v2 {

namespace protocol = a9tas::camera_tool_runtime_v2;

struct Vec3 {
  float x{}, y{}, z{};
};

struct Quat {
  float w{1.0f}, x{}, y{}, z{};
};

struct Camera {
  Vec3 position{};
  Quat rotation{};
  float fov_radians{};
};

struct Input {
  protocol::Mode mode{protocol::Mode::kAbsolute};
  std::uint32_t bits{};
  float yaw_degrees{};
  float pitch_degrees{};
  float move_speed{100.0f};
  float sensitivity{0.2f};
  float orbital_distance{5.0f};
  float orbital_zoom_speed{1.0f};
  float fov_radians{0.959931076f};
  float dt_seconds{};
  Vec3 vehicle_target_game{};
  // The UI speed is authoritative only when its value actually changes.
  // Keeping this edge explicit preserves the upstream E/Q-style accumulated
  // speed adjustment while still making a live slider update take effect.
  bool synchronize_move_speed{};
};

struct State {
  bool initialized{};
  protocol::Mode mode{protocol::Mode::kAbsolute};
  Camera camera{};
  float move_speed{100.0f};
  float orbital_distance{5.0f};
};

inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
inline Vec3 Scale(Vec3 a, float s) { return {a.x*s, a.y*s, a.z*s}; }
inline float Length(Vec3 a) { return std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z); }
inline Vec3 Normalize(Vec3 a) {
  const float length = Length(a);
  return length > 0.0f ? Scale(a, 1.0f / length) : a;
}

inline Quat Multiply(Quat a, Quat b) {
  return {
      a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z,
      a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
      a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
      a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
  };
}

inline Quat Normalize(Quat q) {
  const float length = std::sqrt(q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z);
  if (length <= 0.0f) return q;
  return {q.w/length, q.x/length, q.y/length, q.z/length};
}

inline Vec3 Rotate(Quat q, Vec3 v) {
  q = Normalize(q);
  const Vec3 u{q.x, q.y, q.z};
  const float dot_uv = u.x*v.x + u.y*v.y + u.z*v.z;
  const float dot_uu = u.x*u.x + u.y*u.y + u.z*u.z;
  const Vec3 cross{u.y*v.z-u.z*v.y, u.z*v.x-u.x*v.z,
                   u.x*v.y-u.y*v.x};
  return Add(Add(Scale(u, 2.0f*dot_uv), Scale(v, q.w*q.w-dot_uu)),
             Scale(cross, 2.0f*q.w));
}

inline Quat AngleAxisDegrees(float degrees, Vec3 axis) {
  constexpr float kPi = 3.14159265358979323846f;
  axis = Normalize(axis);
  const float half = degrees * (kPi / 180.0f) * 0.5f;
  const float sine = std::sin(half);
  return {std::cos(half), axis.x*sine, axis.y*sine, axis.z*sine};
}

inline Vec3 GameToOpenGl(Vec3 value) {
  return {value.x, value.z, -value.y};
}

inline Vec3 OpenGlToGame(Vec3 value) {
  return {value.x, -value.z, value.y};
}

inline Quat GameToOpenGl(float x, float y, float z, float w) {
  return {w, x, z, -y};
}

inline void OpenGlToGame(Quat value, float output_xyzw[4]) {
  output_xyzw[0] = value.x;
  output_xyzw[1] = -value.z;
  output_xyzw[2] = value.y;
  output_xyzw[3] = value.w;
}

inline Quat ControllerRotation(float yaw, float pitch) {
  const Quat q_yaw = AngleAxisDegrees(yaw, {0.0f,1.0f,0.0f});
  const Vec3 right = Rotate(q_yaw, {1.0f,0.0f,0.0f});
  const Quat q_pitch = AngleAxisDegrees(
      std::clamp(pitch, -89.0f, 89.0f), right);
  return Normalize(Multiply(q_pitch, q_yaw));
}

inline void Initialize(State* state, const Input& input,
                       const Camera& natural) {
  state->initialized = true;
  state->mode = input.mode;
  state->camera = natural;
  // Upstream pseudo cameras are constructed at 55 degrees and only copy the
  // current position/rotation on Camera Tool entry.
  state->camera.fov_radians = input.fov_radians;
  state->move_speed = std::clamp(input.move_speed, 0.1f, 1000.0f);
  state->orbital_distance = std::max(input.orbital_distance, 0.1f);
}

inline bool Step(State* state, const Input& input, const Camera& natural,
                 Camera* output) {
  if (state == nullptr || output == nullptr ||
      input.bits & ~protocol::kKnownInputMask ||
      !std::isfinite(input.yaw_degrees) ||
      !std::isfinite(input.pitch_degrees) ||
      !std::isfinite(input.move_speed) ||
      !std::isfinite(input.orbital_distance) ||
      !std::isfinite(input.fov_radians) ||
      !std::isfinite(input.dt_seconds))
    return false;
  if (!state->initialized || state->mode != input.mode)
    Initialize(state, input, natural);
  else if (input.synchronize_move_speed)
    state->move_speed = std::clamp(input.move_speed, 0.1f, 1000.0f);
  const float dt = std::clamp(input.dt_seconds, 0.0f, 0.05f);

  if (input.mode == protocol::Mode::kFreeFlight) {
    if (input.bits & protocol::kZoomIn)
      state->move_speed +=
          std::clamp(5.0f*state->move_speed, 1.0f, 2000.0f)*dt;
    if (input.bits & protocol::kZoomOut)
      state->move_speed -=
          std::clamp(5.0f*state->move_speed, 1.0f, 2000.0f)*dt;
    state->move_speed = std::clamp(state->move_speed, 0.1f, 1000.0f);

    Vec3 movement{};
    if (input.bits & protocol::kForward) movement.z -= 1.0f;
    if (input.bits & protocol::kBackward) movement.z += 1.0f;
    if (input.bits & protocol::kLeft) movement.x -= 1.0f;
    if (input.bits & protocol::kRight) movement.x += 1.0f;
    if (input.bits & protocol::kUp) movement.y += 1.0f;
    if (input.bits & protocol::kDown) movement.y -= 1.0f;
    if (Length(movement) > 0.0f) {
      const Vec3 right = Rotate(state->camera.rotation, {1.0f,0.0f,0.0f});
      const Vec3 forward = Rotate(state->camera.rotation, {0.0f,0.0f,-1.0f});
      const Vec3 world = Add(Add(Scale(right, movement.x),
                                 {0.0f,movement.y,0.0f}),
                             Scale(forward, -movement.z));
      state->camera.position = Add(
          state->camera.position,
          Scale(Normalize(world), state->move_speed*dt));
    }
    state->camera.rotation =
        ControllerRotation(input.yaw_degrees, input.pitch_degrees);
    state->camera.fov_radians = input.fov_radians;
  } else if (input.mode == protocol::Mode::kOrbital) {
    float wheel = 0.0f;
    if (input.bits & protocol::kZoomIn) wheel += 1.0f;
    if (input.bits & protocol::kZoomOut) wheel -= 1.0f;
    if (wheel != 0.0f) {
      state->orbital_distance -= wheel * input.orbital_zoom_speed *
          (1.0f + state->orbital_distance*0.005f);
      state->orbital_distance = std::max(state->orbital_distance, 0.1f);
    }
    state->camera.rotation =
        ControllerRotation(input.yaw_degrees, input.pitch_degrees);
    const Vec3 target = GameToOpenGl(input.vehicle_target_game);
    const Vec3 forward = Rotate(state->camera.rotation, {0.0f,0.0f,-1.0f});
    state->camera.position =
        Sub(target, Scale(forward, state->orbital_distance));
    state->camera.fov_radians = input.fov_radians;
  } else {
    return false;
  }
  *output = state->camera;
  return true;
}

}  // namespace a9tas::camera_tool_runtime_core_v2
