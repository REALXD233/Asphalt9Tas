#!/usr/bin/env python3
"""Strict verifier for a canonical one-frame, all-skip Gate 6 A9UTK1 packet."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

from make_unified_phase_only_gate_v1 import PHASE_ONLY_SKIP_FLAGS
from unified_tick_recording_v1 import decode_recording, plan_tick


def _positive_zero(value: float) -> bool:
    return struct.pack("<f", value) == b"\0\0\0\0"


def verify_phase_only_gate(blob: bytes, manifest: dict[str, object] | None = None) -> tuple[int, int, str]:
    fixed_interval_us, frames = decode_recording(blob)
    if len(frames) != 1:
        raise ValueError("phase-only gate must contain exactly one frame")
    frame = frames[0]
    if frame.skip_flags != PHASE_ONLY_SKIP_FLAGS:
        raise ValueError("phase-only gate must set all eight skip flags")
    dormant_floats = (
        frame.steering,
        frame.brake,
        frame.accelerator,
        *frame.barrel_angular,
        *frame.barrel_rbx,
    )
    if not all(_positive_zero(value) for value in dormant_floats):
        raise ValueError("phase-only gate dormant float is not canonical +0.0")
    if frame.nitro_activations != 0 or frame.respawn:
        raise ValueError("phase-only gate contains an active transient action")
    plan = plan_tick(
        frame,
        current_transform=frame.transform,
        current_linear_velocity=frame.linear_velocity,
    )
    if any(
        value is not None
        for value in (plan.steering, plan.brake, plan.accelerator, plan.barrel_angular, plan.barrel_rbx)
    ) or plan.nitro_activations != 0 or plan.respawn or plan.transform_linear_writes:
        raise ValueError("phase-only planner produced a gameplay action/write")
    digest = hashlib.sha256(blob).hexdigest()
    if manifest is not None:
        required = {
            "schema": "A9UTK1_PHASE_ONLY_GATE_MANIFEST_V1",
            "output_format": "A9UTK1",
            "output_sha256": digest,
            "selected_tick": frame.tick,
            "selected_monotonic_ns": frame.monotonic_ns,
            "fixed_interval_us": fixed_interval_us,
            "skip_flags": PHASE_ONLY_SKIP_FLAGS,
            "expected_processed_frames": 1,
            "expected_control_writes": 0,
            "expected_final_correction": "skipped_zero_write",
            "purpose": "phase_switch_gate_only",
        }
        for key, expected in required.items():
            if manifest.get(key) != expected:
                raise ValueError(f"manifest mismatch for {key}")
        claims = manifest.get("does_not_claim")
        if not isinstance(claims, list) or "transform_equal" not in claims:
            raise ValueError("manifest does_not_claim is incomplete")
    return frame.tick, fixed_interval_us, digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    try:
        blob = args.recording.read_bytes()
        manifest = None
        if args.manifest:
            loaded = json.loads(args.manifest.read_text(encoding="utf-8"))
            if not isinstance(loaded, dict):
                raise ValueError("manifest root must be an object")
            manifest = loaded
        tick, interval, digest = verify_phase_only_gate(blob, manifest)
    except (OSError, ValueError, json.JSONDecodeError, struct.error) as error:
        print(f"phase_only_gate_error={error}")
        return 1
    print(
        f"phase_only_gate_supported=1 frames=1 tick={tick} "
        f"fixed_interval_us={interval} skip_flags=0xff "
        f"gameplay_actions=0 final_writes=0 sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
