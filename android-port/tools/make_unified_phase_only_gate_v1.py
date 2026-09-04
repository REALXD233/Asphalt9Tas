#!/usr/bin/env python3
"""Create one canonical all-skip A9UTK1 frame for the Gate 6 phase-only test."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path

from native_physics_recording_v1 import decode_recording as decode_a9nps1
from native_physics_recording_v1 import validate_runtime_safe
from unified_tick_recording_v1 import (
    SKIP_ACCELERATOR,
    SKIP_BARREL_ANGULAR,
    SKIP_BARREL_RBX,
    SKIP_BRAKE,
    SKIP_NITRO,
    SKIP_RESPAWN,
    SKIP_STEER,
    SKIP_TRANSFORM,
    UnifiedTickFrameV1,
    encode_recording,
)


PHASE_ONLY_SKIP_FLAGS = (
    SKIP_STEER
    | SKIP_BRAKE
    | SKIP_NITRO
    | SKIP_ACCELERATOR
    | SKIP_BARREL_ANGULAR
    | SKIP_BARREL_RBX
    | SKIP_RESPAWN
    | SKIP_TRANSFORM
)
assert PHASE_ONLY_SKIP_FLAGS == 0xFF


def build_phase_only_gate(source_blob: bytes, *, frame_index: int, fixed_interval_us: int) -> tuple[bytes, dict[str, object]]:
    source_frames = decode_a9nps1(source_blob)
    validate_runtime_safe(source_frames)
    if not 0 <= frame_index < len(source_frames):
        raise ValueError(f"frame_index must be in 0..{len(source_frames) - 1}")
    source = source_frames[frame_index]
    gate_frame = UnifiedTickFrameV1(
        tick=source.tick,
        monotonic_ns=source.monotonic_ns,
        steering=0.0,
        brake=0.0,
        accelerator=0.0,
        nitro_activations=0,
        skip_flags=PHASE_ONLY_SKIP_FLAGS,
        respawn=False,
        barrel_angular=(0.0, 0.0, 0.0),
        barrel_rbx=(0.0, 0.0),
        transform=source.transform,
        linear_velocity=source.linear_velocity,
    )
    output_blob = encode_recording([gate_frame], fixed_interval_us=fixed_interval_us)
    manifest: dict[str, object] = {
        "schema": "A9UTK1_PHASE_ONLY_GATE_MANIFEST_V1",
        "source_format": "A9NPS1",
        "output_format": "A9UTK1",
        "source_sha256": hashlib.sha256(source_blob).hexdigest(),
        "output_sha256": hashlib.sha256(output_blob).hexdigest(),
        "source_frame_count": len(source_frames),
        "selected_frame_index": frame_index,
        "selected_tick": source.tick,
        "selected_monotonic_ns": source.monotonic_ns,
        "fixed_interval_us": fixed_interval_us,
        "skip_flags": PHASE_ONLY_SKIP_FLAGS,
        "expected_processed_frames": 1,
        "expected_control_writes": 0,
        "expected_final_correction": "skipped_zero_write",
        "purpose": "phase_switch_gate_only",
        "does_not_claim": [
            "transform_equal",
            "different_value_correction",
            "steering_replay",
            "brake_or_accelerator_replay",
            "nitro_or_respawn_replay",
            "barrel_replay",
        ],
    }
    return output_blob, manifest


def write_new_gate(output_path: Path, manifest_path: Path, output_blob: bytes, manifest: dict[str, object]) -> None:
    if output_path.resolve() == manifest_path.resolve():
        raise ValueError("output and manifest paths must differ")
    if output_path.exists() or manifest_path.exists():
        raise FileExistsError("output/manifest already exists; refusing overwrite")
    created_output = False
    try:
        with output_path.open("xb") as file:
            file.write(output_blob)
            file.flush()
            os.fsync(file.fileno())
        created_output = True
        with manifest_path.open("x", encoding="utf-8", newline="\n") as file:
            json.dump(manifest, file, ensure_ascii=False, indent=2, sort_keys=True)
            file.write("\n")
            file.flush()
            os.fsync(file.fileno())
    except Exception:
        if created_output:
            output_path.unlink(missing_ok=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--frame-index", type=int, default=0)
    parser.add_argument("--fixed-interval-us", type=int, default=16667)
    args = parser.parse_args()
    try:
        source_blob = args.source.read_bytes()
        output_blob, manifest = build_phase_only_gate(
            source_blob,
            frame_index=args.frame_index,
            fixed_interval_us=args.fixed_interval_us,
        )
        manifest["source_name"] = args.source.name
        manifest["output_name"] = args.output.name
        write_new_gate(args.output, args.manifest, output_blob, manifest)
    except (OSError, ValueError) as error:
        print(f"phase_only_gate_error={error}")
        return 1
    print(
        f"phase_only_gate_created=1 tick={manifest['selected_tick']} "
        f"fixed_interval_us={manifest['fixed_interval_us']} "
        f"skip_flags=0x{manifest['skip_flags']:02x} "
        f"sha256={manifest['output_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
