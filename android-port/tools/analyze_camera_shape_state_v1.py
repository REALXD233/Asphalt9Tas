#!/usr/bin/env python3
"""Quantify relationships between the bounded camera-shape transform samples."""

from __future__ import annotations

import argparse
import math
import re
from pathlib import Path


SAMPLE_RE = re.compile(
    r"^CAMERA_SHAPE_SAMPLE index=(?P<index>\d+) elapsed_ms=(?P<elapsed>\d+) .*?"
    r"a_pos=(?P<a_pos>[^ ]+) a_q=(?P<a_q>[^ ]+) "
    r"b_pos=(?P<b_pos>[^ ]+) b_q=(?P<b_q>[^ ]+) "
    r"motion=(?P<motion>[^ ]+) "
    r"c_pos=(?P<c_pos>[^ ]+) c_q=(?P<c_q>[^ ]+)$"
)


def vector(text: str) -> tuple[float, ...]:
    return tuple(float(item) for item in text.split(","))


def maximum_difference(left: tuple[float, ...], right: tuple[float, ...]) -> float:
    return max(abs(a - b) for a, b in zip(left, right, strict=True))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=Path)
    args = parser.parse_args()

    samples: list[dict[str, object]] = []
    end_line = ""
    for raw_line in args.report.read_text(encoding="utf-8").splitlines():
        line = raw_line.rstrip("\r")
        match = SAMPLE_RE.match(line)
        if match:
            item: dict[str, object] = {
                "index": int(match.group("index")),
                "elapsed": int(match.group("elapsed")),
            }
            for name in ("a_pos", "a_q", "b_pos", "b_q", "motion", "c_pos", "c_q"):
                item[name] = vector(match.group(name))
            samples.append(item)
        elif line.startswith("CAMERA_SHAPE_STATE_END "):
            end_line = line

    if not samples or not end_line:
        raise SystemExit("incomplete camera-shape report")

    comparisons = {
        "a_b_pos": ("a_pos", "b_pos"),
        "a_b_q": ("a_q", "b_q"),
        "a_c_pos": ("a_pos", "c_pos"),
        "a_c_q": ("a_q", "c_q"),
        "b_c_pos": ("b_pos", "c_pos"),
        "b_c_q": ("b_q", "c_q"),
    }
    maxima = {name: 0.0 for name in comparisons}
    exact = {name: 0 for name in comparisons}
    motion_max = 0.0
    quaternion_norm_error = {name: 0.0 for name in ("a_q", "b_q", "c_q")}
    for sample in samples:
        for name, (left_name, right_name) in comparisons.items():
            left = sample[left_name]
            right = sample[right_name]
            assert isinstance(left, tuple) and isinstance(right, tuple)
            delta = maximum_difference(left, right)
            maxima[name] = max(maxima[name], delta)
            exact[name] += int(left == right)
        motion = sample["motion"]
        assert isinstance(motion, tuple)
        motion_max = max(motion_max, *(abs(value) for value in motion))
        for name in quaternion_norm_error:
            quaternion = sample[name]
            assert isinstance(quaternion, tuple)
            norm = math.sqrt(sum(value * value for value in quaternion))
            quaternion_norm_error[name] = max(quaternion_norm_error[name], abs(norm - 1.0))

    first = samples[0]
    last = samples[-1]
    print(
        "CAMERA_SHAPE_ANALYSIS_V1"
        f" printed_samples={len(samples)}"
        f" first_index={first['index']} last_index={last['index']}"
        f" elapsed_ms={last['elapsed']} motion_max_abs={motion_max:.9g}"
    )
    for name in comparisons:
        print(
            f"CAMERA_SHAPE_COMPARISON pair={name}"
            f" max_abs_delta={maxima[name]:.9g}"
            f" exact_printed={exact[name]}/{len(samples)}"
        )
    for name, error in quaternion_norm_error.items():
        print(f"CAMERA_SHAPE_QUATERNION state={name} max_norm_error={error:.9g}")
    print(end_line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
