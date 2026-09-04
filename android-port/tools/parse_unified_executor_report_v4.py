#!/usr/bin/env python3
"""Strict verifier for A9UER4 reports with split physics/commit owners."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from parse_unified_executor_report_v2 import HEADER_SIZE, _HEADER
from parse_unified_executor_report_v3 import (
    UnifiedReportSummaryV3,
    _FRAME as _FRAME_V3,
    decode_report as decode_report_v3,
)


MAGIC = b"A9UER4\0\0"
VERSION = 4
FRAME_SIZE = 1804
_FRAME = struct.Struct("<QQiIqq7Q2QHi2s64s12s804s804s")
assert _FRAME.size == FRAME_SIZE

UnifiedReportSummaryV4 = UnifiedReportSummaryV3


def decode_report(blob: bytes) -> UnifiedReportSummaryV4:
    if len(blob) < HEADER_SIZE:
        raise ValueError("report is shorter than A9UER4 header")
    header = list(_HEADER.unpack_from(blob))
    if header[0] != MAGIC or header[1] != VERSION:
        raise ValueError("unsupported A9UER4 magic/version")
    if header[2] != HEADER_SIZE or header[3] != FRAME_SIZE:
        raise ValueError("A9UER4 ABI size mismatch")
    frame_count = header[5]
    expected_size = HEADER_SIZE + frame_count * FRAME_SIZE
    if frame_count < 1 or len(blob) != expected_size:
        raise ValueError(f"A9UER4 length must be exactly {expected_size} bytes")

    v3_frames: list[bytes] = []
    cursor = HEADER_SIZE
    for index in range(frame_count):
        frame = _FRAME.unpack_from(blob, cursor)
        cycle_tid = frame[2]
        commit_tid = frame[16]
        reserved = frame[17]
        if commit_tid <= 0 or commit_tid == cycle_tid:
            raise ValueError(f"frame {index}: invalid independent commit tid")
        if reserved != bytes(2):
            raise ValueError(f"frame {index}: reserved bytes are nonzero")
        v3_frames.append(
            _FRAME_V3.pack(
                *frame[:16],
                bytes(6),
                *frame[18:],
            )
        )
        cursor += FRAME_SIZE

    header[0] = b"A9UER3\0\0"
    header[1] = 3
    synthetic_v3 = _HEADER.pack(*header) + b"".join(v3_frames)
    return decode_report_v3(synthetic_v3)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        summary = decode_report(args.report.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9uer4_error={error}")
        return 1
    print(
        f"a9uer4_supported=1 frames={summary.frames} "
        f"ticks={summary.first_tick}..{summary.last_tick} "
        f"fixed_interval_us={summary.fixed_interval_us} "
        f"equal={summary.equal_frames} corrected={summary.corrected_frames} "
        f"skipped={summary.skipped_frames} delta_writes={summary.delta_writes} "
        f"control_writes={summary.control_writes} "
        f"threads={summary.initial_threads}/{summary.final_threads}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
