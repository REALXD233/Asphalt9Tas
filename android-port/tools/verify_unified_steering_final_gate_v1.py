#!/usr/bin/env python3
"""Strict verifier for the three-frame steering+final Gate 9 input."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from make_unified_final_correction_gate_v1 import _component_mismatch_count
from make_unified_steering_final_gate_v1 import GATE9_FRAME_COUNT, STEERING_FINAL_SKIP_FLAGS
from make_unified_steering_gate_v1 import decode_direction_source
from native_physics_recording_v1 import (
    decode_recording as decode_a9nps1,
    validate_runtime_safe,
)
from unified_tick_recording_v1 import decode_recording, plan_tick
from verify_unified_phase_only_gate_v1 import verify_phase_only_gate


def _positive_zero(value: float) -> bool:
    return struct.pack("<f", value) == bytes(4)


def _require_index_list(
    manifest: dict[str, object], key: str, source_count: int
) -> tuple[int, ...]:
    value = manifest.get(key)
    if not isinstance(value, list) or len(value) != GATE9_FRAME_COUNT:
        raise ValueError(f"manifest {key} must contain exactly three entries")
    if any(type(index) is not int or not 0 <= index < source_count for index in value):
        raise ValueError(f"manifest {key} contains an invalid index")
    indices = tuple(value)
    if len(set(indices)) != GATE9_FRAME_COUNT:
        raise ValueError(f"manifest {key} entries must be unique")
    return indices


def verify_steering_final_gate(
    blob: bytes,
    manifest: dict[str, object],
    phase_blob: bytes,
    direction_blob: bytes,
    physics_blob: bytes,
) -> tuple[int, int, int, str, tuple[str, ...], tuple[int, ...]]:
    fixed_interval_us, frames = decode_recording(blob)
    if len(frames) != GATE9_FRAME_COUNT:
        raise ValueError("Gate 9 input must contain exactly three frames")
    phase_tick, phase_interval, phase_digest = verify_phase_only_gate(phase_blob)
    _, phase_frames = decode_recording(phase_blob)
    if fixed_interval_us != phase_interval:
        raise ValueError("Gate 9 fixed interval differs from phase source")
    direction_frames = decode_direction_source(direction_blob)
    physics_frames = decode_a9nps1(physics_blob)
    validate_runtime_safe(physics_frames)
    direction_indices = _require_index_list(
        manifest, "direction_source_indices", len(direction_frames)
    )
    physics_indices = _require_index_list(
        manifest, "physics_source_indices", len(physics_frames)
    )

    steering_bits: list[str] = []
    mismatch_counts: list[int] = []
    previous_payload = phase_frames[0].transform + phase_frames[0].linear_velocity
    for offset, (frame, direction_index, physics_index) in enumerate(
        zip(frames, direction_indices, physics_indices, strict=True)
    ):
        if frame.tick != phase_tick + offset:
            raise ValueError(f"frame {offset}: tick is not phase-relative contiguous")
        expected_time = phase_frames[0].monotonic_ns + offset * fixed_interval_us * 1000
        if frame.monotonic_ns != expected_time:
            raise ValueError(f"frame {offset}: synthetic monotonic time mismatch")
        if frame.skip_flags != STEERING_FINAL_SKIP_FLAGS:
            raise ValueError(f"frame {offset}: Gate 9 must enable only steering and final correction")
        dormant = (
            frame.brake,
            frame.accelerator,
            *frame.barrel_angular,
            *frame.barrel_rbx,
        )
        if not all(_positive_zero(value) for value in dormant):
            raise ValueError(f"frame {offset}: dormant float is not canonical +0.0")
        if frame.nitro_activations != 0 or frame.respawn:
            raise ValueError(f"frame {offset}: active transient action is forbidden")

        source_direction = direction_frames[direction_index]
        if struct.pack("<f", frame.steering) != struct.pack("<f", source_direction[0]):
            raise ValueError(f"frame {offset}: steering is not bound to A9SPR1")
        if not 0.25 <= abs(frame.steering) <= 0.75:
            raise ValueError(f"frame {offset}: steering magnitude is outside Gate 9 range")
        if not _positive_zero(source_direction[1]) or not _positive_zero(source_direction[2]):
            raise ValueError(f"frame {offset}: A9SPR1 dormant values are not +0.0")
        steering_bits.append(struct.pack("<f", frame.steering).hex())

        source_physics = physics_frames[physics_index]
        if frame.transform != source_physics.transform or frame.linear_velocity != source_physics.linear_velocity:
            raise ValueError(f"frame {offset}: physics payload is not bound to A9NPS1")
        payload = frame.transform + frame.linear_velocity
        mismatch = _component_mismatch_count(previous_payload, payload)
        if mismatch < 1:
            raise ValueError(f"frame {offset}: target equals its preceding reference")
        mismatch_counts.append(mismatch)
        plan = plan_tick(
            frame,
            current_transform=previous_payload[:64],
            current_linear_velocity=previous_payload[64:],
        )
        if (
            plan.steering != frame.steering
            or any(
                value is not None
                for value in (
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
            raise ValueError(f"frame {offset}: planner scope is not steering+64+12")
        previous_payload = payload

    digest = hashlib.sha256(blob).hexdigest()
    targets = tuple(frame.transform + frame.linear_velocity for frame in frames)
    selected_physics = tuple(physics_frames[index] for index in physics_indices)
    required = {
        "schema": "A9UTK1_STEERING_FINAL_GATE_MANIFEST_V1",
        "output_format": "A9UTK1",
        "output_sha256": digest,
        "phase_source_format": "A9UTK1",
        "phase_source_sha256": phase_digest,
        "direction_source_format": "A9SPR1",
        "direction_source_sha256": hashlib.sha256(direction_blob).hexdigest(),
        "direction_source_frame_count": len(direction_frames),
        "steering_bits_hex": steering_bits,
        "physics_source_format": "A9NPS1",
        "physics_source_sha256": hashlib.sha256(physics_blob).hexdigest(),
        "physics_source_frame_count": len(physics_frames),
        "physics_source_ticks": [frame.tick for frame in selected_physics],
        "physics_source_monotonic_ns": [frame.monotonic_ns for frame in selected_physics],
        "payload_sha256": [hashlib.sha256(payload).hexdigest() for payload in targets],
        "preceding_reference_mismatch_components": mismatch_counts,
        "selected_first_tick": frames[0].tick,
        "selected_last_tick": frames[-1].tick,
        "selected_first_monotonic_ns": frames[0].monotonic_ns,
        "fixed_interval_us": fixed_interval_us,
        "skip_flags": STEERING_FINAL_SKIP_FLAGS,
        "expected_processed_frames": GATE9_FRAME_COUNT,
        "expected_delta_writes": GATE9_FRAME_COUNT,
        "expected_control_writes": GATE9_FRAME_COUNT * 2,
        "expected_corrected_frames": GATE9_FRAME_COUNT,
        "expected_final_transactions": GATE9_FRAME_COUNT,
        "expected_final_payload_writes": GATE9_FRAME_COUNT * 2,
        "purpose": "short_multiframe_steering_final_integration_gate_only",
    }
    for key, expected in required.items():
        if manifest.get(key) != expected:
            raise ValueError(f"manifest mismatch for {key}")
    claims = manifest.get("does_not_claim")
    if not isinstance(claims, list) or "trajectory_determinism" not in claims:
        raise ValueError("manifest does_not_claim is incomplete")
    return (
        frames[0].tick,
        frames[-1].tick,
        fixed_interval_us,
        digest,
        tuple(steering_bits),
        tuple(mismatch_counts),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--phase-source", type=Path, required=True)
    parser.add_argument("--direction-source", type=Path, required=True)
    parser.add_argument("--physics-source", type=Path, required=True)
    args = parser.parse_args()
    try:
        loaded = json.loads(args.manifest.read_text(encoding="utf-8"))
        if not isinstance(loaded, dict):
            raise ValueError("manifest root must be an object")
        first, last, interval, digest, bits, mismatches = verify_steering_final_gate(
            args.recording.read_bytes(),
            loaded,
            args.phase_source.read_bytes(),
            args.direction_source.read_bytes(),
            args.physics_source.read_bytes(),
        )
    except (OSError, ValueError, json.JSONDecodeError, struct.error) as error:
        print(f"steering_final_gate_error={error}")
        return 1
    print(
        f"steering_final_gate_supported=1 frames=3 ticks={first}..{last} "
        f"fixed_interval_us={interval} steering_bits={','.join(bits)} "
        f"mismatches={','.join(str(value) for value in mismatches)} "
        f"skip_flags=0x7e delta_writes=3 control_writes=6 "
        f"final_transactions=3 payload_writes=6 sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
