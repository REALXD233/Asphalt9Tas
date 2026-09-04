#!/usr/bin/env python3
"""Create a short steering+final-correction A9UTK1 packet for Gate 9."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from make_unified_final_correction_gate_v1 import _component_mismatch_count
from make_unified_phase_only_gate_v1 import write_new_gate
from make_unified_steering_gate_v1 import decode_direction_source
from native_physics_recording_v1 import (
    decode_recording as decode_a9nps1,
    validate_runtime_safe,
)
from unified_tick_recording_v1 import (
    SKIP_ACCELERATOR,
    SKIP_BARREL_ANGULAR,
    SKIP_BARREL_RBX,
    SKIP_BRAKE,
    SKIP_NITRO,
    SKIP_RESPAWN,
    UnifiedTickFrameV1,
    decode_recording,
    encode_recording,
)
from verify_unified_phase_only_gate_v1 import verify_phase_only_gate


STEERING_FINAL_SKIP_FLAGS = (
    SKIP_BRAKE
    | SKIP_NITRO
    | SKIP_ACCELERATOR
    | SKIP_BARREL_ANGULAR
    | SKIP_BARREL_RBX
    | SKIP_RESPAWN
)
assert STEERING_FINAL_SKIP_FLAGS == 0x7E
GATE9_FRAME_COUNT = 3


def _parse_indices(text: str) -> tuple[int, ...]:
    try:
        values = tuple(int(value.strip(), 10) for value in text.split(","))
    except ValueError as error:
        raise ValueError("indices must be comma-separated decimal integers") from error
    if len(values) != GATE9_FRAME_COUNT:
        raise ValueError(f"exactly {GATE9_FRAME_COUNT} indices are required")
    if len(set(values)) != len(values):
        raise ValueError("indices must be unique")
    return values


def build_steering_final_gate(
    phase_blob: bytes,
    direction_blob: bytes,
    physics_blob: bytes,
    *,
    direction_indices: tuple[int, ...],
    physics_indices: tuple[int, ...],
) -> tuple[bytes, dict[str, object]]:
    if len(direction_indices) != GATE9_FRAME_COUNT or len(physics_indices) != GATE9_FRAME_COUNT:
        raise ValueError(f"Gate 9 requires exactly {GATE9_FRAME_COUNT} source indices")
    if len(set(direction_indices)) != GATE9_FRAME_COUNT or len(set(physics_indices)) != GATE9_FRAME_COUNT:
        raise ValueError("Gate 9 source indices must be unique")

    phase_tick, fixed_interval_us, phase_digest = verify_phase_only_gate(phase_blob)
    _, phase_frames = decode_recording(phase_blob)
    phase = phase_frames[0]
    direction_frames = decode_direction_source(direction_blob)
    physics_frames = decode_a9nps1(physics_blob)
    validate_runtime_safe(physics_frames)

    if any(not 0 <= index < len(direction_frames) for index in direction_indices):
        raise ValueError("direction source index is outside the recording")
    if any(not 0 <= index < len(physics_frames) for index in physics_indices):
        raise ValueError("physics source index is outside the recording")

    selected_physics = tuple(physics_frames[index] for index in physics_indices)
    targets = tuple(frame.transform + frame.linear_velocity for frame in selected_physics)
    reference_payloads = (phase.transform + phase.linear_velocity,) + targets[:-1]
    mismatch_counts = tuple(
        _component_mismatch_count(reference, target)
        for reference, target in zip(reference_payloads, targets, strict=True)
    )
    if any(count < 1 for count in mismatch_counts):
        raise ValueError("each Gate 9 target must differ from its preceding reference")

    output_frames: list[UnifiedTickFrameV1] = []
    steering_bits: list[str] = []
    for offset, (direction_index, source) in enumerate(
        zip(direction_indices, selected_physics, strict=True)
    ):
        steering, longitudinal, value_c = direction_frames[direction_index]
        if not 0.25 <= abs(steering) <= 0.75:
            raise ValueError("selected steering magnitude must be in 0.25..0.75")
        if struct.pack("<f", longitudinal) != bytes(4) or struct.pack("<f", value_c) != bytes(4):
            raise ValueError("selected direction source must have dormant +0 values")
        output_frames.append(
            UnifiedTickFrameV1(
                tick=phase.tick + offset,
                monotonic_ns=phase.monotonic_ns + offset * fixed_interval_us * 1000,
                steering=steering,
                brake=0.0,
                accelerator=0.0,
                nitro_activations=0,
                skip_flags=STEERING_FINAL_SKIP_FLAGS,
                respawn=False,
                barrel_angular=(0.0, 0.0, 0.0),
                barrel_rbx=(0.0, 0.0),
                transform=source.transform,
                linear_velocity=source.linear_velocity,
            )
        )
        steering_bits.append(struct.pack("<f", steering).hex())

    output_blob = encode_recording(output_frames, fixed_interval_us=fixed_interval_us)
    manifest: dict[str, object] = {
        "schema": "A9UTK1_STEERING_FINAL_GATE_MANIFEST_V1",
        "output_format": "A9UTK1",
        "output_sha256": hashlib.sha256(output_blob).hexdigest(),
        "phase_source_format": "A9UTK1",
        "phase_source_sha256": phase_digest,
        "direction_source_format": "A9SPR1",
        "direction_source_sha256": hashlib.sha256(direction_blob).hexdigest(),
        "direction_source_frame_count": len(direction_frames),
        "direction_source_indices": list(direction_indices),
        "steering_bits_hex": steering_bits,
        "physics_source_format": "A9NPS1",
        "physics_source_sha256": hashlib.sha256(physics_blob).hexdigest(),
        "physics_source_frame_count": len(physics_frames),
        "physics_source_indices": list(physics_indices),
        "physics_source_ticks": [frame.tick for frame in selected_physics],
        "physics_source_monotonic_ns": [
            frame.monotonic_ns for frame in selected_physics
        ],
        "payload_sha256": [hashlib.sha256(payload).hexdigest() for payload in targets],
        "preceding_reference_mismatch_components": list(mismatch_counts),
        "selected_first_tick": output_frames[0].tick,
        "selected_last_tick": output_frames[-1].tick,
        "selected_first_monotonic_ns": output_frames[0].monotonic_ns,
        "fixed_interval_us": fixed_interval_us,
        "skip_flags": STEERING_FINAL_SKIP_FLAGS,
        "expected_processed_frames": GATE9_FRAME_COUNT,
        "expected_delta_writes": GATE9_FRAME_COUNT,
        "expected_control_writes": GATE9_FRAME_COUNT * 2,
        "expected_corrected_frames": GATE9_FRAME_COUNT,
        "expected_final_transactions": GATE9_FRAME_COUNT,
        "expected_final_payload_writes": GATE9_FRAME_COUNT * 2,
        "purpose": "short_multiframe_steering_final_integration_gate_only",
        "does_not_claim": [
            "trajectory_determinism",
            "long_replay_stability",
            "recording_end_behavior",
            "brake_or_accelerator_replay",
            "nitro_or_respawn_replay",
            "barrel_replay",
        ],
    }
    return output_blob, manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase_source", type=Path)
    parser.add_argument("direction_source", type=Path)
    parser.add_argument("physics_source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--direction-indices", default="25,26,27")
    parser.add_argument("--physics-indices", default="0,1,3")
    args = parser.parse_args()
    try:
        direction_indices = _parse_indices(args.direction_indices)
        physics_indices = _parse_indices(args.physics_indices)
        output_blob, manifest = build_steering_final_gate(
            args.phase_source.read_bytes(),
            args.direction_source.read_bytes(),
            args.physics_source.read_bytes(),
            direction_indices=direction_indices,
            physics_indices=physics_indices,
        )
        manifest["phase_source_name"] = args.phase_source.name
        manifest["direction_source_name"] = args.direction_source.name
        manifest["physics_source_name"] = args.physics_source.name
        manifest["output_name"] = args.output.name
        write_new_gate(args.output, args.manifest, output_blob, manifest)
    except (OSError, ValueError) as error:
        print(f"steering_final_gate_error={error}")
        return 1
    print(
        f"steering_final_gate_created=1 frames={manifest['expected_processed_frames']} "
        f"ticks={manifest['selected_first_tick']}..{manifest['selected_last_tick']} "
        f"mismatches={manifest['preceding_reference_mismatch_components']} "
        f"skip_flags=0x{manifest['skip_flags']:02x} "
        f"sha256={manifest['output_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
