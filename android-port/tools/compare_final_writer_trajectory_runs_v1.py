#!/usr/bin/env python3
"""Compare pre-correction vehicle trajectories across validated A9FWR1 runs."""

from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys

import validate_final_writer_payload_report_v1 as final_writer_v1


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _before_trajectory(report: bytes) -> list[tuple[bytes, bytes, int]]:
    header = final_writer_v1._HEADER.unpack_from(report)
    frame_count = header[5]
    trajectory: list[tuple[bytes, bytes, int]] = []
    cursor = final_writer_v1.HEADER_SIZE
    for index in range(frame_count):
        (frame_index, flags, before_transform, before_linear,
         _immediate_transform, _immediate_linear) = (
            final_writer_v1._AUDIT.unpack_from(report, cursor)
        )
        cursor += final_writer_v1.AUDIT_SIZE
        _require(frame_index == index, f"run audit {index}: index mismatch")
        trajectory.append((before_transform, before_linear, flags))
    return trajectory


def compare_runs(reports: list[bytes], target_blob: bytes,
                 payload: bytes | None = None) -> dict[str, object]:
    _require(len(reports) >= 2, "at least two A9FWR1 runs are required")
    validated = [
        final_writer_v1.validate_report(report, target_blob, payload)
        for report in reports
    ]
    frame_count = int(validated[0]["frames"])
    recording_sha256 = str(validated[0]["recording_sha256"])
    payload_sha256 = str(validated[0]["payload_sha256"])
    for index, summary in enumerate(validated[1:], 1):
        _require(int(summary["frames"]) == frame_count,
                 f"run {index}: frame count differs")
        _require(str(summary["recording_sha256"]) == recording_sha256,
                 f"run {index}: source recording differs")
        _require(str(summary["payload_sha256"]) == payload_sha256,
                 f"run {index}: final-writer payload differs")

    trajectories = [_before_trajectory(report) for report in reports]
    baseline = trajectories[0]
    comparisons: list[dict[str, int]] = []
    all_identical = True
    for run_index, candidate in enumerate(trajectories[1:], 1):
        first_divergence = -1
        differing_frames = 0
        transform_differences = 0
        linear_differences = 0
        class_differences = 0
        for frame_index, (left, right) in enumerate(zip(baseline, candidate)):
            transform_differs = left[0] != right[0]
            linear_differs = left[1] != right[1]
            class_differs = (
                bool(left[2] & final_writer_v1.AUDIT_EQUAL) !=
                bool(right[2] & final_writer_v1.AUDIT_EQUAL)
            )
            if transform_differs or linear_differs:
                if first_divergence < 0:
                    first_divergence = frame_index
                differing_frames += 1
                transform_differences += int(transform_differs)
                linear_differences += int(linear_differs)
            class_differences += int(class_differs)
        identical = first_divergence < 0
        all_identical = all_identical and identical
        comparisons.append({
            "baseline_run": 0,
            "candidate_run": run_index,
            "first_divergence_frame": first_divergence,
            "differing_frames": differing_frames,
            "transform_differences": transform_differences,
            "linear_differences": linear_differences,
            "correction_class_differences": class_differences,
            "identical": int(identical),
        })

    return {
        "runs": len(reports),
        "frames": frame_count,
        "recording_sha256": recording_sha256,
        "payload_sha256": payload_sha256,
        "all_pre_correction_trajectories_identical": int(all_identical),
        "comparisons": comparisons,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target_blob", type=pathlib.Path)
    parser.add_argument("reports", nargs="+", type=pathlib.Path)
    parser.add_argument("--payload", type=pathlib.Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        result = compare_runs(
            [path.read_bytes() for path in args.reports],
            args.target_blob.read_bytes(),
            args.payload.read_bytes() if args.payload is not None else None,
        )
    except (OSError, ValueError, struct.error) as error:
        print(f"trajectory_compare_error={error}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(result, sort_keys=True))
    else:
        print(
            "FINAL_WRITER_TRAJECTORY_COMPARISON_VALID "
            f"runs={result['runs']} frames={result['frames']} "
            "pre_correction_identical="
            f"{result['all_pre_correction_trajectories_identical']}"
        )
        for comparison in result["comparisons"]:
            print("TRAJECTORY_PAIR " + " ".join(
                f"{key}={value}" for key, value in comparison.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
