#!/usr/bin/env python3
"""Create a strict A9UTK1/A9FWT1 prefix pair for short final-writer gates."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import sys

from make_final_writer_target_blob_v1 import encode_target_blob
from unified_tick_recording_v1 import decode_recording, encode_recording


ALLOWED_GATE_LENGTHS = (30, 360, 900)


def make_prefix(recording: bytes, frame_count: int) -> tuple[bytes, bytes]:
    fixed_interval_us, frames = decode_recording(recording)
    if frame_count not in ALLOWED_GATE_LENGTHS:
        raise ValueError(f"frame count must be one of {ALLOWED_GATE_LENGTHS}")
    if frame_count > len(frames):
        raise ValueError("requested prefix exceeds source recording")
    prefix = encode_recording(frames[:frame_count],
                              fixed_interval_us=fixed_interval_us)
    target = encode_target_blob(prefix)
    return prefix, target


def _write_exclusive(path: pathlib.Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as output:
        output.write(data)
        output.flush()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("frame_count", type=int, choices=ALLOWED_GATE_LENGTHS)
    parser.add_argument("recording_output", type=pathlib.Path)
    parser.add_argument("target_output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        if args.recording_output.resolve() == args.target_output.resolve():
            raise ValueError("recording and target outputs must differ")
        prefix, target = make_prefix(args.source.read_bytes(), args.frame_count)
        _write_exclusive(args.recording_output, prefix)
        try:
            _write_exclusive(args.target_output, target)
        except Exception:
            args.recording_output.unlink(missing_ok=True)
            raise
    except (OSError, ValueError) as error:
        print(f"final_writer_prefix_error={error}", file=sys.stderr)
        return 1
    print(
        f"FINAL_WRITER_PREFIX passed=1 frames={args.frame_count} "
        f"recording_sha256={hashlib.sha256(prefix).hexdigest()} "
        f"target_sha256={hashlib.sha256(target).hexdigest()} "
        "outputs=exclusive device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
