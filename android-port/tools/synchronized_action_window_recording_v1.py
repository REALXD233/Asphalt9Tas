#!/usr/bin/env python3
"""Strict A9USR3 verifier for a neutral-anchor steering+brake action window."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from synchronized_brake_recording_v1 import (
    MAGIC as BRAKE_MAGIC,
    REQUIRED_FLAGS as BRAKE_REQUIRED_FLAGS,
    SyncReportV1,
    _HEADER,
    decode_sync_brake_report,
    verify_synchronized_brake_capture,
)
from unified_tick_recording_v1 import decode_recording


MAGIC = b"A9USR3\0\0"
VERSION = 3
REQUIRED_FLAGS = BRAKE_REQUIRED_FLAGS | (1 << 7)


def decode_action_window_report(blob: bytes) -> SyncReportV1:
    if len(blob) < _HEADER.size:
        raise ValueError("report is shorter than the A9USR3 header")
    header = list(_HEADER.unpack_from(blob))
    if header[0] != MAGIC or header[1] != VERSION:
        raise ValueError("unsupported A9USR3 magic/version")
    if header[4] != REQUIRED_FLAGS:
        raise ValueError(f"A9USR3 success flags must be 0x{REQUIRED_FLAGS:x}")
    synthetic = bytearray(blob)
    synthetic[0:8] = BRAKE_MAGIC
    struct.pack_into("<I", synthetic, 8, 2)
    struct.pack_into("<I", synthetic, 20, BRAKE_REQUIRED_FLAGS)
    return decode_sync_brake_report(bytes(synthetic))


def verify_action_window_capture(report_blob: bytes, recording_blob: bytes) -> SyncReportV1:
    # Reuse every A9USR2 bit-for-bit control and physics binding check after
    # substituting its semantic envelope.
    synthetic = bytearray(report_blob)
    report = decode_action_window_report(report_blob)
    synthetic[0:8] = BRAKE_MAGIC
    struct.pack_into("<I", synthetic, 8, 2)
    struct.pack_into("<I", synthetic, 20, BRAKE_REQUIRED_FLAGS)
    verify_synchronized_brake_capture(bytes(synthetic), recording_blob)
    _, frames = decode_recording(recording_blob)

    if frames[0].steering != 0.0 or frames[0].brake != 0.0:
        raise ValueError("action window frame 0 must preserve a neutral lead-in")
    active = [
        index
        for index, frame in enumerate(frames)
        if frame.brake <= -0.5 and abs(frame.steering) >= 0.1
    ]
    if not active:
        raise ValueError("action window lacks simultaneous steering+brake")
    first_active = active[0]
    if not any(frame.brake == 0.0 for frame in frames[first_active + 1 :]):
        raise ValueError("action window lacks brake release after activation")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("recording", type=Path)
    args = parser.parse_args()
    try:
        report = verify_action_window_capture(
            args.report.read_bytes(), args.recording.read_bytes()
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"a9usr3_error={error}")
        return 1
    print(
        f"a9usr3_supported=1 frames={report.captured_frames} "
        f"ticks=0..{report.captured_frames - 1} events={report.event_count} "
        f"delta_writes={report.delta_writes} "
        f"threads={report.initial_threads}/{report.final_threads}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
