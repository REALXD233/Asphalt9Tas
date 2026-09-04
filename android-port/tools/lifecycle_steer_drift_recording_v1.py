#!/usr/bin/env python3
"""Strict action-content validator for lifecycle-bound steer/drift capture."""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path

from lifecycle_source_recording_v1 import verify_lifecycle_source_capture
from unified_tick_recording_v1 import UnifiedTickFrameV1, decode_recording


STEER_ACTIVE_THRESHOLD = 0.10
BRAKE_ACTIVE_THRESHOLD = -0.50


@dataclass(frozen=True)
class SteerDriftCoverageV1:
    frames: int
    first_steer_tick: int
    first_brake_tick: int
    first_overlap_tick: int
    steer_frames: int
    brake_frames: int
    overlap_frames: int
    brake_release_tick: int


def analyze_steer_drift_frames(
    frames: tuple[UnifiedTickFrameV1, ...]
) -> SteerDriftCoverageV1:
    if not frames or frames[0].tick != 0:
        raise ValueError("steer/drift recording must begin at tick 0")
    steer = [
        index for index, frame in enumerate(frames)
        if abs(frame.steering) >= STEER_ACTIVE_THRESHOLD
    ]
    brake = [
        index for index, frame in enumerate(frames)
        if frame.brake <= BRAKE_ACTIVE_THRESHOLD
    ]
    overlap = [
        index for index, frame in enumerate(frames)
        if abs(frame.steering) >= STEER_ACTIVE_THRESHOLD
        and frame.brake <= BRAKE_ACTIVE_THRESHOLD
    ]
    if not steer:
        raise ValueError("no authoritative steering action was captured")
    if not brake:
        raise ValueError("no authoritative brake/drift pulse was captured")
    if not overlap:
        raise ValueError("steering and brake/drift never overlapped")

    last_brake = brake[-1]
    release = next(
        (
            index for index in range(last_brake + 1, len(frames))
            if math.isclose(frames[index].brake, 0.0, abs_tol=0.0)
        ),
        None,
    )
    if release is None:
        raise ValueError("brake/drift release was not captured")

    # Ensure the action values are canonical finite controls, not merely values
    # which happened to cross the activity thresholds.
    for index in set(steer + brake):
        frame = frames[index]
        if not math.isfinite(frame.steering) or not math.isfinite(frame.brake):
            raise ValueError(f"frame {index}: non-finite action value")
        if abs(frame.steering) > 1.05 or abs(frame.brake) > 1.05:
            raise ValueError(f"frame {index}: action value is out of range")

    return SteerDriftCoverageV1(
        frames=len(frames),
        first_steer_tick=steer[0],
        first_brake_tick=brake[0],
        first_overlap_tick=overlap[0],
        steer_frames=len(steer),
        brake_frames=len(brake),
        overlap_frames=len(overlap),
        brake_release_tick=release,
    )


def verify_lifecycle_steer_drift_capture(
    report_blob: bytes, recording_blob: bytes
) -> SteerDriftCoverageV1:
    report = verify_lifecycle_source_capture(report_blob, recording_blob)
    _fixed_interval_us, frames = decode_recording(recording_blob)
    if report.captured_frames != len(frames):
        raise ValueError("lifecycle action frame count mismatch")
    return analyze_steer_drift_frames(frames)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("recording", type=Path)
    args = parser.parse_args()
    try:
        result = verify_lifecycle_steer_drift_capture(
            args.report.read_bytes(), args.recording.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9usr5_action_error={error}")
        return 1
    print(
        "a9usr5_action_supported=1 "
        f"frames={result.frames} first_steer={result.first_steer_tick} "
        f"first_brake={result.first_brake_tick} "
        f"first_overlap={result.first_overlap_tick} "
        f"steer_frames={result.steer_frames} "
        f"brake_frames={result.brake_frames} "
        f"overlap_frames={result.overlap_frames} "
        f"brake_release={result.brake_release_tick}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
