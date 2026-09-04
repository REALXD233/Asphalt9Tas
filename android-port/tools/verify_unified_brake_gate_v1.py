#!/usr/bin/env python3
"""Cross-bind one-frame A9UER6 brake audit to its A9UTK1 target."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from parse_unified_executor_report_v2 import CORRECTION_SKIPPED
from parse_unified_executor_report_v5 import HEADER_SIZE, _FRAME
from parse_unified_executor_report_v6 import BRAKE_APPLIED, decode_report
from unified_tick_recording_v1 import SKIP_BRAKE, decode_recording


def verify(report: bytes, recording: bytes) -> None:
    summary = decode_report(report)
    fixed, frames = decode_recording(recording)
    if len(frames) != 1 or summary.frames != 1 or summary.fixed_interval_us != fixed:
        raise ValueError("Gate 10 frame/interval mismatch")
    target = frames[0]
    if target.skip_flags != 0xFD or target.skip_flags & SKIP_BRAKE:
        raise ValueError("Gate 10 must enable only brake")
    if struct.pack("<f", target.brake) != bytes.fromhex("000080bf"):
        raise ValueError("Gate 10 brake target must be raw -1.0")
    frame = _FRAME.unpack_from(report, HEADER_SIZE)
    flags = frame[3]
    if not flags & BRAKE_APPLIED or not flags & CORRECTION_SKIPPED:
        raise ValueError("Gate 10 audit flags mismatch")
    target_bits = struct.unpack("<I", struct.pack("<f", target.brake))[0]
    if frame[20] & 0xFFFFFFFF != target_bits or frame[21] & 0xFFFFFFFF != target_bits:
        raise ValueError("Gate 10 brake write bits mismatch")
    if frame[22] != target.transform or frame[23] != target.linear_velocity:
        raise ValueError("Gate 10 recorded payload binding mismatch")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("recording", type=Path)
    args = parser.parse_args()
    try:
        verify(args.report.read_bytes(), args.recording.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"gate10_brake_error={error}")
        return 1
    print("gate10_brake_supported=1 frames=1 brake_bits=bf800000 writes=2")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
