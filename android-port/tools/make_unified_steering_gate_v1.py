#!/usr/bin/env python3
"""Create one steering-only A9UTK1 frame for the Gate 7 integration test."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from pathlib import Path

from make_unified_phase_only_gate_v1 import write_new_gate
from unified_tick_recording_v1 import (
    SKIP_ACCELERATOR,
    SKIP_BARREL_ANGULAR,
    SKIP_BARREL_RBX,
    SKIP_BRAKE,
    SKIP_NITRO,
    SKIP_RESPAWN,
    SKIP_TRANSFORM,
    UnifiedTickFrameV1,
    decode_recording,
    encode_recording,
)
from verify_unified_phase_only_gate_v1 import verify_phase_only_gate


STEERING_ONLY_SKIP_FLAGS = (
    SKIP_BRAKE
    | SKIP_NITRO
    | SKIP_ACCELERATOR
    | SKIP_BARREL_ANGULAR
    | SKIP_BARREL_RBX
    | SKIP_RESPAWN
    | SKIP_TRANSFORM
)
assert STEERING_ONLY_SKIP_FLAGS == 0xFE

_A9SPR1_HEADER = struct.Struct("<8s4I20s20s")
_A9SPR1_FRAME = struct.Struct("<fffI")
_BUILD_ID = bytes.fromhex("e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b")


def _positive_zero(value: float) -> bool:
    return struct.pack("<f", value) == bytes(4)


def decode_direction_source(blob: bytes) -> tuple[tuple[float, float, float], ...]:
    if len(blob) < _A9SPR1_HEADER.size:
        raise ValueError("direction source is shorter than A9SPR1 header")
    magic, version, header_size, frame_size, count, build_id, reserved = (
        _A9SPR1_HEADER.unpack_from(blob)
    )
    if magic != b"A9SPR1\0\0" or version != 1:
        raise ValueError("unsupported direction source magic/version")
    if header_size != 64 or frame_size != 16 or count < 1:
        raise ValueError("invalid direction source ABI/count")
    if build_id != _BUILD_ID or reserved != bytes(20):
        raise ValueError("direction source identity/reserved mismatch")
    if len(blob) != header_size + count * frame_size:
        raise ValueError("direction source length mismatch")
    frames: list[tuple[float, float, float]] = []
    for index in range(count):
        steering, longitudinal, value_c, flags = _A9SPR1_FRAME.unpack_from(
            blob, header_size + index * frame_size
        )
        if (
            flags != 7
            or not all(
                math.isfinite(value) and abs(value) <= 8.0
                for value in (steering, longitudinal, value_c)
            )
        ):
            raise ValueError(f"direction source frame {index} is invalid")
        frames.append((steering, longitudinal, value_c))
    return tuple(frames)


def build_steering_gate(
    phase_blob: bytes,
    direction_blob: bytes,
    *,
    direction_frame_index: int,
) -> tuple[bytes, dict[str, object]]:
    phase_tick, fixed_interval_us, phase_digest = verify_phase_only_gate(phase_blob)
    _, phase_frames = decode_recording(phase_blob)
    phase = phase_frames[0]
    direction_frames = decode_direction_source(direction_blob)
    if not 0 <= direction_frame_index < len(direction_frames):
        raise ValueError(
            f"direction_frame_index must be in 0..{len(direction_frames) - 1}"
        )
    steering, longitudinal, value_c = direction_frames[direction_frame_index]
    if not 0.25 <= abs(steering) <= 0.75:
        raise ValueError("selected steering magnitude must be in 0.25..0.75")
    if not _positive_zero(longitudinal) or not _positive_zero(value_c):
        raise ValueError("selected source frame must have dormant +0 longitudinal/value_c")

    gate_frame = UnifiedTickFrameV1(
        tick=phase.tick,
        monotonic_ns=phase.monotonic_ns,
        steering=steering,
        brake=0.0,
        accelerator=0.0,
        nitro_activations=0,
        skip_flags=STEERING_ONLY_SKIP_FLAGS,
        respawn=False,
        barrel_angular=(0.0, 0.0, 0.0),
        barrel_rbx=(0.0, 0.0),
        transform=phase.transform,
        linear_velocity=phase.linear_velocity,
    )
    output_blob = encode_recording([gate_frame], fixed_interval_us=fixed_interval_us)
    steering_bits = struct.pack("<f", steering).hex()
    manifest: dict[str, object] = {
        "schema": "A9UTK1_STEERING_ONLY_GATE_MANIFEST_V1",
        "output_format": "A9UTK1",
        "output_sha256": hashlib.sha256(output_blob).hexdigest(),
        "phase_source_sha256": phase_digest,
        "direction_source_format": "A9SPR1",
        "direction_source_sha256": hashlib.sha256(direction_blob).hexdigest(),
        "direction_source_frame_count": len(direction_frames),
        "direction_frame_index": direction_frame_index,
        "steering_bits_hex": steering_bits,
        "selected_tick": phase_tick,
        "selected_monotonic_ns": phase.monotonic_ns,
        "fixed_interval_us": fixed_interval_us,
        "skip_flags": STEERING_ONLY_SKIP_FLAGS,
        "expected_processed_frames": 1,
        "expected_control_writes": 2,
        "expected_final_correction": "skipped_zero_write",
        "purpose": "unified_steering_integration_gate_only",
        "does_not_claim": [
            "trajectory_determinism",
            "brake_or_accelerator_replay",
            "nitro_or_respawn_replay",
            "barrel_replay",
            "transform_equal_or_correction",
        ],
    }
    return output_blob, manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase_source", type=Path)
    parser.add_argument("direction_source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--direction-frame-index", type=int, default=25)
    args = parser.parse_args()
    try:
        output_blob, manifest = build_steering_gate(
            args.phase_source.read_bytes(),
            args.direction_source.read_bytes(),
            direction_frame_index=args.direction_frame_index,
        )
        manifest["phase_source_name"] = args.phase_source.name
        manifest["direction_source_name"] = args.direction_source.name
        manifest["output_name"] = args.output.name
        write_new_gate(args.output, args.manifest, output_blob, manifest)
    except (OSError, ValueError) as error:
        print(f"steering_gate_error={error}")
        return 1
    print(
        f"steering_gate_created=1 tick={manifest['selected_tick']} "
        f"steering_bits={manifest['steering_bits_hex']} "
        f"skip_flags=0x{manifest['skip_flags']:02x} "
        f"sha256={manifest['output_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
