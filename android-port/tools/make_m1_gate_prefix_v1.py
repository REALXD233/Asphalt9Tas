#!/usr/bin/env python3
"""Create an exact leading A9UTK1 prefix and its SHA-bound A9FWT1 target."""

from __future__ import annotations

import argparse
import pathlib

from make_final_writer_target_blob_v1 import (
    decode_target_blob,
    encode_target_blob,
)
from unified_tick_recording_v1 import decode_recording, encode_recording


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("frames", type=int)
    parser.add_argument("recording_output", type=pathlib.Path)
    parser.add_argument("target_output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        if not 2 <= args.frames <= 30:
            raise ValueError("M1 gate prefix must contain 2..30 frames")
        if args.recording_output.exists() or args.target_output.exists():
            raise ValueError("refusing to overwrite an output")
        interval, frames = decode_recording(args.source.read_bytes())
        if len(frames) < args.frames or frames[0].tick != 0:
            raise ValueError("source is not a start-line recording")
        prefix = encode_recording(frames[: args.frames],
                                  fixed_interval_us=interval)
        target = encode_target_blob(prefix)
        decoded_target = decode_target_blob(target, expected_recording=prefix)
        if decoded_target["frame_count"] != args.frames:
            raise ValueError("target frame count mismatch")
        args.recording_output.parent.mkdir(parents=True, exist_ok=True)
        args.target_output.parent.mkdir(parents=True, exist_ok=True)
        args.recording_output.write_bytes(prefix)
        args.target_output.write_bytes(target)
    except (OSError, ValueError) as error:
        for output in (args.recording_output, args.target_output):
            try:
                output.unlink(missing_ok=True)
            except OSError:
                pass
        print(f"m1_gate_prefix_error={error}")
        return 1
    print(
        f"M1_GATE_PREFIX passed=1 frames={args.frames} "
        f"fixed_interval_us={interval} first_tick=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
