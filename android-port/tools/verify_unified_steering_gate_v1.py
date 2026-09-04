#!/usr/bin/env python3
"""Strict verifier for the one-frame steering-only Gate 7 A9UTK1 packet."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from make_unified_steering_gate_v1 import STEERING_ONLY_SKIP_FLAGS
from unified_tick_recording_v1 import decode_recording, plan_tick


def _positive_zero(value: float) -> bool:
    return struct.pack("<f", value) == bytes(4)


def verify_steering_gate(
    blob: bytes, manifest: dict[str, object] | None = None
) -> tuple[int, int, str, str]:
    fixed_interval_us, frames = decode_recording(blob)
    if len(frames) != 1:
        raise ValueError("steering gate must contain exactly one frame")
    frame = frames[0]
    if frame.skip_flags != STEERING_ONLY_SKIP_FLAGS:
        raise ValueError("steering gate must enable steering and skip all other overrides")
    steering_bits = struct.pack("<f", frame.steering).hex()
    if not 0.25 <= abs(frame.steering) <= 0.75:
        raise ValueError("steering magnitude must be in 0.25..0.75")
    dormant_floats = (
        frame.brake,
        frame.accelerator,
        *frame.barrel_angular,
        *frame.barrel_rbx,
    )
    if not all(_positive_zero(value) for value in dormant_floats):
        raise ValueError("steering gate dormant float is not canonical +0.0")
    if frame.nitro_activations != 0 or frame.respawn:
        raise ValueError("steering gate contains an active transient action")
    plan = plan_tick(
        frame,
        current_transform=frame.transform,
        current_linear_velocity=frame.linear_velocity,
    )
    if (
        plan.steering != frame.steering
        or any(value is not None for value in (plan.brake, plan.accelerator, plan.barrel_angular, plan.barrel_rbx))
        or plan.nitro_activations != 0
        or plan.respawn
        or plan.transform_linear_writes
    ):
        raise ValueError("steering gate planner scope is not steering-only")
    digest = hashlib.sha256(blob).hexdigest()
    if manifest is not None:
        required = {
            "schema": "A9UTK1_STEERING_ONLY_GATE_MANIFEST_V1",
            "output_format": "A9UTK1",
            "output_sha256": digest,
            "steering_bits_hex": steering_bits,
            "selected_tick": frame.tick,
            "selected_monotonic_ns": frame.monotonic_ns,
            "fixed_interval_us": fixed_interval_us,
            "skip_flags": STEERING_ONLY_SKIP_FLAGS,
            "expected_processed_frames": 1,
            "expected_control_writes": 2,
            "expected_final_correction": "skipped_zero_write",
            "purpose": "unified_steering_integration_gate_only",
        }
        for key, expected in required.items():
            if manifest.get(key) != expected:
                raise ValueError(f"manifest mismatch for {key}")
        claims = manifest.get("does_not_claim")
        if not isinstance(claims, list) or "trajectory_determinism" not in claims:
            raise ValueError("manifest does_not_claim is incomplete")
    return frame.tick, fixed_interval_us, digest, steering_bits


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    try:
        manifest = None
        if args.manifest:
            loaded = json.loads(args.manifest.read_text(encoding="utf-8"))
            if not isinstance(loaded, dict):
                raise ValueError("manifest root must be an object")
            manifest = loaded
        tick, interval, digest, steering_bits = verify_steering_gate(
            args.recording.read_bytes(), manifest
        )
    except (OSError, ValueError, json.JSONDecodeError, struct.error) as error:
        print(f"steering_gate_error={error}")
        return 1
    print(
        f"steering_gate_supported=1 frames=1 tick={tick} "
        f"fixed_interval_us={interval} steering_bits={steering_bits} "
        f"skip_flags=0xfe control_writes=2 final_writes=0 sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
