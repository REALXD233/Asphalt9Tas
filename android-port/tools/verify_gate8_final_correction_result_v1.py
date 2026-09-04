#!/usr/bin/env python3
"""Cross-bind a corrected Gate 8 A9UER5 report to its exact A9UTK1 input."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from parse_unified_executor_report_v2 import CORRECTION_CORRECTED, STEERING_APPLIED
from parse_unified_executor_report_v5 import HEADER_SIZE, _FRAME, decode_report
from unified_tick_recording_v1 import decode_recording
from verify_unified_final_correction_gate_v1 import verify_final_correction_gate


def verify_gate8_result(
    report_blob: bytes,
    input_blob: bytes,
    manifest: dict[str, object],
    phase_blob: bytes,
    physics_blob: bytes,
) -> tuple[int, str, int]:
    input_tick, input_interval, input_digest, mismatches = (
        verify_final_correction_gate(
            input_blob, manifest, phase_blob, physics_blob
        )
    )
    summary = decode_report(report_blob)
    if summary.frames != 1 or summary.first_tick != input_tick or summary.last_tick != input_tick:
        raise ValueError("Gate 8 report tick/frame does not match input")
    if summary.fixed_interval_us != input_interval:
        raise ValueError("Gate 8 applied fixed interval does not match input")
    if (summary.equal_frames, summary.corrected_frames, summary.skipped_frames) != (0, 1, 0):
        raise ValueError("Gate 8 must report exactly one different-value correction")
    if summary.delta_writes != 1 or summary.control_writes != 0:
        raise ValueError("Gate 8 must perform one delta and no control writes")

    _, input_frames = decode_recording(input_blob)
    report_frame = _FRAME.unpack_from(report_blob, HEADER_SIZE)
    frame_flags = report_frame[3]
    if not frame_flags & CORRECTION_CORRECTED:
        raise ValueError("Gate 8 report lacks corrected-branch evidence")
    if frame_flags & STEERING_APPLIED:
        raise ValueError("Gate 8 unexpectedly applied steering")
    if report_frame[18] != 0 or report_frame[20] != 0 or report_frame[21] != 0:
        raise ValueError("Gate 8 contains a steering value or write audit")
    if (
        report_frame[22] != input_frames[0].transform
        or report_frame[23] != input_frames[0].linear_velocity
    ):
        raise ValueError("Gate 8 report correction payload is not bound to input")
    return input_tick, input_digest, mismatches


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("input", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("phase_source", type=Path)
    parser.add_argument("physics_source", type=Path)
    args = parser.parse_args()
    try:
        loaded = json.loads(args.manifest.read_text(encoding="utf-8"))
        if not isinstance(loaded, dict):
            raise ValueError("manifest root must be an object")
        tick, digest, mismatches = verify_gate8_result(
            args.report.read_bytes(),
            args.input.read_bytes(),
            loaded,
            args.phase_source.read_bytes(),
            args.physics_source.read_bytes(),
        )
    except (OSError, ValueError, json.JSONDecodeError, struct.error) as error:
        print(f"gate8_final_correction_error={error}")
        return 1
    print(
        f"gate8_final_correction_supported=1 frames=1 tick={tick} "
        f"delta_writes=1 control_writes=0 corrected=1 "
        f"mismatch_components={mismatches} input_sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
