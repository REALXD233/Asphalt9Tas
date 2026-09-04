#include "camera_tool_runtime_core_v2.h"

#include <cmath>

namespace core = a9tas::camera_tool_runtime_core_v2;
namespace protocol = a9tas::camera_tool_runtime_v2;

namespace {

bool Near(float left, float right, float epsilon = 0.001f) {
  return std::fabs(left - right) <= epsilon;
}

float StepForward(core::State* state, float requested_speed,
                  bool synchronize_speed) {
  core::Input input{};
  input.mode = protocol::Mode::kFreeFlight;
  input.bits = protocol::kForward;
  input.move_speed = requested_speed;
  input.synchronize_move_speed = synchronize_speed;
  input.fov_radians = 0.959931076f;
  input.dt_seconds = 0.01f;
  core::Camera natural{};
  natural.rotation = {};
  natural.fov_radians = input.fov_radians;
  core::Camera output{};
  const float before = state->camera.position.z;
  if (!core::Step(state, input, natural, &output)) return -1.0f;
  return std::fabs(output.position.z - before);
}

}  // namespace

int main() {
  core::State state{};
  if (!Near(StepForward(&state, 12.0f, true), 0.12f)) return 1;
  if (!Near(StepForward(&state, 120.0f, true), 1.20f)) return 2;
  if (!Near(StepForward(&state, 600.0f, true), 6.00f)) return 3;
  // A command that only changes yaw/input bits must not erase speed adjusted
  // by the runtime controller.  Only an explicit speed edge is authoritative.
  if (!Near(StepForward(&state, 12.0f, false), 6.00f)) return 4;
  if (!Near(state.move_speed, 600.0f)) return 5;
  return 0;
}
