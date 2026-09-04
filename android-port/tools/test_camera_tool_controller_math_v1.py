#!/usr/bin/env python3
"""Offline tests for upstream Camera Tool controller parity."""

from __future__ import annotations

import math
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

from camera_tool_controller_math_v1 import (  # noqa: E402
    Camera,
    FreeFlightController,
    InputFrame,
    OrbitalController,
    Quat,
    Vec3,
    camera_to_stream_values,
    game_position_to_opengl,
    game_rotation_to_opengl,
    opengl_position_to_game,
    opengl_rotation_to_game,
)


def close(actual: float, expected: float, tolerance: float = 2.0e-5) -> None:
    assert abs(actual - expected) <= tolerance, (actual, expected)


def vec_close(actual: Vec3, expected: Vec3) -> None:
    close(actual.x, expected.x)
    close(actual.y, expected.y)
    close(actual.z, expected.z)


def source_parity() -> None:
    camera_layer = (ROOT / "source" / "AluTasV2-main" / "AsphaltTool" / "tool" / "src" / "layer" / "CameraToolLayer.cpp").read_text(encoding="utf-8")
    free_source = (ROOT / "source" / "AluTasV2-main" / "engine" / "src" / "core" / "scene" / "FreeCam_CameraController.cpp").read_text(encoding="utf-8")
    orbital_source = (ROOT / "source" / "AluTasV2-main" / "engine" / "src" / "core" / "scene" / "OrbitalCam_CameraController.cpp").read_text(encoding="utf-8")
    state_source = (ROOT / "source" / "AluTasV2-main" / "AsphaltTool" / "tool" / "src" / "common" / "CameraState.cpp").read_text(encoding="utf-8")
    for needle in (
        "SetMoveSpeed(100.0f)", "SetSensitivity(0.2f)", "zoom_rate  = 5.0f",
        "speed = std::clamp(speed, 0.1f, 1000.0f)",
        "m_mouse_is_pressed[GLFW_MOUSE_BUTTON_RIGHT] = true",
    ):
        assert needle in camera_layer, needle
    for needle in ("movement.z -= 1.0f", "camera.GetAbsoluteUp()", "m_camera_pitch, -89.0f, 89.0f", "q_pitch * q_yaw"):
        assert needle in free_source, needle
    for needle in ("m_distance * 0.005f", "std::max(m_distance, 0.1f)", "m_target - forward * m_distance"):
        assert needle in orbital_source, needle
    assert "std::swap(xzy.y, xzy.z)" in state_source
    assert "std::swap(out.y, out.z)" in state_source


def coordinate_round_trips() -> None:
    game_position = Vec3(1.25, -2.5, 9.0)
    assert opengl_position_to_game(game_position_to_opengl(game_position)) == game_position
    game_rotation = (0.1, -0.2, 0.3, 0.92736185)
    opengl = game_rotation_to_opengl(game_rotation)
    for actual, expected in zip(opengl_rotation_to_game(opengl), game_rotation):
        close(actual, expected)


def free_flight() -> None:
    camera = Camera(Vec3(1.0, 2.0, 3.0), Quat.identity(), math.radians(55.0))
    controller = FreeFlightController()
    controller.update(camera, InputFrame(w=True), 0.5)
    vec_close(camera.position, Vec3(1.0, 2.0, -47.0))
    close(controller.move_speed, 100.0)

    diagonal = Camera(Vec3(0.0, 0.0, 0.0), Quat.identity(), math.radians(55.0))
    controller = FreeFlightController(move_speed=10.0)
    controller.update(diagonal, InputFrame(w=True, d=True), 1.0)
    close(diagonal.position.x, math.sqrt(50.0))
    close(diagonal.position.z, -math.sqrt(50.0))

    speed_camera = Camera(Vec3(0.0, 0.0, 0.0), Quat.identity(), math.radians(55.0))
    controller = FreeFlightController(move_speed=100.0)
    controller.update(speed_camera, InputFrame(e=True), 0.25)
    close(controller.move_speed, 150.0)
    controller.update(speed_camera, InputFrame(q=True), 0.25)
    close(controller.move_speed, 100.0)

    controller = FreeFlightController(move_speed=10.0)
    controller.update(speed_camera, InputFrame(e=True, q=True), 0.1)
    close(controller.move_speed, 7.5)

    controller.update(speed_camera, InputFrame(mouse_dx=50.0, mouse_dy=-25.0), 0.0)
    close(controller.yaw_degrees, -10.0)
    close(controller.pitch_degrees, 5.0)
    assert abs(speed_camera.rotation.w) < 1.0
    assert abs(speed_camera.rotation.w) > 0.9


def orbital() -> None:
    camera = Camera(Vec3(99.0, 99.0, 99.0), Quat.identity(), math.radians(55.0))
    controller = OrbitalController(distance=5.0)
    target = Vec3(10.0, 2.0, -3.0)
    controller.update(camera, target, InputFrame())
    vec_close(camera.position, Vec3(10.0, 2.0, 2.0))
    controller.update(camera, target, InputFrame(e=True))
    close(controller.distance, 3.975)
    vec_close(camera.position, Vec3(10.0, 2.0, 0.975))


def stream_layout() -> None:
    camera = Camera(Vec3(1.0, 2.0, 3.0), Quat.identity(), 0.75)
    values = camera_to_stream_values(camera)
    expected = (1.0, -3.0, 2.0, 0.0, -0.0, 0.0, 1.0, 0.75)
    for actual, wanted in zip(values, expected):
        close(actual, wanted)


def main() -> int:
    source_parity()
    coordinate_round_trips()
    free_flight()
    orbital()
    stream_layout()
    print(
        "CAMERA_TOOL_CONTROLLER_MATH passed=1 source_parity=1 f32=1 "
        "free_flight=1 orbital=1 coordinate_conversion=1 stream_layout=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
