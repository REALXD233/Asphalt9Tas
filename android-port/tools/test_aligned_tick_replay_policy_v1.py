#!/usr/bin/env python3
"""Static and numeric safety tests for the aligned replay gate."""

from __future__ import annotations

import pathlib
import struct
import unittest

from aligned_tick_replay_v1 import first_frame_aligned, first_frame_alignment_ratios


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_unified_tick_executor_v1.cpp").read_text(
    encoding="utf-8"
)
BUILD = (ROOT / "build-hwbp-aligned-tick-replay-v1.ps1").read_text(
    encoding="utf-8"
)


def payload(values: tuple[float, ...]) -> bytes:
    return struct.pack("<19f", *values)


class AlignedTickReplayPolicyTests(unittest.TestCase):
    def test_guard_accepts_small_first_frame_divergence(self) -> None:
        first = tuple(float(index) for index in range(19))
        second = tuple(value + 0.01 for value in first)
        current = tuple(value + 0.02 for value in first)
        self.assertTrue(first_frame_aligned(payload(current), payload(first), payload(second)))
        self.assertLessEqual(max(first_frame_alignment_ratios(payload(current), payload(first), payload(second))), 1.0)

    def test_guard_rejects_large_transform_and_linear_divergence(self) -> None:
        first = (0.0,) * 19
        second = (0.001,) * 19
        far_transform = (1.0,) + (0.0,) * 18
        far_linear = (0.0,) * 18 + (20.0,)
        self.assertFalse(first_frame_aligned(payload(far_transform), payload(first), payload(second)))
        self.assertFalse(first_frame_aligned(payload(far_linear), payload(first), payload(second)))

    def test_cpp_guard_precedes_first_final_payload_write(self) -> None:
        guard = SOURCE.index("first_frame_alignment_guard")
        pose_write = SOURCE.index("backend.native_pose_address", guard)
        self.assertLess(guard, pose_write)
        self.assertIn("machine.frame_index == 0", SOURCE[:guard])

    def test_guard_is_compile_time_isolated(self) -> None:
        self.assertGreaterEqual(SOURCE.count("#ifdef A9TAS_REPLAY_ALIGNMENT_GUARD_V1"), 3)
        self.assertIn('"-DA9TAS_REPLAY_ALIGNMENT_GUARD_V1"', BUILD)
        self.assertIn("a9tas_hwbp_aligned_tick_replay_v1", BUILD)


if __name__ == "__main__":
    unittest.main()
