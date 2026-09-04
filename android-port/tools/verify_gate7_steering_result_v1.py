#!/usr/bin/env python3
"""Cross-bind a Gate 7 A9UER5 report to its steering-only A9UTK1 input."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from parse_unified_executor_report_v2 import STEERING_APPLIED
from parse_unified_executor_report_v5 import HEADER_SIZE, _FRAME, decode_report
from unified_tick_recording_v1 import decode_recording
from verify_unified_steering_gate_v1 import verify_steering_gate


def verify_gate7_result(
    report_blob: bytes,
    input_blob: bytes,
    manifest: dict[str, object],
) -> tuple[int, str, str]:
    input_tick, input_interval, input_digest, steering_bits_hex = (
        verify_steering_gate(input_blob, manifest)
    )
    summary = decode_report(report_blob)
    if summary.frames != 1 or summary.first_tick != input_tick or summary.last_tick != input_tick:
        raise ValueError("Gate 7 report tick/frame does not match input")
    if summary.fixed_interval_us != input_interval:
        raise ValueError("Gate 7 applied fixed interval does not match input")
    if (summary.equal_frames, summary.corrected_frames, summary.skipped_frames) != (0, 0, 1):
        raise ValueError("Gate 7 must report exactly one skipped correction")
    if summary.delta_writes != 1 or summary.control_writes != 2:
        raise ValueError("Gate 7 must perform one delta and two steering writes")

    _, input_frames = decode_recording(input_blob)
    report_frame = _FRAME.unpack_from(report_blob, HEADER_SIZE)
    frame_flags = report_frame[3]
    steering_bits = report_frame[18]
    c98_pair_after = report_frame[20]
    c9c_pair_after = report_frame[21]
    if not frame_flags & STEERING_APPLIED:
        raise ValueError("Gate 7 report lacks steering-applied evidence")
    expected_bits = int.from_bytes(bytes.fromhex(steering_bits_hex), "little")
    if steering_bits != expected_bits:
        raise ValueError("Gate 7 report steering bits are not bound to input")
    if c98_pair_after >> 32 != expected_bits or c9c_pair_after >> 32 != expected_bits:
        raise ValueError("Gate 7 C98/C9C write audits are not bound to input")
    if (
        report_frame[22] != input_frames[0].transform
        or report_frame[23] != input_frames[0].linear_velocity
    ):
        raise ValueError("Gate 7 report physics payload is not bound to input")
    return input_tick, input_digest, steering_bits_hex


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
        tick, digest, steering_bits = verify_gate7_result(
            args.report.read_bytes(), args.input.read_bytes(), loaded
        )
    except (OSError, ValueError, json.JSONDecodeError, struct.error) as error:
        print(f"gate7_steering_error={error}")
        return 1
    print(
        f"gate7_steering_supported=1 frames=1 tick={tick} "
        f"delta_writes=1 control_writes=2 steering_bits={steering_bits} "
        f"final_correction=skipped input_sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
