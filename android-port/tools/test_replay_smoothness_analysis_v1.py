#!/usr/bin/env python3
"""Focused unit tests for correction-run and input-onset analysis."""

from __future__ import annotations

import struct
import unittest
from types import SimpleNamespace

from analyze_replay_smoothness_v1 import (
    analyze_report,
    consecutive_runs,
    first_control_frame,
    first_motion_frame,
    motion_onset_metrics,
)
from pathlib import Path


def frame(*, steering=0.0, brake=0.0, accelerator=0.0, nitro=0,
          respawn=False, velocity=(0.0, 0.0, 0.0)):
    return SimpleNamespace(
        steering=steering,
        brake=brake,
        accelerator=accelerator,
        nitro_activations=nitro,
        respawn=respawn,
        linear_velocity=struct.pack("<3f", *velocity),
    )


class ReplaySmoothnessAnalysisTests(unittest.TestCase):
    def test_consecutive_runs(self):
        self.assertEqual(
            consecutive_runs([2, 3, 4, 8, 10, 11]),
            [(2, 4), (8, 8), (10, 11)],
        )
        self.assertEqual(consecutive_runs([]), [])

    def test_motion_precedes_input(self):
        frames = [frame(), frame(velocity=(0.0, 0.0, 0.1)), frame(steering=0.2)]
        self.assertEqual(first_motion_frame(frames), 1)
        self.assertEqual(first_control_frame(frames), 2)

    def test_all_control_kinds(self):
        variants = (
            frame(steering=0.1), frame(brake=-1.0), frame(accelerator=1.0),
            frame(nitro=1), frame(respawn=True),
        )
        for item in variants:
            self.assertEqual(first_control_frame([frame(), item]), 1)

    def test_temporal_hold_is_distinct_from_spatial_snap(self):
        static = (0.0, 0.0, 0.0)
        moving = (0.0, 0.0, 0.3)
        metrics = motion_onset_metrics(
            [static] * 8 + [moving] * 4,
            [static] * 2 + [moving] * 10,
        )
        self.assertEqual(metrics["first_native_motion_frame"], 2)
        self.assertEqual(metrics["first_target_motion_frame"], 8)
        self.assertEqual(metrics["motion_onset_lag_frames"], 6)
        self.assertEqual(metrics["temporal_hold_frames"], 6)
        self.assertEqual(metrics["longest_temporal_hold_run"], 6)
        self.assertTrue(metrics["temporal_hold_risk"])

    def test_short_onset_difference_is_not_flagged(self):
        static = (0.0, 0.0, 0.0)
        moving = (0.3, 0.0, 0.0)
        metrics = motion_onset_metrics(
            [static, static, moving],
            [static, moving, moving],
        )
        self.assertEqual(metrics["motion_onset_lag_frames"], 1)
        self.assertEqual(metrics["temporal_hold_frames"], 1)
        self.assertFalse(metrics["temporal_hold_risk"])

    def test_velocity_count_mismatch_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "count mismatch"):
            motion_onset_metrics([(0.0, 0.0, 0.0)], [])

    def test_live_a9uer8_report_is_supported(self):
        report = (
            Path(__file__).resolve().parents[1] / "evidence" /
            "a9tas_final_writer_30f_20260821_160947_897.a9uer8"
        )
        if not report.is_file():
            self.skipTest("bounded live A9UER8 evidence is not present")
        result = analyze_report(report.read_bytes(), 30)
        self.assertEqual(result["equal_frames"], 29)
        self.assertEqual(result["corrected_frames"], 1)


if __name__ == "__main__":
    unittest.main()
