#!/usr/bin/env python3
"""Strict A9UTK1 unified tick packet codec and original-semantics planner."""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from transform_correction_semantics_v1 import plan_transform_linear_correction


MAGIC = b"A9UTK1\0\0"
VERSION = 1
HEADER_SIZE = 96
FRAME_SIZE = 144
MAX_FRAMES = 36_000
SUPPORTED_BUILD_ID = bytes.fromhex("e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b")

TRANSFORM_OFFSET = 0x10
LINEAR_OFFSET = 0x150
ANGULAR_OFFSET = 0x160
TRANSFORM_SIZE = 64
LINEAR_SIZE = 12
ANGULAR_SIZE = 12

SKIP_STEER = 1 << 0
SKIP_BRAKE = 1 << 1
SKIP_NITRO = 1 << 2
SKIP_ACCELERATOR = 1 << 3
SKIP_BARREL_ANGULAR = 1 << 4
SKIP_BARREL_RBX = 1 << 5
SKIP_RESPAWN = 1 << 6
SKIP_TRANSFORM = 1 << 7
SUPPORTED_SKIP_MASK = 0xFF

HEADER_RAW_FLOAT_BITS = 1 << 0
HEADER_COMPONENT_FLOAT_COMPARISON = 1 << 1
HEADER_ALL_OR_NOTHING_TRANSFORM_LINEAR = 1 << 2
HEADER_FIXED_INTERVAL = 1 << 3
HEADER_CERTIFIED_FINAL_BOUNDARY = 1 << 4
REQUIRED_HEADER_FLAGS = 0x1F

FRAME_CONTROLS_VALID = 1 << 0
FRAME_ACTIONS_VALID = 1 << 1
FRAME_PHYSICS_VALID = 1 << 2
REQUIRED_FRAME_FLAGS = 0x7

_HEADER = struct.Struct("<8s7I20s6I16s")
_FRAME = struct.Struct("<QQfffIIB3x3f2f64s12sII")

assert _HEADER.size == HEADER_SIZE
assert _FRAME.size == FRAME_SIZE


@dataclass(frozen=True)
class UnifiedTickFrameV1:
    tick: int
    monotonic_ns: int
    steering: float
    brake: float
    accelerator: float
    nitro_activations: int
    skip_flags: int
    respawn: bool
    barrel_angular: tuple[float, float, float]
    barrel_rbx: tuple[float, float]
    transform: bytes
    linear_velocity: bytes
    flags: int = REQUIRED_FRAME_FLAGS


@dataclass(frozen=True)
class TickPlanV1:
    steering: float | None
    brake: float | None
    accelerator: float | None
    nitro_activations: int
    respawn: bool
    barrel_angular: bytes | None
    barrel_rbx: tuple[float, float] | None
    transform_linear_writes: tuple


def _finite_bounded(values: Iterable[float], limit: float) -> bool:
    return all(math.isfinite(value) and abs(value) <= limit for value in values)


def _floats(raw: bytes) -> tuple[float, ...]:
    return tuple(value for (value,) in struct.iter_unpack("<f", raw))


def _validate_frame(frame: UnifiedTickFrameV1) -> None:
    if not 0 <= frame.tick <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("tick is outside uint64 range")
    if not 0 <= frame.monotonic_ns <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("monotonic_ns is outside uint64 range")
    if not _finite_bounded((frame.steering, frame.brake, frame.accelerator), 8.0):
        raise ValueError("control value is non-finite/out-of-range")
    if not 0 <= frame.nitro_activations <= 2:
        raise ValueError("nitro activations must be 0, 1, or 2")
    if frame.skip_flags & ~SUPPORTED_SKIP_MASK:
        raise ValueError("unknown skip override flag")
    if type(frame.respawn) is not bool:
        raise ValueError("respawn must be bool")
    if len(frame.barrel_angular) != 3 or not _finite_bounded(frame.barrel_angular, 1_000_000.0):
        raise ValueError("invalid barrel angular payload")
    if len(frame.barrel_rbx) != 2 or not _finite_bounded(frame.barrel_rbx, 1_000_000.0):
        raise ValueError("invalid barrel RBX payload")
    if len(frame.transform) != TRANSFORM_SIZE or len(frame.linear_velocity) != LINEAR_SIZE:
        raise ValueError("invalid transform/linear payload size")
    if not _finite_bounded(_floats(frame.transform + frame.linear_velocity), 1_000_000.0):
        raise ValueError("invalid transform/linear component")
    if frame.flags != REQUIRED_FRAME_FLAGS:
        raise ValueError("unsupported unified frame flags")


def encode_recording(
    frames: Iterable[UnifiedTickFrameV1], *, fixed_interval_us: int
) -> bytes:
    materialized = tuple(frames)
    if not 1 <= len(materialized) <= MAX_FRAMES:
        raise ValueError(f"frame count must be in 1..{MAX_FRAMES}")
    if not 1000 <= fixed_interval_us <= 100_000:
        raise ValueError("fixed_interval_us must be in 1000..100000")
    output = bytearray(
        _HEADER.pack(
            MAGIC,
            VERSION,
            HEADER_SIZE,
            FRAME_SIZE,
            len(materialized),
            fixed_interval_us,
            REQUIRED_HEADER_FLAGS,
            SUPPORTED_SKIP_MASK,
            SUPPORTED_BUILD_ID,
            TRANSFORM_OFFSET,
            LINEAR_OFFSET,
            ANGULAR_OFFSET,
            TRANSFORM_SIZE,
            LINEAR_SIZE,
            ANGULAR_SIZE,
            bytes(16),
        )
    )
    previous_tick: int | None = None
    previous_time: int | None = None
    for frame in materialized:
        _validate_frame(frame)
        if previous_tick is not None and frame.tick != previous_tick + 1:
            raise ValueError("ticks must be contiguous")
        if previous_time is not None and frame.monotonic_ns < previous_time:
            raise ValueError("monotonic time moved backward")
        output += _FRAME.pack(
            frame.tick,
            frame.monotonic_ns,
            frame.steering,
            frame.brake,
            frame.accelerator,
            frame.nitro_activations,
            frame.skip_flags,
            frame.respawn,
            *frame.barrel_angular,
            *frame.barrel_rbx,
            frame.transform,
            frame.linear_velocity,
            frame.flags,
            0,
        )
        previous_tick = frame.tick
        previous_time = frame.monotonic_ns
    return bytes(output)


