#!/usr/bin/env python3
"""Strict verifier for A9UER5 reports with exact steering write audits."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from parse_unified_executor_report_v2 import HEADER_SIZE, STEERING_APPLIED, _HEADER
from parse_unified_executor_report_v4 import (
    UnifiedReportSummaryV4,
    _FRAME as _FRAME_V4,
    decode_report as decode_report_v4,
)


MAGIC = b"A9UER5\0\0"
VERSION = 5
FRAME_SIZE = 1828
_FRAME = struct.Struct("<QQiIqq7Q2QHi2sI4sQQ64s12s804s804s")
assert _FRAME.size == FRAME_SIZE

UnifiedReportSummaryV5 = UnifiedReportSummaryV4


def decode_report(blob: bytes) -> UnifiedReportSummaryV5:
    if len(blob) < HEADER_SIZE:
        raise ValueError("report is shorter than A9UER5 header")
    header = list(_HEADER.unpack_from(blob))
    if header[0] != MAGIC or header[1] != VERSION:
        raise ValueError("unsupported A9UER5 magic/version")
    if header[2] != HEADER_SIZE or header[3] != FRAME_SIZE:
        raise ValueError("A9UER5 ABI size mismatch")
    frame_count = header[5]
    expected_size = HEADER_SIZE + frame_count * FRAME_SIZE
    if frame_count < 1 or len(blob) != expected_size:
        raise ValueError(f"A9UER5 length must be exactly {expected_size} bytes")

    v4_frames: list[bytes] = []
    cursor = HEADER_SIZE
    for index in range(frame_count):
        frame = _FRAME.unpack_from(blob, cursor)
        frame_flags = frame[3]
        steering_bits = frame[18]
        steering_reserved = frame[19]
        c98_pair_after = frame[20]
        c9c_pair_after = frame[21]
        if steering_reserved != bytes(4):
            raise ValueError(f"frame {index}: steering reserved bytes are nonzero")
        if frame_flags & STEERING_APPLIED:
            if (
                c98_pair_after >> 32 != steering_bits
                or c9c_pair_after >> 32 != steering_bits
            ):
                raise ValueError(f"frame {index}: steering write audit mismatch")
        elif c98_pair_after != 0 or c9c_pair_after != 0:
            raise ValueError(f"frame {index}: skipped steering has a write audit")
        v4_frames.append(
            _FRAME_V4.pack(
                *frame[:18],
                *frame[22:],
            )
        )
        cursor += FRAME_SIZE

    header[0] = b"A9UER4\0\0"
    header[1] = 4
    header[3] = _FRAME_V4.size
    synthetic_v4 = _HEADER.pack(*header) + b"".join(v4_frames)
    return decode_report_v4(synthetic_v4)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        summary = decode_report(args.report.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9uer5_error={error}")
        return 1
    print(
        f"a9uer5_supported=1 frames={summary.frames} "
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
