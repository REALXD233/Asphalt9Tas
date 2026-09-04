#!/usr/bin/env python3
"""Cross-bind a three-frame Gate 9 A9UER5 report to all fixed inputs."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

from parse_unified_executor_report_v2 import CORRECTION_CORRECTED, STEERING_APPLIED
from parse_unified_executor_report_v5 import HEADER_SIZE, FRAME_SIZE, _FRAME, decode_report
from unified_tick_recording_v1 import decode_recording
from verify_unified_steering_final_gate_v1 import verify_steering_final_gate


def verify_gate9_result(
    report_blob: bytes,
    input_blob: bytes,
    manifest: dict[str, object],
    phase_blob: bytes,
    direction_blob: bytes,
    physics_blob: bytes,
) -> tuple[int, int, str, tuple[str, ...]]:
    first, last, interval, digest, steering_bits, _ = verify_steering_final_gate(
        input_blob, manifest, phase_blob, direction_blob, physics_blob
    )
    summary = decode_report(report_blob)
    if summary.frames != 3 or summary.first_tick != first or summary.last_tick != last:
        raise ValueError("Gate 9 report ticks/frame count do not match input")
    if summary.fixed_interval_us != interval:
        raise ValueError("Gate 9 applied fixed interval does not match input")
    if (summary.equal_frames, summary.corrected_frames, summary.skipped_frames) != (0, 3, 0):
        raise ValueError("Gate 9 must report three different-value corrections")
    if summary.delta_writes != 3 or summary.control_writes != 6:
        raise ValueError("Gate 9 delta/control write totals are not 3/6")

    _, input_frames = decode_recording(input_blob)
    for index, input_frame in enumerate(input_frames):
        report_frame = _FRAME.unpack_from(report_blob, HEADER_SIZE + index * FRAME_SIZE)
        flags = report_frame[3]
        if not flags & CORRECTION_CORRECTED or not flags & STEERING_APPLIED:
            raise ValueError(f"frame {index}: steering/corrected evidence is incomplete")
        expected_bits = int.from_bytes(bytes.fromhex(steering_bits[index]), "little")
        if report_frame[18] != expected_bits:
            raise ValueError(f"frame {index}: steering target is not bound to input")
        if report_frame[20] >> 32 != expected_bits or report_frame[21] >> 32 != expected_bits:
            raise ValueError(f"frame {index}: C98/C9C steering audit is not bound to input")
        if report_frame[22] != input_frame.transform or report_frame[23] != input_frame.linear_velocity:
            raise ValueError(f"frame {index}: correction payload is not bound to input")
    return first, last, digest, steering_bits


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("input", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("phase_source", type=Path)
    parser.add_argument("direction_source", type=Path)
    parser.add_argument("physics_source", type=Path)
    args = parser.parse_args()
    try:
        loaded = json.loads(args.manifest.read_text(encoding="utf-8"))
        if not isinstance(loaded, dict):
            raise ValueError("manifest root must be an object")
        first, last, digest, bits = verify_gate9_result(
            args.report.read_bytes(),
            args.input.read_bytes(),
            loaded,
            args.phase_source.read_bytes(),
            args.direction_source.read_bytes(),
            args.physics_source.read_bytes(),
        )
    except (OSError, ValueError, json.JSONDecodeError, struct.error) as error:
        print(f"gate9_steering_final_error={error}")
        return 1
    print(
        f"gate9_steering_final_supported=1 frames=3 ticks={first}..{last} "
        f"delta_writes=3 control_writes=6 corrected=3 "
        f"steering_bits={','.join(bits)} input_sha256={digest}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
