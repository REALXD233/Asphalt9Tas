#!/usr/bin/env python3
"""Strict verifier for A9UER3 reports with a deferred callback clear."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from parse_unified_executor_report_v2 import (
    HEADER_SIZE,
    UnifiedReportSummaryV2,
    _FRAME as _FRAME_V2,
    _HEADER,
    decode_report as decode_report_v2,
)


MAGIC = b"A9UER3\0\0"
VERSION = 3
FRAME_SIZE = 1804
_FRAME = struct.Struct("<QQiIqq7Q2QH6s64s12s804s804s")
assert _FRAME.size == FRAME_SIZE

UnifiedReportSummaryV3 = UnifiedReportSummaryV2


def decode_report(blob: bytes) -> UnifiedReportSummaryV3:
    if len(blob) < HEADER_SIZE:
        raise ValueError("report is shorter than A9UER3 header")
    header = list(_HEADER.unpack_from(blob))
    if header[0] != MAGIC or header[1] != VERSION:
        raise ValueError("unsupported A9UER3 magic/version")
    if header[2] != HEADER_SIZE or header[3] != FRAME_SIZE:
        raise ValueError("A9UER3 ABI size mismatch")
    frame_count = header[5]
    expected_size = HEADER_SIZE + frame_count * FRAME_SIZE
    if frame_count < 1 or len(blob) != expected_size:
        raise ValueError(f"A9UER3 length must be exactly {expected_size} bytes")

    v2_frames: list[bytes] = []
    cursor = HEADER_SIZE
    for index in range(frame_count):
        frame = _FRAME.unpack_from(blob, cursor)
        callback_close_event = frame[10]
        deferred_clear_event = frame[11]
        world_commit_event = frame[12]
        if not callback_close_event < deferred_clear_event < world_commit_event:
            raise ValueError(
                f"frame {index}: deferred callback clear event order mismatch"
            )
        v2_frames.append(
            _FRAME_V2.pack(
                *frame[:11],
                world_commit_event,
                *frame[13:],
            )
        )
        cursor += FRAME_SIZE

    # A9UER3 is A9UER2 plus one mandatory per-frame phase event. Reuse the
    # complete A9UER2 semantic/audit verifier after removing that field.
    header[0] = b"A9UER2\0\0"
    header[1] = 2
    header[3] = _FRAME_V2.size
    synthetic_v2 = _HEADER.pack(*header) + b"".join(v2_frames)
    return decode_report_v2(synthetic_v2)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        summary = decode_report(args.report.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9uer3_error={error}")
        return 1
    print(
        f"a9uer3_supported=1 frames={summary.frames} "
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
