#!/usr/bin/env python3
"""Project A9USR2/A9USR4 A9UTK1 to controls-only start-line replay.

Raw steering and brake bits, timing, physics bytes and every ABI field are
preserved. The sole semantic change is setting SkipTransformForced, because
this stage replays controls but does not claim physical correction.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import struct
import sys

from unified_tick_recording_v1 import (
    FRAME_SIZE,
    HEADER_SIZE,
    SKIP_ACCELERATOR,
    SKIP_BARREL_ANGULAR,
    SKIP_BARREL_RBX,
    SKIP_NITRO,
    SKIP_RESPAWN,
    SKIP_TRANSFORM,
    decode_recording,
)

SOURCE_SKIP_MASK = (
    SKIP_NITRO | SKIP_ACCELERATOR | SKIP_BARREL_ANGULAR |
    SKIP_BARREL_RBX | SKIP_RESPAWN
)
PROJECTED_SKIP_MASK = SOURCE_SKIP_MASK | SKIP_TRANSFORM
SKIP_OFFSET = 32
STEERING_BRAKE_BEGIN = 16
STEERING_BRAKE_END = 24


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def write_exclusive(path: pathlib.Path, blob: bytes) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(blob)
            stream.flush()
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def project(source: bytes) -> tuple[bytes, dict[str, object]]:
    fixed, frames = decode_recording(source)
    projected = bytearray(source)
    control_bits = bytearray()
    nonzero_steering = 0
    nonzero_brake = 0
    for index, frame in enumerate(frames):
        if frame.skip_flags != SOURCE_SKIP_MASK:
            raise ValueError(f"frame {index}: source skip mask must be 0x{SOURCE_SKIP_MASK:x}")
        base = HEADER_SIZE + index * FRAME_SIZE
        raw_controls = source[base + STEERING_BRAKE_BEGIN:base + STEERING_BRAKE_END]
        control_bits += raw_controls
        nonzero_steering += raw_controls[:4] != bytes(4)
        nonzero_brake += raw_controls[4:] != bytes(4)
        struct.pack_into("<I", projected, base + SKIP_OFFSET, PROJECTED_SKIP_MASK)

    output = bytes(projected)
    projected_fixed, projected_frames = decode_recording(output)
    if projected_fixed != fixed or len(projected_frames) != len(frames):
        raise ValueError("projection changed recording shape")
    changed_offsets = {
        (index - HEADER_SIZE) % FRAME_SIZE
        for index, (before, after) in enumerate(zip(source, output))
        if index >= HEADER_SIZE and before != after
    }
    if not changed_offsets <= set(range(SKIP_OFFSET, SKIP_OFFSET + 4)):
        raise ValueError(f"projection changed forbidden offsets: {changed_offsets}")
    for index, frame in enumerate(projected_frames):
        base = HEADER_SIZE + index * FRAME_SIZE
        if frame.skip_flags != PROJECTED_SKIP_MASK:
            raise ValueError(f"frame {index}: projected skip mask")
        if output[base + STEERING_BRAKE_BEGIN:base + STEERING_BRAKE_END] != source[
            base + STEERING_BRAKE_BEGIN:base + STEERING_BRAKE_END
        ]:
            raise ValueError(f"frame {index}: control bits changed")
        if output[base:base + FRAME_SIZE] != (
            source[base:base + SKIP_OFFSET] +
            output[base + SKIP_OFFSET:base + SKIP_OFFSET + 4] +
            source[base + SKIP_OFFSET + 4:base + FRAME_SIZE]
        ):
            raise ValueError(f"frame {index}: non-skip bytes changed")
    return output, {
        "format": "A9_CONTROLS_PROJECTION_V1",
        "frame_count": len(frames),
        "fixed_interval_us": fixed,
        "source_skip_mask": SOURCE_SKIP_MASK,
        "projected_skip_mask": PROJECTED_SKIP_MASK,
        "nonzero_steering_frames": nonzero_steering,
        "nonzero_brake_frames": nonzero_brake,
        "control_bits_sha256": sha256(bytes(control_bits)),
        "changed_frame_offsets": sorted(changed_offsets),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("manifest", type=pathlib.Path)
    args = parser.parse_args()
    if args.output.exists() or args.manifest.exists():
        raise ValueError("an output exists; refusing to overwrite")
    source = args.source.read_bytes()
    output, details = project(source)
    write_exclusive(args.output, output)
    try:
        manifest = {
            **details,
            "source": str(args.source),
            "source_sha256": sha256(source),
            "output": str(args.output),
            "output_sha256": sha256(output),
        }
        write_exclusive(
            args.manifest,
            (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode(),
        )
    except BaseException:
        args.output.unlink(missing_ok=True)
        args.manifest.unlink(missing_ok=True)
        raise
    print(
        "AUTHORITATIVE_CONTROLS_PROJECTION passed=1 "
        f"frames={details['frame_count']} steering={details['nonzero_steering_frames']} "
        f"brake={details['nonzero_brake_frames']} skip=0x{PROJECTED_SKIP_MASK:x}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error) as error:
        print(f"AUTHORITATIVE_CONTROLS_PROJECTION passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
