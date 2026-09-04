#!/usr/bin/env python3
"""Synthetic offline test for RaceView phase trace analysis."""

from __future__ import annotations

import importlib.util
import pathlib
import sys


SCRIPT = pathlib.Path(__file__).with_name("analyze_camera_raceview_phase_v1.py")
SPEC = importlib.util.spec_from_file_location("raceview_phase", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def line(index: int, manager_x: float, shape_x: float) -> str:
    mask = 7 if index == 0 else 3
    return (
        f"RACEVIEW_PHASE sample={index} t_us={index * 5000} mask={mask} "
        f"mp={manager_x},0,0 mq=0,0,0,1 "
        f"sp={shape_x},0,0 sq=0,0,0,1 fov=1\n"
    )


def main() -> int:
    # Shape velocity is the manager velocity delayed by exactly two samples.
    manager = [0.0, 1.0, 3.0, 6.0, 10.0, 15.0, 21.0, 28.0]
    final_shape = [0.0, 0.0] + manager[:-2]
    samples = MODULE.parse_samples("".join(
        line(index, manager[index], final_shape[index])
        for index in range(len(manager))
    ))
    report = MODULE.analyze(samples)
    assert report["samples"] == len(manager)
    assert report["best_shape_velocity_shift"]["samples"] == 2
    assert report["best_shape_velocity_shift"]["score"] > 0.999
    assert report["manager_without_shape"] == 0
    assert report["shape_without_manager"] == 0
    assert report["device_access"] == 0
    assert report["gameplay_writes"] == 0
    print("CAMERA_RACEVIEW_PHASE_ANALYSIS passed=1 synthetic_shift=2 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
