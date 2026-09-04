#!/usr/bin/env python3
"""Create a hash-bound composite replay Gate without selecting a Nitro colour."""

from __future__ import annotations

import argparse
import dataclasses
import hashlib
import pathlib

from make_final_writer_target_blob_v1 import encode_target_blob
from unified_tick_recording_v1 import (
    SKIP_NITRO,
    UnifiedTickFrameV1,
    decode_recording,
    encode_recording,
)


SOURCE_SHA256 = "a871e41482c5319918f70f39cddd85ccba8625f1e82181db07043ec55a8232c0"
SEQUENCE = (0, 1, 0, 2, 0)
DEFAULT_START_FRAME = 240


def build(recording: bytes, start_frame: int = DEFAULT_START_FRAME) -> tuple[bytes, bytes]:
    if hashlib.sha256(recording).hexdigest() != SOURCE_SHA256:
        raise ValueError("source is not the reviewed Ancient Ruins + ZL1 360-frame Gate")
    interval, frames = decode_recording(recording)
    if len(frames) != 360 or interval != 16667:
        raise ValueError("source must be the reviewed 360-frame fixed-delta Gate")
    if start_frame < 0 or start_frame + len(SEQUENCE) > len(frames):
        raise ValueError("natural-action sequence is outside the recording")
    output: list[UnifiedTickFrameV1] = []
    for index, frame in enumerate(frames):
        count = SEQUENCE[index - start_frame] if (
            start_frame <= index < start_frame + len(SEQUENCE)
        ) else 0
        output.append(dataclasses.replace(
            frame,
            nitro_activations=count,
            # Every frame is authoritative for this field. Zero means no call;
            # it is not an unsupported/skip placeholder.
            skip_flags=frame.skip_flags & ~SKIP_NITRO,
        ))
    encoded = encode_recording(output, fixed_interval_us=interval)
    return encoded, encode_target_blob(encoded)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("recording_out", type=pathlib.Path)
    parser.add_argument("target_out", type=pathlib.Path)
    parser.add_argument("--start-frame", type=int, default=DEFAULT_START_FRAME)
    args = parser.parse_args()
    try:
        recording, target = build(args.source.read_bytes(), args.start_frame)
        for path in (args.recording_out, args.target_out):
            if path.exists():
                raise ValueError(f"refusing to overwrite: {path}")
        args.recording_out.write_bytes(recording)
        args.target_out.write_bytes(target)
    except (OSError, ValueError) as error:
        print(f"composite_gate_error={error}")
        return 1
    print(
        "FINAL_WRITER_NATURAL_ACTION_GATE_CREATED "
        f"frames=360 sequence_start={args.start_frame} sequence=0_1_0_2_0 "
        f"recording_sha256={hashlib.sha256(recording).hexdigest()} "
        f"target_sha256={hashlib.sha256(target).hexdigest()} "
        "nitro_colour_forced=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
