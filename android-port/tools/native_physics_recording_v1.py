#!/usr/bin/env python3
"""Strict A9NPS1 native transform/linear recording codec.

The on-disk payload preserves float bits exactly.  Structural decoding does
not silently canonicalize NaNs or signed zero.  Runtime safety validation is a
separate explicit step so corrupt/non-finite captures can be rejected before
they ever reach a process-writing executor.
"""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from transform_correction_semantics_v1 import (
    LINEAR_VELOCITY_SIZE,
    TRANSFORM_SIZE,
    plan_transform_linear_correction,
)


MAGIC = b"A9NPS1\0\0"
VERSION = 1
HEADER_SIZE = 64
FRAME_SIZE = 96
MAX_FRAMES = 10_000_000
SUPPORTED_BUILD_ID = bytes.fromhex("e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b")
TRANSFORM_OFFSET = 0x10
LINEAR_VELOCITY_OFFSET = 0x150

HEADER_RAW_FLOAT_BITS = 1 << 0
HEADER_COMPONENT_FLOAT_COMPARISON = 1 << 1
HEADER_ALL_OR_NOTHING_COPY = 1 << 2
REQUIRED_HEADER_FLAGS = (
    HEADER_RAW_FLOAT_BITS
    | HEADER_COMPONENT_FLOAT_COMPARISON
    | HEADER_ALL_OR_NOTHING_COPY
)
FRAME_CAPTURED_AT_CERTIFIED_BOUNDARY = 1 << 0
REQUIRED_FRAME_FLAGS = FRAME_CAPTURED_AT_CERTIFIED_BOUNDARY

_HEADER = struct.Struct("<8sIIII20sIIIII")
_FRAME_PREFIX = struct.Struct("<QQ")
_FRAME_SUFFIX = struct.Struct("<I")

assert _HEADER.size == HEADER_SIZE
assert _FRAME_PREFIX.size + TRANSFORM_SIZE + LINEAR_VELOCITY_SIZE + _FRAME_SUFFIX.size == FRAME_SIZE


@dataclass(frozen=True)
class NativePhysicsFrameV1:
    tick: int
    monotonic_ns: int
    transform: bytes
    linear_velocity: bytes
    flags: int = REQUIRED_FRAME_FLAGS


def _require_payload_size(name: str, payload: bytes, expected: int) -> None:
    if len(payload) != expected:
        raise ValueError(f"{name} must be exactly {expected} bytes, got {len(payload)}")


def _validate_frame_structure(frame: NativePhysicsFrameV1) -> None:
    if not 0 <= frame.tick <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("tick is outside uint64 range")
    if not 0 <= frame.monotonic_ns <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("monotonic_ns is outside uint64 range")
    _require_payload_size("transform", frame.transform, TRANSFORM_SIZE)
    _require_payload_size(
        "linear_velocity", frame.linear_velocity, LINEAR_VELOCITY_SIZE
    )
    if frame.flags != REQUIRED_FRAME_FLAGS:
        raise ValueError(f"unsupported frame flags 0x{frame.flags:x}")


def encode_recording(frames: Iterable[NativePhysicsFrameV1]) -> bytes:
    """Encode an exact-build A9NPS1 recording without changing float bits."""

    materialized = tuple(frames)
    if not materialized or len(materialized) > MAX_FRAMES:
        raise ValueError(f"frame count must be in 1..{MAX_FRAMES}")
    for frame in materialized:
        _validate_frame_structure(frame)

    output = bytearray(
        _HEADER.pack(
            MAGIC,
            VERSION,
            HEADER_SIZE,
            FRAME_SIZE,
            len(materialized),
            SUPPORTED_BUILD_ID,
            TRANSFORM_OFFSET,
            LINEAR_VELOCITY_OFFSET,
            TRANSFORM_SIZE,
            LINEAR_VELOCITY_SIZE,
            REQUIRED_HEADER_FLAGS,
        )
    )
    for frame in materialized:
        output += _FRAME_PREFIX.pack(frame.tick, frame.monotonic_ns)
        output += frame.transform
        output += frame.linear_velocity
        output += _FRAME_SUFFIX.pack(frame.flags)
    return bytes(output)


