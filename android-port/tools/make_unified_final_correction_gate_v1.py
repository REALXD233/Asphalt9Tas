#!/usr/bin/env python3
"""Create one final-correction-only A9UTK1 frame for Gate 8."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from make_unified_phase_only_gate_v1 import write_new_gate
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
    SKIP_STEER,
    UnifiedTickFrameV1,
    decode_recording,
    encode_recording,
)
from verify_unified_phase_only_gate_v1 import verify_phase_only_gate


FINAL_CORRECTION_ONLY_SKIP_FLAGS = (
    SKIP_STEER
    | SKIP_BRAKE
    | SKIP_NITRO
    | SKIP_ACCELERATOR
    | SKIP_BARREL_ANGULAR
    | SKIP_BARREL_RBX
    | SKIP_RESPAWN
)
assert FINAL_CORRECTION_ONLY_SKIP_FLAGS == 0x7F


def _component_mismatch_count(left: bytes, right: bytes) -> int:
    return sum(
        a != b
        for a, b in zip(
            struct.unpack("<19f", left), struct.unpack("<19f", right), strict=True
        )
    )


def build_final_correction_gate(
    phase_blob: bytes,
    physics_blob: bytes,
    *,
    physics_frame_index: int,
) -> tuple[bytes, dict[str, object]]:
    phase_tick, fixed_interval_us, phase_digest = verify_phase_only_gate(phase_blob)
    _, phase_frames = decode_recording(phase_blob)
    phase = phase_frames[0]

    physics_frames = decode_a9nps1(physics_blob)
    validate_runtime_safe(physics_frames)
    if not 0 <= physics_frame_index < len(physics_frames):
        raise ValueError(
            f"physics_frame_index must be in 0..{len(physics_frames) - 1}"
        )
    source = physics_frames[physics_frame_index]
    phase_payload = phase.transform + phase.linear_velocity
    source_payload = source.transform + source.linear_velocity
    mismatch_count = _component_mismatch_count(phase_payload, source_payload)
    if mismatch_count < 1:
        raise ValueError("physics source equals the phase reference in all 19 components")

    gate_frame = UnifiedTickFrameV1(
        tick=phase.tick,
        monotonic_ns=phase.monotonic_ns,
        steering=0.0,
        brake=0.0,
        accelerator=0.0,
        nitro_activations=0,
        skip_flags=FINAL_CORRECTION_ONLY_SKIP_FLAGS,
        respawn=False,
        barrel_angular=(0.0, 0.0, 0.0),
        barrel_rbx=(0.0, 0.0),
        transform=source.transform,
        linear_velocity=source.linear_velocity,
    )
    output_blob = encode_recording([gate_frame], fixed_interval_us=fixed_interval_us)
    manifest: dict[str, object] = {
        "schema": "A9UTK1_FINAL_CORRECTION_ONLY_GATE_MANIFEST_V1",
        "output_format": "A9UTK1",
        "output_sha256": hashlib.sha256(output_blob).hexdigest(),
        "phase_source_format": "A9UTK1",
        "phase_source_sha256": phase_digest,
        "physics_source_format": "A9NPS1",
        "physics_source_sha256": hashlib.sha256(physics_blob).hexdigest(),
        "physics_source_frame_count": len(physics_frames),
        "physics_source_frame_index": physics_frame_index,
        "physics_source_tick": source.tick,
        "physics_source_monotonic_ns": source.monotonic_ns,
        "payload_sha256": hashlib.sha256(source_payload).hexdigest(),
        "phase_reference_mismatch_components": mismatch_count,
        "selected_tick": phase_tick,
        "selected_monotonic_ns": phase.monotonic_ns,
        "fixed_interval_us": fixed_interval_us,
        "skip_flags": FINAL_CORRECTION_ONLY_SKIP_FLAGS,
        "expected_processed_frames": 1,
        "expected_control_writes": 0,
        "expected_final_correction": "different_value_one_transaction",
        "expected_final_payload_writes": 2,
        "purpose": "unified_final_correction_integration_gate_only",
        "does_not_claim": [
            "trajectory_determinism",
            "equal_zero_write_branch",
            "steering_replay",
            "brake_or_accelerator_replay",
            "nitro_or_respawn_replay",
            "barrel_replay",
        ],
    }
    return output_blob, manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("phase_source", type=Path)
    parser.add_argument("physics_source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--physics-frame-index", type=int, default=0)
    args = parser.parse_args()
    try:
        output_blob, manifest = build_final_correction_gate(
            args.phase_source.read_bytes(),
            args.physics_source.read_bytes(),
            physics_frame_index=args.physics_frame_index,
        )
        manifest["phase_source_name"] = args.phase_source.name
        manifest["physics_source_name"] = args.physics_source.name
        manifest["output_name"] = args.output.name
        write_new_gate(args.output, args.manifest, output_blob, manifest)
    except (OSError, ValueError) as error:
        print(f"final_correction_gate_error={error}")
        return 1
    print(
        f"final_correction_gate_created=1 tick={manifest['selected_tick']} "
        f"source_frame={manifest['physics_source_frame_index']} "
        f"mismatch_components={manifest['phase_reference_mismatch_components']} "
        f"skip_flags=0x{manifest['skip_flags']:02x} "
        f"sha256={manifest['output_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
