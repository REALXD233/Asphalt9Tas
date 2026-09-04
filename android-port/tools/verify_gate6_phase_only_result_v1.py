#!/usr/bin/env python3
"""Cross-bind a Gate 6 A9UER5 success report to its one-frame A9UTK1 input."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from parse_unified_executor_report_v5 import HEADER_SIZE as REPORT_HEADER_SIZE
from parse_unified_executor_report_v5 import _FRAME as REPORT_FRAME
from parse_unified_executor_report_v5 import decode_report
from unified_tick_recording_v1 import decode_recording
from verify_unified_phase_only_gate_v1 import verify_phase_only_gate


def verify_gate6_result(report_blob: bytes, input_blob: bytes, manifest: dict[str, object]) -> tuple[int, str]:
    input_tick, input_interval, input_digest = verify_phase_only_gate(input_blob, manifest)
    summary = decode_report(report_blob)
    if summary.frames != 1 or summary.first_tick != input_tick or summary.last_tick != input_tick:
        raise ValueError("Gate 6 report tick/frame does not match input")
    if summary.fixed_interval_us != input_interval:
        raise ValueError("Gate 6 applied fixed interval does not match input")
    if (summary.equal_frames, summary.corrected_frames, summary.skipped_frames) != (0, 0, 1):
        raise ValueError("Gate 6 must report exactly one skipped correction")
    if summary.control_writes != 0:
        raise ValueError("Gate 6 must perform zero control writes")
    if summary.delta_writes != 1:
        raise ValueError("Gate 6 must perform exactly one fixed-delta write")
    _, input_frames = decode_recording(input_blob)
    report_frame = REPORT_FRAME.unpack_from(report_blob, REPORT_HEADER_SIZE)
    report_transform = report_frame[22]
    report_linear = report_frame[23]
    if report_transform != input_frames[0].transform or report_linear != input_frames[0].linear_velocity:
        raise ValueError("Gate 6 report payload is not bound to A9UTK1 input")
    return input_tick, input_digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("input", type=Path)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    try:
        loaded = json.loads(args.manifest.read_text(encoding="utf-8"))
        if not isinstance(loaded, dict):
            raise ValueError("manifest root must be an object")
        tick, digest = verify_gate6_result(
            args.report.read_bytes(), args.input.read_bytes(), loaded
        )
    except (OSError, ValueError, json.JSONDecodeError, struct.error) as error:
        print(f"gate6_phase_only_error={error}")
        return 1
    print(
        f"gate6_phase_only_supported=1 frames=1 tick={tick} "
        f"delta_writes=1 control_writes=0 final_correction=skipped "
        f"input_sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
