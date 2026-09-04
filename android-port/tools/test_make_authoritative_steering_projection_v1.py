#!/usr/bin/env python3
"""Offline tests for the authoritative steering-only source projection."""

from __future__ import annotations

import pathlib
import unittest

from make_authoritative_steering_projection_v1 import (
    NON_STEERING_BEGIN,
    NON_STEERING_END,
    PHYSICS_BEGIN,
    PHYSICS_END,
    PROJECTED_SKIP_MASK,
    STEERING_OFFSET,
    project,
)
from unified_tick_recording_v1 import FRAME_SIZE, HEADER_SIZE, decode_recording


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (
    WORKSPACE
    / "android-port"
    / "evidence"
    / "a9tas_action_until_release_20260817_224943_800.a9utk1"
).read_bytes()


class SteeringProjectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.projected, cls.details = project(SOURCE)
        cls.fixed, cls.frames = decode_recording(cls.projected)

    def test_full_bound_source_shape_is_preserved(self) -> None:
        self.assertEqual((len(self.frames), self.fixed), (344, 16667))
        self.assertEqual([frame.tick for frame in self.frames], list(range(344)))

    def test_real_nonzero_steering_is_preserved(self) -> None:
        self.assertEqual(self.details["nonzero_steering_frames"], 94)
        self.assertEqual(self.details["first_nonzero_steering_frame"], 250)
        self.assertEqual(self.details["last_nonzero_steering_frame"], 343)
        for index in range(344):
            base = HEADER_SIZE + index * FRAME_SIZE
            self.assertEqual(
                self.projected[base + STEERING_OFFSET:base + STEERING_OFFSET + 4],
                SOURCE[base + STEERING_OFFSET:base + STEERING_OFFSET + 4],
            )

    def test_physics_payload_is_byte_identical(self) -> None:
        for index in range(344):
            base = HEADER_SIZE + index * FRAME_SIZE
            self.assertEqual(
                self.projected[base + PHYSICS_BEGIN:base + PHYSICS_END],
                SOURCE[base + PHYSICS_BEGIN:base + PHYSICS_END],
            )

    def test_every_nonsteering_capability_is_skipped(self) -> None:
        self.assertTrue(all(frame.skip_flags == PROJECTED_SKIP_MASK for frame in self.frames))
        self.assertTrue(all(frame.brake == frame.accelerator == 0.0 for frame in self.frames))
        self.assertTrue(all(frame.nitro_activations == 0 and not frame.respawn for frame in self.frames))

    def test_only_declared_payload_range_can_change(self) -> None:
        changed = set(self.details["changed_frame_offsets"])
        self.assertTrue(changed)
        self.assertLessEqual(changed, set(range(NON_STEERING_BEGIN, NON_STEERING_END)))


if __name__ == "__main__":
    unittest.main()
