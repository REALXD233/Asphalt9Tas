#!/usr/bin/env python3
"""Cross-bind A9UTK1 steering/brake bits to A9UER6/A9UER8 writes."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

import parse_unified_executor_report_v6 as report_v6
import parse_unified_executor_report_v8 as report_v8
from lifecycle_steer_drift_recording_v1 import analyze_steer_drift_frames
from parse_unified_executor_report_v2 import HEADER_SIZE, _HEADER
from parse_unified_executor_report_v5 import FRAME_SIZE, _FRAME
from unified_tick_recording_v1 import decode_recording


STEERING_APPLIED = 1 << 0
BRAKE_APPLIED = 1 << 8
BRAKE_APPLIED_NATURAL = 1 << 9


@dataclass(frozen=True)
class ActionReplayProofV1:
    version: int
    frames: int
    control_writes: int
    steer_frames: int
    brake_frames: int
    overlap_frames: int


def _float_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def validate_action_control_replay(
    recording_blob: bytes, report_blob: bytes
) -> ActionReplayProofV1:
    _fixed_interval_us, frames = decode_recording(recording_blob)
    coverage = analyze_steer_drift_frames(frames)
    if len(report_blob) < HEADER_SIZE:
        raise ValueError("executor report is shorter than its header")
    header = _HEADER.unpack_from(report_blob)
    magic, version = header[0], header[1]
    if magic == report_v6.MAGIC and version == report_v6.VERSION:
        report_v6.decode_report(report_blob)
    elif magic == report_v8.MAGIC and version == report_v8.VERSION:
        report_v8.decode_report(report_blob)
    else:
        raise ValueError("expected A9UER6 or A9UER8 action replay report")
    if header[2] != HEADER_SIZE or header[3] != FRAME_SIZE:
        raise ValueError("action replay report ABI mismatch")
    if header[5] != len(frames) or header[6] != len(frames):
        raise ValueError("recording/report frame count mismatch")
    if header[26] != len(frames) * 2:
        raise ValueError("action replay must write two control pairs per frame")

    cursor = HEADER_SIZE
    for index, recorded in enumerate(frames):
        audit = _FRAME.unpack_from(report_blob, cursor)
        cursor += FRAME_SIZE
        if audit[0] != recorded.tick or audit[1] != recorded.monotonic_ns:
            raise ValueError(f"frame {index}: tick/timestamp provenance mismatch")
        flags = audit[3]
        brake_flag = flags & (BRAKE_APPLIED | BRAKE_APPLIED_NATURAL)
        if not (flags & STEERING_APPLIED) or brake_flag not in (
            BRAKE_APPLIED, BRAKE_APPLIED_NATURAL
        ):
            raise ValueError(f"frame {index}: steering/brake applied flags missing")
        steering_bits = _float_bits(recorded.steering)
        brake_bits = _float_bits(recorded.brake)
        expected_pair = (steering_bits << 32) | brake_bits
        if audit[18] != steering_bits:
            raise ValueError(f"frame {index}: steering provenance bits mismatch")
        if audit[20] != expected_pair or audit[21] != expected_pair:
            raise ValueError(f"frame {index}: C98/C9C control pair differs from A9UTK1")

    return ActionReplayProofV1(
        version=version,
        frames=len(frames),
        control_writes=header[26],
        steer_frames=coverage.steer_frames,
        brake_frames=coverage.brake_frames,
        overlap_frames=coverage.overlap_frames,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("executor_report", type=Path)
    args = parser.parse_args()
    try:
        proof = validate_action_control_replay(
            args.recording.read_bytes(), args.executor_report.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"action_control_replay_error={error}")
        return 1
    print(
        "ACTION_CONTROL_REPLAY_VALID passed=1 "
        f"a9uer_version={proof.version} frames={proof.frames} "
        f"control_writes={proof.control_writes} "
        f"steer_frames={proof.steer_frames} "
        f"brake_frames={proof.brake_frames} "
        f"overlap_frames={proof.overlap_frames} raw_bit_exact=1 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
