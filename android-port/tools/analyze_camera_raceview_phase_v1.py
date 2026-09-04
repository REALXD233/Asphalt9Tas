#!/usr/bin/env python3
"""Analyze timestamped RaceView manager/shape samples without device access."""

from __future__ import annotations

import json
import math
import pathlib
import statistics
import sys
from dataclasses import dataclass


@dataclass(frozen=True)
class Sample:
    index: int
    time_us: int
    mask: int
    manager_position: tuple[float, float, float]
    shape_position: tuple[float, float, float]
    fov: float


def _vector(value: str, expected: int) -> tuple[float, ...]:
    result = tuple(float(item) for item in value.split(","))
    if len(result) != expected:
        raise ValueError(f"expected {expected} components, got {len(result)}")
    return result


def parse_samples(text: str) -> list[Sample]:
    samples: list[Sample] = []
    for line in text.splitlines():
        if not line.startswith("RACEVIEW_PHASE sample="):
            continue
        fields = dict(token.split("=", 1) for token in line.split()[1:])
        samples.append(Sample(
            index=int(fields["sample"]),
            time_us=int(fields["t_us"]),
            mask=int(fields["mask"]),
            manager_position=_vector(fields["mp"], 3),
            shape_position=_vector(fields["sp"], 3),
            fov=float(fields["fov"]),
        ))
    if len(samples) < 3:
        raise ValueError("at least three RaceView phase samples are required")
    if any(right.index != left.index + 1
           for left, right in zip(samples, samples[1:])):
        raise ValueError("non-contiguous RaceView sample indices")
    if any(right.time_us <= left.time_us
           for left, right in zip(samples, samples[1:])):
        raise ValueError("non-monotonic RaceView timestamps")
    return samples


def _velocity(samples: list[Sample], shape: bool) -> list[tuple[float, ...]]:
    field = "shape_position" if shape else "manager_position"
    output: list[tuple[float, ...]] = [(0.0, 0.0, 0.0)]
    for previous, current in zip(samples, samples[1:]):
        left = getattr(previous, field)
        right = getattr(current, field)
        output.append(tuple(b - a for a, b in zip(left, right)))
    return output


def best_velocity_shift(samples: list[Sample], radius: int = 8) -> dict:
    manager = _velocity(samples, shape=False)
    final_shape = _velocity(samples, shape=True)
    radius = min(radius, max(1, len(samples) // 4))
    candidates: list[tuple[float, int, int]] = []
    for shift in range(-radius, radius + 1):
        dot = 0.0
        manager_energy = 0.0
        shape_energy = 0.0
        pairs = 0
        for manager_index in range(1, len(samples)):
            shape_index = manager_index + shift
            if shape_index < 1 or shape_index >= len(samples):
                continue
            left = manager[manager_index]
            right = final_shape[shape_index]
            left_energy = sum(value * value for value in left)
            right_energy = sum(value * value for value in right)
            if left_energy <= 1.0e-12 and right_energy <= 1.0e-12:
                continue
            dot += sum(a * b for a, b in zip(left, right))
            manager_energy += left_energy
            shape_energy += right_energy
            pairs += 1
        score = (dot / math.sqrt(manager_energy * shape_energy)
                 if manager_energy > 0.0 and shape_energy > 0.0 else -1.0)
        candidates.append((score, shift, pairs))
    score, shift, pairs = max(candidates)
    return {"samples": shift, "score": score, "pairs": pairs}


def _nearest_deltas(source: list[Sample], target: list[Sample],
                    radius_us: int = 50000) -> list[int]:
    deltas: list[int] = []
    for item in source:
        nearest = min(target, key=lambda candidate:
                      abs(candidate.time_us - item.time_us), default=None)
        if nearest is not None:
            delta = nearest.time_us - item.time_us
            if abs(delta) <= radius_us:
                deltas.append(delta)
    return deltas


def analyze(samples: list[Sample]) -> dict:
    manager_events = [sample for sample in samples[1:] if sample.mask & 1]
    shape_events = [sample for sample in samples[1:] if sample.mask & 2]
    fov_events = [sample for sample in samples[1:] if sample.mask & 4]
    manager_to_shape = _nearest_deltas(manager_events, shape_events)
    manager_to_fov = _nearest_deltas(manager_events, fov_events)
    offsets = [
        math.sqrt(sum((shape - manager) ** 2
                      for manager, shape in zip(sample.manager_position,
                                                sample.shape_position)))
        for sample in samples
    ]
    intervals = [right.time_us - left.time_us
                 for left, right in zip(samples, samples[1:])]
    mask_histogram = {
        str(mask): sum(1 for sample in samples[1:] if sample.mask == mask)
        for mask in range(8)
    }
    same_manager_shape = sum(
        1 for sample in samples[1:] if sample.mask & 3 == 3)
    same_manager_fov = sum(
        1 for sample in samples[1:] if sample.mask & 5 == 5)
    return {
        "samples": len(samples),
        "elapsed_us": samples[-1].time_us - samples[0].time_us,
        "median_interval_us": statistics.median(intervals),
        "manager_events": len(manager_events),
        "shape_events": len(shape_events),
        "fov_events": len(fov_events),
        "same_sample_manager_shape": same_manager_shape,
        "same_sample_manager_fov": same_manager_fov,
        "manager_without_shape": len(manager_events) - same_manager_shape,
        "shape_without_manager": len(shape_events) - same_manager_shape,
        "fov_without_manager": len(fov_events) - same_manager_fov,
        "mask_histogram": mask_histogram,
        "manager_to_nearest_shape_us_median": (
            statistics.median(manager_to_shape) if manager_to_shape else None),
        "manager_to_nearest_fov_us_median": (
            statistics.median(manager_to_fov) if manager_to_fov else None),
        "best_shape_velocity_shift": best_velocity_shift(samples),
        "manager_shape_distance_min": min(offsets),
        "manager_shape_distance_median": statistics.median(offsets),
        "manager_shape_distance_max": max(offsets),
        "device_access": 0,
        "gameplay_writes": 0,
    }


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} PHASE_LOG", file=sys.stderr)
        return 2
    source = pathlib.Path(sys.argv[1])
    report = analyze(parse_samples(source.read_text(encoding="utf-8")))
    print("RACEVIEW_PHASE_ANALYSIS " + json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
