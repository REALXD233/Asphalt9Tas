#!/usr/bin/env python3
"""Quantify correction clustering in start-line A9UER6 replay reports."""

from __future__ import annotations

import argparse
import json
import math
import struct
from pathlib import Path

from aligned_tick_replay_v1 import _snapshot_payload
from parse_unified_executor_report_v2 import CORRECTION_CORRECTED
from parse_unified_executor_report_v5 import FRAME_SIZE, HEADER_SIZE, _FRAME
from parse_unified_executor_report_v6 import decode_report as decode_report_v6
from parse_unified_executor_report_v8 import decode_report as decode_report_v8
from unified_tick_recording_v1 import decode_recording


def consecutive_runs(indices: list[int]) -> list[tuple[int, int]]:
    if not indices:
        return []
    runs: list[tuple[int, int]] = []
    start = previous = indices[0]
    for index in indices[1:]:
        if index == previous + 1:
            previous = index
            continue
        runs.append((start, previous))
        start = previous = index
    runs.append((start, previous))
    return runs


def first_motion_frame(frames, threshold: float = 0.05) -> int | None:
    for index, frame in enumerate(frames):
        velocity = struct.unpack("<3f", frame.linear_velocity)
        if math.sqrt(sum(value * value for value in velocity)) > threshold:
            return index
    return None


def first_control_frame(frames, threshold: float = 1e-5) -> int | None:
    for index, frame in enumerate(frames):
        if (abs(frame.steering) > threshold or abs(frame.brake) > threshold or
                abs(frame.accelerator) > threshold or frame.nitro_activations != 0 or
                frame.respawn):
            return index
    return None


def motion_onset_metrics(
    target_velocities: list[tuple[float, float, float]],
    native_velocities: list[tuple[float, float, float]],
    threshold: float = 0.05,
) -> dict[str, object]:
    """Measure a replay holding a natively moving car at a static target.

    This is intentionally separate from visual snap detection: a conditional
    final writer can remain spatially smooth while delaying the race start.
    Six 60-Hz frames (about 100 ms) are enough to flag a visible temporal hold.
    """
    if len(target_velocities) != len(native_velocities):
        raise ValueError("target/native velocity count mismatch")

    def moving(velocity: tuple[float, float, float]) -> bool:
        return math.sqrt(sum(value * value for value in velocity)) > threshold

    target_flags = [moving(item) for item in target_velocities]
    native_flags = [moving(item) for item in native_velocities]
    target_first = next((i for i, value in enumerate(target_flags) if value), None)
    native_first = next((i for i, value in enumerate(native_flags) if value), None)
    held = [
        index for index, (target, native) in
        enumerate(zip(target_flags, native_flags))
        if native and not target
    ]
    runs = consecutive_runs(held)
    longest = max((end - start + 1 for start, end in runs), default=0)
    lag = (
        target_first - native_first
        if target_first is not None and native_first is not None
        else None
    )
    return {
        "first_target_motion_frame": target_first,
        "first_native_motion_frame": native_first,
        "motion_onset_lag_frames": lag,
        "temporal_hold_frames": len(held),
        "temporal_hold_run_count": len(runs),
        "longest_temporal_hold_run": longest,
        "temporal_hold_risk": longest >= 6,
    }


def analyze_report(blob: bytes, frame_count: int) -> dict[str, object]:
    if blob.startswith(b"A9UER8\0\0"):
        summary = decode_report_v8(blob)
    else:
        summary = decode_report_v6(blob)
    if summary.frames != frame_count:
        raise ValueError("report/recording frame-count mismatch")
    corrected: list[int] = []
    target_velocities: list[tuple[float, float, float]] = []
    native_velocities: list[tuple[float, float, float]] = []
    maximum_transform_error = 0.0
    maximum_linear_error = 0.0
    for index in range(frame_count):
        frame = _FRAME.unpack_from(blob, HEADER_SIZE + index * FRAME_SIZE)
        target = struct.unpack("<19f", frame[22] + frame[23])
        before = struct.unpack("<19f", _snapshot_payload(frame[24]))
        target_velocities.append(target[16:])
        native_velocities.append(before[16:])
        if not frame[3] & CORRECTION_CORRECTED:
            continue
        corrected.append(index)
        maximum_transform_error = max(
            maximum_transform_error,
            max(abs(left - right) for left, right in zip(target[:16], before[:16])),
        )
        maximum_linear_error = max(
            maximum_linear_error,
            max(abs(left - right) for left, right in zip(target[16:], before[16:])),
        )
    runs = consecutive_runs(corrected)
    longest = max((end - start + 1 for start, end in runs), default=0)
    result = {
        "frames": frame_count,
        "equal_frames": summary.equal_frames,
        "corrected_frames": summary.corrected_frames,
        "corrected_ratio": summary.corrected_frames / frame_count,
        "first_corrected_frame": corrected[0] if corrected else None,
        "last_corrected_frame": corrected[-1] if corrected else None,
        "correction_run_count": len(runs),
        "longest_correction_run": longest,
        "longest_runs": [
            {"start": start, "end": end, "length": end - start + 1}
            for start, end in sorted(
                runs, key=lambda item: item[1] - item[0], reverse=True
            )[:8]
        ],
        "maximum_transform_component_error": maximum_transform_error,
        "maximum_linear_component_error": maximum_linear_error,
        "corrected_indices": corrected,
    }
    result.update(motion_onset_metrics(target_velocities, native_velocities))
    return result


def analyze(recording_blob: bytes, reports: list[tuple[str, bytes]]) -> dict[str, object]:
    _, frames = decode_recording(recording_blob)
    motion = first_motion_frame(frames)
    control = first_control_frame(frames)
    report_results = []
    corrected_sets: list[set[int]] = []
    for name, blob in reports:
        result = analyze_report(blob, len(frames))
        corrected_sets.append(set(result.pop("corrected_indices")))
        result["report"] = name
        report_results.append(result)
    common = set.intersection(*corrected_sets) if corrected_sets else set()
    union = set.union(*corrected_sets) if corrected_sets else set()
    first_corrections = [
        item["first_corrected_frame"] for item in report_results
        if item["first_corrected_frame"] is not None
    ]
    longest = max(
        (int(item["longest_correction_run"]) for item in report_results),
        default=0,
    )
    temporal_hold_risk = any(
        bool(item["temporal_hold_risk"]) for item in report_results
    )
    systematic_before_input = bool(
        first_corrections and control is not None and
        max(first_corrections) <= control
    )
    return {
        "recording_frames": len(frames),
        "first_motion_frame": motion,
        "first_control_frame": control,
        "reports": report_results,
        "common_corrected_frames": len(common),
        "union_corrected_frames": len(union),
        "systematic_correction_before_manual_input": systematic_before_input,
        "visual_snap_risk": systematic_before_input and longest >= 30,
        "temporal_hold_risk": temporal_hold_risk,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("reports", type=Path, nargs="+")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        result = analyze(
            args.recording.read_bytes(),
            [(path.name, path.read_bytes()) for path in args.reports],
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"replay_smoothness_error={error}")
        return 1
    encoded = json.dumps(result, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(encoded + "\n", encoding="utf-8")
        print(f"REPLAY_SMOOTHNESS_ANALYSIS_OK output={args.output}")
    else:
        print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
