#!/usr/bin/env python3
"""Byte-preservation tests for controls-only source projection."""
from __future__ import annotations

import pathlib
import unittest

from make_authoritative_controls_projection_v1 import (
    PROJECTED_SKIP_MASK,
    SKIP_OFFSET,
    STEERING_BRAKE_BEGIN,
    STEERING_BRAKE_END,
    project,
)
from unified_tick_recording_v1 import FRAME_SIZE, HEADER_SIZE, decode_recording

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "android-port/evidence/a9tas_action_until_release_20260821_091052_694.a9utk1").read_bytes()


class ControlsProjectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.projected, cls.details = project(SOURCE)
        cls.fixed, cls.frames = decode_recording(cls.projected)

    def test_shape_and_ticks_are_preserved(self) -> None:
        self.assertEqual((len(self.frames), self.fixed), (438, 16667))
        self.assertEqual([frame.tick for frame in self.frames], list(range(438)))

    def test_raw_steering_and_brake_bits_are_preserved(self) -> None:
        for index in range(438):
            base = HEADER_SIZE + index * FRAME_SIZE
            self.assertEqual(
                self.projected[base + STEERING_BRAKE_BEGIN:base + STEERING_BRAKE_END],
                SOURCE[base + STEERING_BRAKE_BEGIN:base + STEERING_BRAKE_END],
            )

    def test_only_skip_field_changes(self) -> None:
        self.assertTrue(self.details["changed_frame_offsets"])
        self.assertLessEqual(
            set(self.details["changed_frame_offsets"]),
            set(range(SKIP_OFFSET, SKIP_OFFSET + 4)),
        )
        self.assertTrue(all(frame.skip_flags == PROJECTED_SKIP_MASK for frame in self.frames))

    def test_real_action_window_is_retained(self) -> None:
        self.assertEqual(self.details["nonzero_steering_frames"], 33)
        self.assertGreater(self.details["nonzero_brake_frames"], 0)


if __name__ == "__main__":
    unittest.main()
