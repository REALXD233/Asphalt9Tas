#!/usr/bin/env python3
"""Compare A9PST1 traces tick-by-tick without hiding divergence."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

from parse_vehicle_state_trace_v1 import read_trace


def vector_error(left: list[float], right: list[float]) -> float:
    return math.sqrt(sum((float(a) - float(b)) ** 2 for a, b in zip(left, right)))


def quaternion_error(left: list[float], right: list[float]) -> float:
    direct = vector_error(left, right)
    negated = vector_error(left, [-float(value) for value in right])
    return min(direct, negated)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("traces", nargs="+", type=Path)
    parser.add_argument(
        "--detail-count",
        type=int,
        default=0,
        help="include the first N ticks whose state exceeds any threshold",
    )
    args = parser.parse_args()
    if len(args.traces) < 2:
        raise SystemExit("at least two traces are required")

    loaded = [read_trace(path) for path in args.traces]
    headers = [item[0] for item in loaded]
    frame_sets = [item[1] for item in loaded]
    frame_count = len(frame_sets[0])
    comparable = all(
        bool(header["clean"])
        and int(header["state_frames"]) == frame_count
        and int(header["fixed_interval_us"])
        == int(headers[0]["fixed_interval_us"])
        for header in headers
    ) and all(len(frames) == frame_count for frames in frame_sets)

    metrics = {key: [] for key in ("position", "rotation", "linear", "angular")}
    metric_locations: dict[str, list[tuple[int, int]]] = {
        key: [] for key in metrics
    }
    per_tick_errors: list[dict[str, float]] = []
    first_divergence: dict[str, int | None] = {key: None for key in metrics}
    thresholds = {
        "position": 1e-4,
        "rotation": 1e-5,
        "linear": 1e-4,
        "angular": 1e-5,
    }
    if comparable:
        reference = frame_sets[0]
        for run_index, run in enumerate(frame_sets[1:], start=1):
            for tick, (left, right) in enumerate(zip(reference, run)):
                tick_errors: dict[str, float] = {}
                for key in metrics:
                    error = (
                        quaternion_error(left[key], right[key])
                        if key == "rotation"
                        else vector_error(left[key], right[key])
                    )
                    metrics[key].append(error)
                    metric_locations[key].append((run_index, tick))
                    tick_errors[key] = error
                    if error > thresholds[key] and first_divergence[key] is None:
                        first_divergence[key] = tick
                if len(frame_sets) == 2:
                    per_tick_errors.append(tick_errors)

    peak_index = {
        key: (max(range(len(values)), key=values.__getitem__) if values else None)
        for key, values in metrics.items()
    }
    peak_location = {
        key: (
            {
                "run_index": metric_locations[key][index][0],
                "tick": metric_locations[key][index][1],
            }
            if index is not None
            else None
        )
        for key, index in peak_index.items()
    }
    peak_details = {}
    if comparable and len(frame_sets) == 2:
        for key, location in peak_location.items():
            if location is None:
                continue
            tick = int(location["tick"])
            peak_details[key] = {
                "tick": tick,
                "error": per_tick_errors[tick][key],
                "run0": frame_sets[0][tick][key],
                "run1": frame_sets[1][tick][key],
            }
    divergence_details = []
    if comparable and len(frame_sets) == 2 and args.detail_count > 0:
        for tick, errors in enumerate(per_tick_errors):
            if not any(errors[key] > thresholds[key] for key in metrics):
                continue
            left = frame_sets[0][tick]
            right = frame_sets[1][tick]
            divergence_details.append(
                {
                    "tick": tick,
                    "error": errors,
                    "run0": {
                        key: left[key]
                        for key in ("position", "rotation", "linear", "angular")
                    },
                    "run1": {
                        key: right[key]
                        for key in ("position", "rotation", "linear", "angular")
                    },
                }
            )
            if len(divergence_details) >= args.detail_count:
                break

    result = {
        "traces": [str(path) for path in args.traces],
        "comparable": comparable,
        "frame_count": frame_count,
        "fixed_interval_us": headers[0]["fixed_interval_us"],
        "max_error": {
            key: max(values) if values else None for key, values in metrics.items()
        },
        "mean_error": {
            key: sum(values) / len(values) if values else None
            for key, values in metrics.items()
        },
        "first_divergence_tick": first_divergence,
        "peak_error_location": peak_location,
        "peak_error_detail": peak_details,
        "divergence_details": divergence_details,
        "thresholds": thresholds,
    }
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if comparable else 2


if __name__ == "__main__":
    raise SystemExit(main())