def decode_recording(blob: bytes) -> tuple[NativePhysicsFrameV1, ...]:
    """Decode and strictly validate the A9NPS1 structure and exact build."""

    if len(blob) < HEADER_SIZE:
        raise ValueError("recording is shorter than the A9NPS1 header")
    (
        magic,
        version,
        header_size,
        frame_size,
        frame_count,
        build_id,
        transform_offset,
        linear_offset,
        transform_size,
        linear_size,
        header_flags,
    ) = _HEADER.unpack_from(blob)
    if magic != MAGIC:
        raise ValueError("invalid A9NPS1 magic")
    if version != VERSION or header_size != HEADER_SIZE or frame_size != FRAME_SIZE:
        raise ValueError("unsupported A9NPS1 ABI")
    if build_id != SUPPORTED_BUILD_ID:
        raise ValueError("recording build ID does not match the supported game build")
    if (
        transform_offset != TRANSFORM_OFFSET
        or linear_offset != LINEAR_VELOCITY_OFFSET
        or transform_size != TRANSFORM_SIZE
        or linear_size != LINEAR_VELOCITY_SIZE
    ):
        raise ValueError("recording native-body layout does not match A9NPS1")
    if header_flags != REQUIRED_HEADER_FLAGS:
        raise ValueError(f"unsupported header flags 0x{header_flags:x}")
    if not 1 <= frame_count <= MAX_FRAMES:
        raise ValueError("invalid A9NPS1 frame count")
    expected_size = HEADER_SIZE + frame_count * FRAME_SIZE
    if len(blob) != expected_size:
        raise ValueError(
            f"invalid A9NPS1 length: expected {expected_size}, got {len(blob)}"
        )

    frames: list[NativePhysicsFrameV1] = []
    cursor = HEADER_SIZE
    for _ in range(frame_count):
        tick, monotonic_ns = _FRAME_PREFIX.unpack_from(blob, cursor)
        transform_start = cursor + _FRAME_PREFIX.size
        linear_start = transform_start + TRANSFORM_SIZE
        flags_start = linear_start + LINEAR_VELOCITY_SIZE
        frame = NativePhysicsFrameV1(
            tick=tick,
            monotonic_ns=monotonic_ns,
            transform=bytes(blob[transform_start:linear_start]),
            linear_velocity=bytes(blob[linear_start:flags_start]),
            flags=_FRAME_SUFFIX.unpack_from(blob, flags_start)[0],
        )
        _validate_frame_structure(frame)
        frames.append(frame)
        cursor += FRAME_SIZE
    return tuple(frames)


def validate_runtime_safe(frames: Iterable[NativePhysicsFrameV1]) -> None:
    """Fail closed on chronology errors or non-finite physics components."""

    materialized = tuple(frames)
    if not materialized:
        raise ValueError("recording contains no frames")
    previous_tick: int | None = None
    previous_time: int | None = None
    for index, frame in enumerate(materialized):
        _validate_frame_structure(frame)
        if previous_tick is not None and frame.tick != previous_tick + 1:
            raise ValueError(f"non-contiguous tick at frame {index}")
        if previous_time is not None and frame.monotonic_ns < previous_time:
            raise ValueError(f"monotonic time moved backward at frame {index}")
        components = struct.iter_unpack(
            "<f", frame.transform + frame.linear_velocity
        )
        if not all(math.isfinite(component) for (component,) in components):
            raise ValueError(f"non-finite physics component at frame {index}")
        previous_tick = frame.tick
        previous_time = frame.monotonic_ns


def plan_frame_correction(
    current_transform: bytes,
    current_linear_velocity: bytes,
    frame: NativePhysicsFrameV1,
):
    """Apply the already-tested AluTasV2 comparison/copy contract to a frame."""

    _validate_frame_structure(frame)
    return plan_transform_linear_correction(
        current_transform,
        current_linear_velocity,
        frame.transform,
        frame.linear_velocity,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--require-runtime-safe", action="store_true")
    args = parser.parse_args()
    try:
        frames = decode_recording(args.recording.read_bytes())
        if args.require_runtime_safe:
            validate_runtime_safe(frames)
    except (OSError, ValueError, struct.error) as error:
        print(f"a9nps1_error={error}")
        return 1
    first = frames[0]
    last = frames[-1]
    print(
        f"a9nps1_frames={len(frames)} first_tick={first.tick} "
        f"last_tick={last.tick} runtime_safe={int(args.require_runtime_safe)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