def decode_recording(blob: bytes) -> tuple[int, tuple[UnifiedTickFrameV1, ...]]:
    if len(blob) < HEADER_SIZE:
        raise ValueError("recording is shorter than the A9UTK1 header")
    values = _HEADER.unpack_from(blob)
    (
        magic,
        version,
        header_size,
        frame_size,
        frame_count,
        fixed_interval_us,
        header_flags,
        skip_mask,
        build_id,
        transform_offset,
        linear_offset,
        angular_offset,
        transform_size,
        linear_size,
        angular_size,
        reserved,
    ) = values
    if magic != MAGIC or version != VERSION:
        raise ValueError("unsupported A9UTK1 magic/version")
    if header_size != HEADER_SIZE or frame_size != FRAME_SIZE:
        raise ValueError("A9UTK1 ABI size mismatch")
    if not 1 <= frame_count <= MAX_FRAMES:
        raise ValueError("invalid A9UTK1 frame count")
    if not 1000 <= fixed_interval_us <= 100_000:
        raise ValueError("invalid A9UTK1 fixed interval")
    if header_flags != REQUIRED_HEADER_FLAGS or skip_mask != SUPPORTED_SKIP_MASK:
        raise ValueError("unsupported A9UTK1 semantics flags")
    if build_id != SUPPORTED_BUILD_ID:
        raise ValueError("A9UTK1 build ID mismatch")
    if (
        (transform_offset, linear_offset, angular_offset)
        != (TRANSFORM_OFFSET, LINEAR_OFFSET, ANGULAR_OFFSET)
        or (transform_size, linear_size, angular_size)
        != (TRANSFORM_SIZE, LINEAR_SIZE, ANGULAR_SIZE)
    ):
        raise ValueError("A9UTK1 native layout mismatch")
    if reserved != bytes(16):
        raise ValueError("A9UTK1 reserved header bytes are nonzero")
    expected_size = HEADER_SIZE + frame_count * FRAME_SIZE
    if len(blob) != expected_size:
        raise ValueError(f"A9UTK1 length must be exactly {expected_size} bytes")

    frames: list[UnifiedTickFrameV1] = []
    cursor = HEADER_SIZE
    previous_tick: int | None = None
    previous_time: int | None = None
    for index in range(frame_count):
        unpacked = _FRAME.unpack_from(blob, cursor)
        (
            tick,
            monotonic_ns,
            steering,
            brake,
            accelerator,
            nitro,
            skip_flags,
            respawn,
            angular_x,
            angular_y,
            angular_z,
            rbx_2228,
            rbx_222c,
            transform,
            linear,
            flags,
            frame_reserved,
        ) = unpacked
        frame = UnifiedTickFrameV1(
            tick,
            monotonic_ns,
            steering,
            brake,
            accelerator,
            nitro,
            skip_flags,
            bool(respawn),
            (angular_x, angular_y, angular_z),
            (rbx_2228, rbx_222c),
            transform,
            linear,
            flags,
        )
        _validate_frame(frame)
        if respawn not in (0, 1) or frame_reserved != 0:
            raise ValueError(f"frame {index}: noncanonical bool/reserved field")
        if previous_tick is not None and tick != previous_tick + 1:
            raise ValueError(f"frame {index}: tick is not contiguous")
        if previous_time is not None and monotonic_ns < previous_time:
            raise ValueError(f"frame {index}: monotonic time moved backward")
        frames.append(frame)
        previous_tick = tick
        previous_time = monotonic_ns
        cursor += FRAME_SIZE
    return fixed_interval_us, tuple(frames)


def plan_tick(
    frame: UnifiedTickFrameV1,
    *,
    current_transform: bytes,
    current_linear_velocity: bytes,
) -> TickPlanV1:
    _validate_frame(frame)
    correction = ()
    if not (frame.skip_flags & SKIP_TRANSFORM):
        correction = plan_transform_linear_correction(
            current_transform,
            current_linear_velocity,
            frame.transform,
            frame.linear_velocity,
        )
    angular = None
    if not (frame.skip_flags & SKIP_BARREL_ANGULAR):
        angular = struct.pack("<3f", *frame.barrel_angular)
    return TickPlanV1(
        None if frame.skip_flags & SKIP_STEER else frame.steering,
        None if frame.skip_flags & SKIP_BRAKE else frame.brake,
        None if frame.skip_flags & SKIP_ACCELERATOR else frame.accelerator,
        0 if frame.skip_flags & SKIP_NITRO else frame.nitro_activations,
        False if frame.skip_flags & SKIP_RESPAWN else frame.respawn,
        angular,
        None if frame.skip_flags & SKIP_BARREL_RBX else frame.barrel_rbx,
        correction,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    args = parser.parse_args()
    try:
        fixed_interval_us, frames = decode_recording(args.recording.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9utk1_error={error}")
        return 1
    print(
        f"a9utk1_frames={len(frames)} first_tick={frames[0].tick} "
        f"last_tick={frames[-1].tick} fixed_interval_us={fixed_interval_us}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
