#!/usr/bin/env python3
"""Upstream-faithful Camera Tool controller math.

This module is deliberately independent from adb and the live payload.  It
models AluTasV2's Free Flight and Orbital pseudo cameras, then converts their
OpenGL coordinates to the Gameloft camera layout consumed by the Android
source-authority backend.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
import struct


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]


def clamp(value: float, low: float, high: float) -> float:
    return f32(max(low, min(high, value)))


@dataclass(frozen=True)
class Vec3:
    x: float
    y: float
    z: float

    def __post_init__(self) -> None:
        object.__setattr__(self, "x", f32(self.x))
        object.__setattr__(self, "y", f32(self.y))
        object.__setattr__(self, "z", f32(self.z))

    def __add__(self, other: "Vec3") -> "Vec3":
        return Vec3(f32(self.x + other.x), f32(self.y + other.y), f32(self.z + other.z))

    def __sub__(self, other: "Vec3") -> "Vec3":
        return Vec3(f32(self.x - other.x), f32(self.y - other.y), f32(self.z - other.z))

    def scale(self, scalar: float) -> "Vec3":
        return Vec3(f32(self.x * scalar), f32(self.y * scalar), f32(self.z * scalar))

    def length(self) -> float:
        return f32(math.sqrt(f32(f32(self.x * self.x) + f32(self.y * self.y) + f32(self.z * self.z))))

    def normalized(self) -> "Vec3":
        length = self.length()
        if length == 0.0:
            return self
        return self.scale(f32(1.0 / length))


@dataclass(frozen=True)
class Quat:
    """GLM presentation order: w, x, y, z."""

    w: float
    x: float
    y: float
    z: float

    def __post_init__(self) -> None:
        object.__setattr__(self, "w", f32(self.w))
        object.__setattr__(self, "x", f32(self.x))
        object.__setattr__(self, "y", f32(self.y))
        object.__setattr__(self, "z", f32(self.z))

    @staticmethod
    def identity() -> "Quat":
        return Quat(1.0, 0.0, 0.0, 0.0)

    def __mul__(self, other: "Quat") -> "Quat":
        return Quat(
            f32(f32(self.w * other.w) - f32(self.x * other.x) - f32(self.y * other.y) - f32(self.z * other.z)),
            f32(f32(self.w * other.x) + f32(self.x * other.w) + f32(self.y * other.z) - f32(self.z * other.y)),
            f32(f32(self.w * other.y) - f32(self.x * other.z) + f32(self.y * other.w) + f32(self.z * other.x)),
            f32(f32(self.w * other.z) + f32(self.x * other.y) - f32(self.y * other.x) + f32(self.z * other.w)),
        )

    def normalized(self) -> "Quat":
        length = f32(math.sqrt(f32(
            f32(self.w * self.w) + f32(self.x * self.x) +
            f32(self.y * self.y) + f32(self.z * self.z)
        )))
        if length == 0.0:
            return self
        inv = f32(1.0 / length)
        return Quat(f32(self.w * inv), f32(self.x * inv), f32(self.y * inv), f32(self.z * inv))

    def rotate(self, vector: Vec3) -> Vec3:
        # Equivalent to GLM's q * vec3 for a normalized quaternion.
        q = self.normalized()
        u = Vec3(q.x, q.y, q.z)
        dot_uv = f32(f32(u.x * vector.x) + f32(u.y * vector.y) + f32(u.z * vector.z))
        dot_uu = f32(f32(u.x * u.x) + f32(u.y * u.y) + f32(u.z * u.z))
        cross = Vec3(
            f32(u.y * vector.z - u.z * vector.y),
            f32(u.z * vector.x - u.x * vector.z),
            f32(u.x * vector.y - u.y * vector.x),
        )
        return u.scale(f32(2.0 * dot_uv)) + vector.scale(f32(q.w * q.w - dot_uu)) + cross.scale(f32(2.0 * q.w))


def angle_axis_degrees(angle_degrees: float, axis: Vec3) -> Quat:
    unit = axis.normalized()
    half = f32(f32(angle_degrees * f32(math.pi / 180.0)) * 0.5)
    sine = f32(math.sin(half))
    return Quat(f32(math.cos(half)), f32(unit.x * sine), f32(unit.y * sine), f32(unit.z * sine))


@dataclass
class Camera:
    position: Vec3
    rotation: Quat
    fov_radians: float

    def __post_init__(self) -> None:
        self.rotation = self.rotation.normalized()
        self.fov_radians = f32(self.fov_radians)

    def forward(self) -> Vec3:
        return self.rotation.rotate(Vec3(0.0, 0.0, -1.0))

    def right(self) -> Vec3:
        return self.rotation.rotate(Vec3(1.0, 0.0, 0.0))


@dataclass(frozen=True)
class InputFrame:
    w: bool = False
    s: bool = False
    a: bool = False
    d: bool = False
    space: bool = False
    left_control: bool = False
    e: bool = False
    q: bool = False
    mouse_dx: float = 0.0
    mouse_dy: float = 0.0


@dataclass
class FreeFlightController:
    move_speed: float = 100.0
    sensitivity: float = 0.2
    yaw_degrees: float = 0.0
    pitch_degrees: float = 0.0

    def update(self, camera: Camera, inputs: InputFrame, dt_seconds: float) -> None:
        dt = f32(dt_seconds)
        speed = f32(self.move_speed)
        zoom_rate = f32(5.0)
        if inputs.e:
            speed_delta = clamp(f32(zoom_rate * speed), 1.0, 200.0)
            speed = f32(speed + f32(speed_delta * dt))
        if inputs.q:
            speed_delta = clamp(f32(zoom_rate * speed), 1.0, 200.0)
            speed = f32(speed - f32(speed_delta * dt))
        self.move_speed = clamp(speed, 0.1, 1000.0)

        movement = Vec3(
            (-1.0 if inputs.a else 0.0) + (1.0 if inputs.d else 0.0),
            (1.0 if inputs.space else 0.0) + (-1.0 if inputs.left_control else 0.0),
            (-1.0 if inputs.w else 0.0) + (1.0 if inputs.s else 0.0),
        )
        if movement.length() > 0.0:
            world = (
                camera.right().scale(movement.x)
                + Vec3(0.0, 1.0, 0.0).scale(movement.y)
                + camera.forward().scale(f32(-movement.z))
            )
            camera.position = camera.position + world.normalized().scale(f32(self.move_speed * dt))

        self.yaw_degrees = f32(self.yaw_degrees - f32(inputs.mouse_dx * self.sensitivity))
        self.pitch_degrees = clamp(
            f32(self.pitch_degrees - f32(inputs.mouse_dy * self.sensitivity)), -89.0, 89.0
        )
        q_yaw = angle_axis_degrees(self.yaw_degrees, Vec3(0.0, 1.0, 0.0))
        right = q_yaw.rotate(Vec3(1.0, 0.0, 0.0))
        q_pitch = angle_axis_degrees(self.pitch_degrees, right)
        camera.rotation = (q_pitch * q_yaw).normalized()

        # CameraToolLayer never forwards wheel input to FreeCam, but the
        # controller still performs the degrees/radians round-trip each update.
        fov_degrees = clamp(f32(camera.fov_radians * f32(180.0 / math.pi)), 10.0, 160.0)
        camera.fov_radians = f32(fov_degrees * f32(math.pi / 180.0))


@dataclass
class OrbitalController:
    distance: float = 5.0
    sensitivity: float = 0.2
    zoom_speed: float = 1.0
    yaw_degrees: float = 0.0
    pitch_degrees: float = 0.0

    def update(self, camera: Camera, target: Vec3, inputs: InputFrame) -> None:
        wheel = f32((1.0 if inputs.e else 0.0) + (-1.0 if inputs.q else 0.0))
        if wheel != 0.0:
            factor = f32(1.0 + f32(self.distance * 0.005))
            self.distance = f32(self.distance - f32(f32(wheel * self.zoom_speed) * factor))
            self.distance = f32(max(self.distance, 0.1))

        self.yaw_degrees = f32(self.yaw_degrees - f32(inputs.mouse_dx * self.sensitivity))
        self.pitch_degrees = clamp(
            f32(self.pitch_degrees - f32(inputs.mouse_dy * self.sensitivity)), -89.0, 89.0
        )
        q_yaw = angle_axis_degrees(self.yaw_degrees, Vec3(0.0, 1.0, 0.0))
        right = q_yaw.rotate(Vec3(1.0, 0.0, 0.0))
        q_pitch = angle_axis_degrees(self.pitch_degrees, right)
        camera.rotation = (q_pitch * q_yaw).normalized()
        camera.position = target - camera.forward().scale(self.distance)


def game_position_to_opengl(value: Vec3) -> Vec3:
    return Vec3(value.x, value.z, f32(-value.y))


def opengl_position_to_game(value: Vec3) -> Vec3:
    return Vec3(value.x, f32(-value.z), value.y)


def game_rotation_to_opengl(value_xyzw: tuple[float, float, float, float]) -> Quat:
    x, y, z, w = value_xyzw
    return Quat(w, x, z, f32(-y))


def opengl_rotation_to_game(value: Quat) -> tuple[float, float, float, float]:
    return (value.x, f32(-value.z), value.y, value.w)


def camera_to_stream_values(camera: Camera) -> tuple[float, ...]:
    position = opengl_position_to_game(camera.position)
    qx, qy, qz, qw = opengl_rotation_to_game(camera.rotation)
    return (position.x, position.y, position.z, qx, qy, qz, qw, f32(camera.fov_radians))
