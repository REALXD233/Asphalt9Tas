#!/usr/bin/env python3
"""Strict A9UER6 verifier for brake/negative-longitudinal pair writes."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from parse_unified_executor_report_v2 import HEADER_SIZE, STEERING_APPLIED, _HEADER
from parse_unified_executor_report_v5 import FRAME_SIZE, _FRAME
from parse_unified_executor_report_v4 import _FRAME as _FRAME_V4, decode_report as decode_report_v4


MAGIC = b"A9UER6\0\0"
VERSION = 6
BRAKE_APPLIED = 1 << 8
BRAKE_APPLIED_NATURAL = 1 << 9


def decode_report(blob: bytes):
    if len(blob) < HEADER_SIZE:
        raise ValueError("report is shorter than A9UER6 header")
    header = list(_HEADER.unpack_from(blob))
    if header[0] != MAGIC or header[1] != VERSION:
        raise ValueError("unsupported A9UER6 magic/version")
    if header[2] != HEADER_SIZE or header[3] != FRAME_SIZE:
        raise ValueError("A9UER6 ABI size mismatch")
    frame_count = header[5]
    expected_size = HEADER_SIZE + frame_count * FRAME_SIZE
    if frame_count < 1 or len(blob) != expected_size:
        raise ValueError(f"A9UER6 length must be exactly {expected_size} bytes")

    v4_frames = []
    cursor = HEADER_SIZE
    for index in range(frame_count):
        frame = list(_FRAME.unpack_from(blob, cursor))
        flags = frame[3]
        steering_bits = frame[18]
        c98_pair, c9c_pair = frame[20], frame[21]
        brake_flag = flags & (BRAKE_APPLIED | BRAKE_APPLIED_NATURAL)
        brake_applied = bool(brake_flag)
        steering_applied = bool(flags & STEERING_APPLIED)
        if frame[19] != bytes(4):
            raise ValueError(f"frame {index}: reserved bytes are nonzero")
        if not brake_applied:
            raise ValueError(f"frame {index}: brake audit flag is missing")
        if brake_flag == (BRAKE_APPLIED | BRAKE_APPLIED_NATURAL):
            raise ValueError(f"frame {index}: conflicting brake audit flags")
        target_brake = c98_pair & 0xFFFFFFFF
        if (c9c_pair & 0xFFFFFFFF) != target_brake:
            raise ValueError(f"frame {index}: brake pair writes differ")
        if steering_applied and (
            c98_pair >> 32 != steering_bits or c9c_pair >> 32 != steering_bits
        ):
            raise ValueError(f"frame {index}: steering pair audit mismatch")
        # Reuse the mature V4 structural verifier.  It only understands one
        # control-applied bit, so synthesize it for either member of the pair.
        frame[3] = (
            flags & ~(BRAKE_APPLIED | BRAKE_APPLIED_NATURAL)
        ) | STEERING_APPLIED
        v4_frames.append(_FRAME_V4.pack(*frame[:18], *frame[22:]))
        cursor += FRAME_SIZE
    header[0] = b"A9UER4\0\0"
    header[1] = 4
    header[3] = _FRAME_V4.size
    return decode_report_v4(_HEADER.pack(*header) + b"".join(v4_frames))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        summary = decode_report(args.report.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9uer6_error={error}")
        return 1
    print(
        f"a9uer6_supported=1 frames={summary.frames} ticks={summary.first_tick}..{summary.last_tick} "
        f"delta_writes={summary.delta_writes} control_writes={summary.control_writes} "
        f"threads={summary.initial_threads}/{summary.final_threads}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
