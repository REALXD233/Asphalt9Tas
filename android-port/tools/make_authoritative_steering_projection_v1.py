#!/usr/bin/env python3
"""Project a bound A9UTK1 source to steering-only without changing its ticks.

The projection preserves every tick/time, steering raw bit, transform byte,
linear-velocity byte and frame ABI field. It clears non-steering payloads and
sets every skip bit except steering. A new A9NPA1 is then hash-bound to the
projected recording and the original live source report.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import struct
import sys

from natural_preroll_anchor_v1 import bind_anchor, decode_anchor
from unified_tick_recording_v1 import (
    FRAME_SIZE,
    HEADER_SIZE,
    SKIP_STEER,
    SUPPORTED_SKIP_MASK,
    decode_recording,
)


PROJECTED_SKIP_MASK = SUPPORTED_SKIP_MASK & ~SKIP_STEER
STEERING_OFFSET = 16
NON_STEERING_BEGIN = 20
NON_STEERING_END = 60
PHYSICS_BEGIN = 60
PHYSICS_END = 136


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_exclusive(path: pathlib.Path, data: bytes) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
    except BaseException:
        path.unlink(missing_ok=True)
        raise


def project(source: bytes) -> tuple[bytes, dict[str, object]]:
    fixed_interval_us, source_frames = decode_recording(source)
    projected = bytearray(source)
    steering_bits: list[str] = []
    nonzero_indices: list[int] = []
    for index, frame in enumerate(source_frames):
        base = HEADER_SIZE + index * FRAME_SIZE
        raw_steering = bytes(source[base + STEERING_OFFSET:base + STEERING_OFFSET + 4])
        steering_bits.append(raw_steering.hex())
        if raw_steering != bytes(4):
            nonzero_indices.append(index)
        # Clear brake, accelerator, Nitro count, respawn/padding, barrel
        # angular and RBX. Then set all skip bits except steering.
        projected[base + NON_STEERING_BEGIN:base + NON_STEERING_END] = bytes(
            NON_STEERING_END - NON_STEERING_BEGIN
        )
        struct.pack_into("<I", projected, base + 32, PROJECTED_SKIP_MASK)

    projected_bytes = bytes(projected)
    projected_fixed, projected_frames = decode_recording(projected_bytes)
    if projected_fixed != fixed_interval_us or len(projected_frames) != len(source_frames):
        raise ValueError("projection changed recording shape")
    changed_offsets: set[int] = set()
    for index, (before, after) in enumerate(zip(source, projected_bytes)):
        if before == after or index < HEADER_SIZE:
            continue
        changed_offsets.add((index - HEADER_SIZE) % FRAME_SIZE)
    allowed = set(range(NON_STEERING_BEGIN, NON_STEERING_END))
    if not changed_offsets <= allowed:
        raise ValueError(f"projection changed forbidden frame offsets: {changed_offsets - allowed}")
    for index, frame in enumerate(projected_frames):
        base = HEADER_SIZE + index * FRAME_SIZE
        if frame.tick != source_frames[index].tick or frame.monotonic_ns != source_frames[index].monotonic_ns:
            raise ValueError(f"frame {index}: tick/time changed")
        if bytes(projected_bytes[base + STEERING_OFFSET:base + STEERING_OFFSET + 4]) != bytes(
            source[base + STEERING_OFFSET:base + STEERING_OFFSET + 4]
        ):
            raise ValueError(f"frame {index}: steering bits changed")
        if projected_bytes[base + PHYSICS_BEGIN:base + PHYSICS_END] != source[
            base + PHYSICS_BEGIN:base + PHYSICS_END
        ]:
            raise ValueError(f"frame {index}: physics bytes changed")
        if frame.skip_flags != PROJECTED_SKIP_MASK:
            raise ValueError(f"frame {index}: skip mask mismatch")
    if not nonzero_indices:
        raise ValueError("source has no nonzero steering sample")
    return projected_bytes, {
        "frame_count": len(projected_frames),
        "fixed_interval_us": fixed_interval_us,
        "projected_skip_mask": PROJECTED_SKIP_MASK,
        "nonzero_steering_frames": len(nonzero_indices),
        "first_nonzero_steering_frame": nonzero_indices[0],
        "last_nonzero_steering_frame": nonzero_indices[-1],
        "steering_bits_sha256": sha256(b"".join(bytes.fromhex(value) for value in steering_bits)),
        "changed_frame_offsets": sorted(changed_offsets),
        "preserved_frame_offsets": [
            "0..19 tick,time,steering",
            "60..135 transform+linear",
            "136..143 frame flags+reserved",
        ],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_recording", type=pathlib.Path)
    parser.add_argument("source_report", type=pathlib.Path)
    parser.add_argument("raw_anchor", type=pathlib.Path)
    parser.add_argument("output_recording", type=pathlib.Path)
    parser.add_argument("output_anchor", type=pathlib.Path)
    parser.add_argument("output_manifest", type=pathlib.Path)
    args = parser.parse_args(argv)
    outputs = (args.output_recording, args.output_anchor, args.output_manifest)
    if any(path.exists() for path in outputs):
        raise ValueError("an output already exists; refusing to overwrite")
    source = args.source_recording.read_bytes()
    projected, details = project(source)
    write_exclusive(args.output_recording, projected)
    try:
        anchor = bind_anchor(
            args.raw_anchor,
            args.source_report,
            args.output_recording,
            args.output_anchor,
        )
        # The projected frame-0 physics must still be the certified source
        # frame-0 physics, otherwise re-binding would be misleading.
        decoded = decode_anchor(args.output_anchor.read_bytes())
        if decoded.frame0_transform != anchor.frame0_transform or decoded.frame0_linear != anchor.frame0_linear:
            raise ValueError("projected anchor frame-0 binding mismatch")
        manifest = {
            "format": "A9_STEERING_PROJECTION_V1",
            "source_recording": str(args.source_recording),
            "source_recording_sha256": sha256(source),
            "source_report": str(args.source_report),
            "source_report_sha256": sha256(args.source_report.read_bytes()),
            "raw_anchor": str(args.raw_anchor),
            "raw_anchor_sha256": sha256(args.raw_anchor.read_bytes()),
            "output_recording": str(args.output_recording),
            "output_recording_sha256": sha256(projected),
            "output_anchor": str(args.output_anchor),
            "output_anchor_sha256": sha256(args.output_anchor.read_bytes()),
            **details,
        }
        write_exclusive(
            args.output_manifest,
            (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8"),
        )
    except BaseException:
        for path in outputs:
            path.unlink(missing_ok=True)
        raise
    print(
        "AUTHORITATIVE_STEERING_PROJECTION passed=1 "
        f"frames={details['frame_count']} "
        f"nonzero={details['nonzero_steering_frames']} "
        f"first={details['first_nonzero_steering_frame']} "
        f"last={details['last_nonzero_steering_frame']} "
        f"skip=0x{PROJECTED_SKIP_MASK:x}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"AUTHORITATIVE_STEERING_PROJECTION passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
