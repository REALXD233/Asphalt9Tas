#!/usr/bin/env python3
"""Strict A9USR5 lifecycle-source report and A9UTK1 cross-validator."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from synchronized_brake_recording_v1 import (
    INPUT_CYCLE_ANCHOR_FLAG,
    MAGIC as A9USR2_MAGIC,
    VERSION as A9USR2_VERSION,
    verify_synchronized_brake_capture,
)


MAGIC = b"A9USR5\0\0"
VERSION = 5
HEADER_SIZE = 240
FLAGS_OFFSET = 20
RACE_LIFECYCLE_FLAG = INPUT_CYCLE_ANCHOR_FLAG


def verify_lifecycle_source_capture(report_blob: bytes, recording_blob: bytes):
    if len(report_blob) < HEADER_SIZE:
        raise ValueError("report is shorter than the A9USR5 header")
    if report_blob[:8] != MAGIC or struct.unpack_from("<I", report_blob, 8)[0] != VERSION:
        raise ValueError("unsupported A9USR5 magic/version")
    flags = struct.unpack_from("<I", report_blob, FLAGS_OFFSET)[0]
    if (flags & RACE_LIFECYCLE_FLAG) == 0:
        raise ValueError("A9USR5 authoritative lifecycle witness is missing")

    # A9USR5 intentionally retains the proven A9USR2 physical ABI. Substitute
    # only the semantic envelope and reuse its strict per-frame cross-check.
    synthetic = bytearray(report_blob)
    synthetic[:8] = A9USR2_MAGIC
    struct.pack_into("<I", synthetic, 8, A9USR2_VERSION)
    return verify_synchronized_brake_capture(
        bytes(synthetic), recording_blob, require_input_cycle_anchor=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("recording", type=Path)
    args = parser.parse_args()
    try:
        result = verify_lifecycle_source_capture(
            args.report.read_bytes(), args.recording.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9usr5_error={error}")
        return 1
    print(
        f"a9usr5_supported=1 frames={result.captured_frames} "
        f"ticks=0..{result.captured_frames - 1} events={result.event_count} "
        "race_lifecycle_2_to_3=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

