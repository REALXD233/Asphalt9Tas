#!/usr/bin/env python3
"""Quantify the pre-correction displacement in a validated A9FWR1 run."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import statistics
import struct
import sys

import make_final_writer_target_blob_v1 as target_blob_v1
import validate_final_writer_payload_report_v1 as final_writer_v1


def _floats(raw: bytes) -> tuple[float, ...]:
    return struct.unpack(f"<{len(raw) // 4}f", raw)


def _norm(values: tuple[float, ...] | list[float]) -> float:
    return math.sqrt(sum(value * value for value in values))


def _percentiles(values: list[float]) -> dict[str, float]:
    if not values:
        return {"p50": 0.0, "p90": 0.0, "p99": 0.0, "max": 0.0}
    ordered = sorted(values)

    def select(fraction: float) -> float:
        index = round((len(ordered) - 1) * fraction)
        return ordered[index]

    return {
        "p50": statistics.median(ordered),
        "p90": select(0.90),
        "p99": select(0.99),
        "max": ordered[-1],
    }


def _longest_true_run(values: list[bool]) -> int:
    longest = 0
    current = 0
    for value in values:
        current = current + 1 if value else 0
        longest = max(longest, current)
    return longest


def analyze(report: bytes, target_blob: bytes) -> dict[str, object]:
    validated = final_writer_v1.validate_report(report, target_blob)
    targets = target_blob_v1.decode_target_blob(target_blob)["targets"]

    position_errors: list[float] = []
    basis_errors: list[float] = []
    linear_errors: list[float] = []
    corrected: list[bool] = []
    transform_mismatches = 0
    linear_mismatches = 0
    maximum_position_frame = -1
    maximum_position_error = -1.0
    nearest_target_counts = {"previous": 0, "current": 0, "next": 0}
    nearest_target_ties = 0
    target_positions = [tuple(_floats(transform)[12:15])
                        for transform, _linear in targets]

    cursor = final_writer_v1.HEADER_SIZE
    for frame_index, (target_transform_raw, target_linear_raw) in enumerate(targets):
        (audit_index, flags, before_transform_raw, before_linear_raw,
         _immediate_transform, _immediate_linear) = (
            final_writer_v1._AUDIT.unpack_from(report, cursor)
        )
        cursor += final_writer_v1.AUDIT_SIZE
        if audit_index != frame_index:
            raise ValueError(f"audit {frame_index}: frame index mismatch")

        before_transform = _floats(before_transform_raw)
        target_transform = _floats(target_transform_raw)
        before_linear = _floats(before_linear_raw)
        target_linear = _floats(target_linear_raw)

        position_error = _norm([
            before_transform[index] - target_transform[index]
            for index in range(12, 15)
        ])
        basis_error = _norm([
            before_transform[index] - target_transform[index]
            for index in range(12)
        ])
        linear_error = _norm([
            before_linear[index] - target_linear[index]
            for index in range(3)
        ])
        position_errors.append(position_error)
        basis_errors.append(basis_error)
        linear_errors.append(linear_error)
        is_corrected = bool(flags & final_writer_v1.AUDIT_CORRECTED)
        corrected.append(is_corrected)
        transform_mismatches += int(before_transform_raw != target_transform_raw)
        linear_mismatches += int(before_linear_raw != target_linear_raw)
        if position_error > maximum_position_error:
            maximum_position_error = position_error
            maximum_position_frame = frame_index

        candidates: list[tuple[str, float]] = []
        for label, target_index in (
            ("previous", frame_index - 1),
            ("current", frame_index),
            ("next", frame_index + 1),
        ):
            if 0 <= target_index < len(targets):
                candidate = _floats(targets[target_index][0])
                distance = _norm([
                    before_transform[index] - candidate[index]
                    for index in range(12, 15)
                ])
                candidates.append((label, distance))
        minimum = min(distance for _, distance in candidates)
        winners = [label for label, distance in candidates
                   if math.isclose(distance, minimum, rel_tol=1e-9,
                                   abs_tol=1e-12)]
        if len(winners) == 1:
            nearest_target_counts[winners[0]] += 1
        else:
            nearest_target_ties += 1

    corrected_position_errors = [
        error for error, is_corrected in zip(position_errors, corrected)
        if is_corrected
    ]
    corrected_basis_errors = [
        error for error, is_corrected in zip(basis_errors, corrected)
        if is_corrected
    ]
    corrected_linear_errors = [
        error for error, is_corrected in zip(linear_errors, corrected)
        if is_corrected
    ]
    target_step_distances = [
        _norm([right[index] - left[index] for index in range(3)])
        for left, right in zip(target_positions, target_positions[1:])
    ]
    top_position_frames = sorted(
        ({"frame": index, "error": error}
         for index, error in enumerate(position_errors)),
        key=lambda item: (-item["error"], item["frame"]),
    )[:10]
    return {
        "format": "A9FWR1-correction-magnitude-v1",
        "frames": int(validated["frames"]),
        "equal_frames": int(validated["equal"]),
        "corrected_frames": int(validated["corrected"]),
        "transform_mismatch_frames": transform_mismatches,
        "linear_mismatch_frames": linear_mismatches,
        "longest_consecutive_correction_run": _longest_true_run(corrected),
        "position_error_all": _percentiles(position_errors),
        "position_error_corrected": _percentiles(corrected_position_errors),
        "target_position_step": _percentiles(target_step_distances),
        "position_error_threshold_counts": {
            "gt_0_01": sum(error > 0.01 for error in position_errors),
            "gt_0_1": sum(error > 0.1 for error in position_errors),
            "gt_0_5": sum(error > 0.5 for error in position_errors),
            "gt_1_0": sum(error > 1.0 for error in position_errors),
        },
        "top_position_error_frames": top_position_frames,
        "basis_frobenius_error_corrected": _percentiles(corrected_basis_errors),
        "linear_velocity_error_corrected": _percentiles(corrected_linear_errors),
        "maximum_position_error_frame": maximum_position_frame,
        "nearest_position_target": nearest_target_counts,
        "nearest_position_target_ties": nearest_target_ties,
        "units_note": (
            "position units are the game's native transform units; basis and "
            "linear values are raw float-domain Euclidean differences"
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=pathlib.Path)
    parser.add_argument("target_blob", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        result = analyze(args.report.read_bytes(), args.target_blob.read_bytes())
        encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
        if args.output is not None:
            args.output.write_text(encoded, encoding="utf-8")
        else:
            print(encoded, end="")
    except (OSError, ValueError, struct.error) as error:
        print(f"correction_magnitude_error={error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
