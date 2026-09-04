#!/usr/bin/env python3
"""Strict verifier for the one-frame final-correction-only Gate 8 input."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from make_unified_final_correction_gate_v1 import (
    FINAL_CORRECTION_ONLY_SKIP_FLAGS,
    _component_mismatch_count,
)
from native_physics_recording_v1 import (
    decode_recording as decode_a9nps1,
    validate_runtime_safe,
)
from unified_tick_recording_v1 import decode_recording, plan_tick
from verify_unified_phase_only_gate_v1 import verify_phase_only_gate


def _positive_zero(value: float) -> bool:
    return struct.pack("<f", value) == bytes(4)


def verify_final_correction_gate(
    blob: bytes,
    manifest: dict[str, object],
    phase_blob: bytes,
    physics_blob: bytes,
) -> tuple[int, int, str, int]:
    fixed_interval_us, frames = decode_recording(blob)
    if len(frames) != 1:
        raise ValueError("final-correction gate must contain exactly one frame")
    frame = frames[0]
    if frame.skip_flags != FINAL_CORRECTION_ONLY_SKIP_FLAGS:
        raise ValueError("final-correction gate must enable only transform+linear")
    dormant = (
        frame.steering,
        frame.brake,
        frame.accelerator,
        *frame.barrel_angular,
        *frame.barrel_rbx,
    )
    if not all(_positive_zero(value) for value in dormant):
        raise ValueError("final-correction gate dormant float is not canonical +0.0")
    if frame.nitro_activations != 0 or frame.respawn:
        raise ValueError("final-correction gate contains an active transient action")

    phase_tick, phase_interval, phase_digest = verify_phase_only_gate(phase_blob)
    _, phase_frames = decode_recording(phase_blob)
    if frame.tick != phase_tick or frame.monotonic_ns != phase_frames[0].monotonic_ns:
        raise ValueError("final-correction gate phase identity mismatch")
    if fixed_interval_us != phase_interval:
        raise ValueError("final-correction gate fixed interval differs from phase source")

    physics_frames = decode_a9nps1(physics_blob)
    validate_runtime_safe(physics_frames)
    source_index = manifest.get("physics_source_frame_index")
    if type(source_index) is not int or not 0 <= source_index < len(physics_frames):
        raise ValueError("manifest physics source frame index is invalid")
    source = physics_frames[source_index]
    if frame.transform != source.transform or frame.linear_velocity != source.linear_velocity:
        raise ValueError("final-correction payload is not bound to the A9NPS1 source")

    phase_payload = phase_frames[0].transform + phase_frames[0].linear_velocity
    payload = frame.transform + frame.linear_velocity
    mismatch_count = _component_mismatch_count(phase_payload, payload)
    if mismatch_count < 1:
        raise ValueError("final-correction payload does not differ from phase reference")
    plan = plan_tick(
        frame,
        current_transform=phase_frames[0].transform,
        current_linear_velocity=phase_frames[0].linear_velocity,
    )
    if (
        any(
            value is not None
            for value in (
                plan.steering,
                plan.brake,
                plan.accelerator,
                plan.barrel_angular,
                plan.barrel_rbx,
            )
        )
        or plan.nitro_activations != 0
        or plan.respawn
        or len(plan.transform_linear_writes) != 2
    ):
        raise ValueError("final-correction gate planner scope is not 64+12 only")

    digest = hashlib.sha256(blob).hexdigest()
    payload_digest = hashlib.sha256(payload).hexdigest()
    required = {
        "schema": "A9UTK1_FINAL_CORRECTION_ONLY_GATE_MANIFEST_V1",
        "output_format": "A9UTK1",
        "output_sha256": digest,
        "phase_source_format": "A9UTK1",
        "phase_source_sha256": phase_digest,
        "physics_source_format": "A9NPS1",
        "physics_source_sha256": hashlib.sha256(physics_blob).hexdigest(),
        "physics_source_frame_count": len(physics_frames),
        "physics_source_tick": source.tick,
        "physics_source_monotonic_ns": source.monotonic_ns,
        "payload_sha256": payload_digest,
        "phase_reference_mismatch_components": mismatch_count,
        "selected_tick": frame.tick,
        "selected_monotonic_ns": frame.monotonic_ns,
        "fixed_interval_us": fixed_interval_us,
        "skip_flags": FINAL_CORRECTION_ONLY_SKIP_FLAGS,
        "expected_processed_frames": 1,
        "expected_control_writes": 0,
        "expected_final_correction": "different_value_one_transaction",
        "expected_final_payload_writes": 2,
        "purpose": "unified_final_correction_integration_gate_only",
    }
    for key, expected in required.items():
        if manifest.get(key) != expected:
            raise ValueError(f"manifest mismatch for {key}")
    claims = manifest.get("does_not_claim")
    if not isinstance(claims, list) or "trajectory_determinism" not in claims:
        raise ValueError("manifest does_not_claim is incomplete")
    return frame.tick, fixed_interval_us, digest, mismatch_count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--phase-source", type=Path, required=True)
    parser.add_argument("--physics-source", type=Path, required=True)
    args = parser.parse_args()
    try:
        loaded = json.loads(args.manifest.read_text(encoding="utf-8"))
        if not isinstance(loaded, dict):
            raise ValueError("manifest root must be an object")
        tick, interval, digest, mismatches = verify_final_correction_gate(
            args.recording.read_bytes(),
            loaded,
            args.phase_source.read_bytes(),
            args.physics_source.read_bytes(),
        )
    except (OSError, ValueError, json.JSONDecodeError, struct.error) as error:
        print(f"final_correction_gate_error={error}")
        return 1
    print(
        f"final_correction_gate_supported=1 frames=1 tick={tick} "
        f"fixed_interval_us={interval} mismatch_components={mismatches} "
        f"skip_flags=0x7f control_writes=0 final_transactions=1 "
        f"payload_writes=2 sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
