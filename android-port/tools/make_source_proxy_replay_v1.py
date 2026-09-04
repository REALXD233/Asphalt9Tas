#!/usr/bin/env python3
"""Convert a clean strict-pair A9HEV5 trace to an A9SPR1 replay file."""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

from analyze_hwbp_events_v5 import parse_trace, summarize, bits_float


HEADER = struct.Struct("<8sIIII20s20s")
FRAME = struct.Struct("<fffI")
MAGIC = b"A9SPR1\0\0"
BUILD_ID = bytes.fromhex("e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b")
VALID_FLAGS = 0x7


def convert(
    source: pathlib.Path,
    destination: pathlib.Path,
    start_frame: int = 0,
    frame_count: int | None = None,
) -> int:
    header, events = parse_trace(source)
    summary = summarize(header, events)
    integrity_ok = (
        summary["header_clean"]
        and summary["target_signature_verified"]
        and not summary["sequence_errors"]
        and not summary["invalid_reads"]
        and not summary["invalid_floats"]
        and not summary["invalid_flags"]
        and not summary["timestamp_errors"]
        and not summary["header_count_errors"]
        and summary["pairing_confident"]
    )
    if not integrity_ok:
        raise ValueError("source trace is not a clean, complete, strict-pair A9HEV5 trace")

    unpaired = summary["unpaired_events"]
    allowed_boundary_unpaired = {
        index for index in (0, len(events) - 1) if 0 <= index < len(events)
    }
    if any(index not in allowed_boundary_unpaired for index in unpaired) or len(unpaired) > 2:
        raise ValueError(f"source trace has internal unpaired events: {unpaired}")

    pairs = summary["inferred_tick_pairs"]
    if start_frame < 0 or start_frame >= len(pairs):
        raise ValueError(
            f"start frame is outside source trace: {start_frame}/{len(pairs)}"
        )
    if frame_count is not None and frame_count <= 0:
        raise ValueError(f"frame count must be positive: {frame_count}")
    end_frame = len(pairs) if frame_count is None else start_frame + frame_count
    if end_frame > len(pairs):
        raise ValueError(
            f"requested frame range exceeds source trace: "
            f"{start_frame}:{end_frame}/{len(pairs)}"
        )

    frames: list[bytes] = []
    for first, second in pairs[start_frame:end_frame]:
        # Both records are post-write snapshots. The second record contains
        # the complete pair for this tick. Dynamic A/D/S tests prove C9C maps
        # to source value_A (steering) and C98 to source value_B.
        steering = bits_float(second.c9c_bits)
        longitudinal = bits_float(second.c98_bits)
        frames.append(FRAME.pack(steering, longitudinal, 0.0, VALID_FLAGS))

    output_header = HEADER.pack(
        MAGIC,
        1,
        HEADER.size,
        FRAME.size,
        len(frames),
        BUILD_ID,
        bytes(20),
    )
    with destination.open("wb") as output:
        output.write(output_header)
        output.writelines(frames)
    return len(frames)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("destination", type=pathlib.Path)
    parser.add_argument("--start-frame", type=int, default=0)
    parser.add_argument("--frame-count", type=int)
    args = parser.parse_args(argv)
    try:
        frame_count = convert(
            args.source,
            args.destination,
            start_frame=args.start_frame,
            frame_count=args.frame_count,
        )
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    print(f"wrote {frame_count} frames to {args.destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
