#!/usr/bin/env python3
"""Offline regressions for lifecycle steer/drift action coverage."""

from __future__ import annotations

import pathlib
import struct
import unittest

from lifecycle_source_recording_v1 import MAGIC, RACE_LIFECYCLE_FLAG, VERSION
from lifecycle_steer_drift_recording_v1 import (
    verify_lifecycle_steer_drift_capture,
)
from synchronized_brake_recording_v1 import REQUIRED_FLAGS
from synchronized_tick_recording_v1 import _HEADER


ROOT = pathlib.Path(__file__).resolve().parents[1]
ACTION_REPORT = (
    ROOT / "evidence" /
    "a9tas_action_until_release_20260821_091052_694.a9usr4"
)
ACTION_RECORDING = (
    ROOT / "evidence" /
    "a9tas_action_until_release_20260821_091052_694.a9utk1"
)
NEUTRAL_REPORT = (
    ROOT / "evidence" /
    "a9tas_lifecycle_source_360f_20260821_195014_433.a9usr5"
)
NEUTRAL_RECORDING = (
    ROOT / "evidence" /
    "a9tas_lifecycle_source_360f_20260821_195014_433.a9utk1"
)


def action_lifecycle_envelope() -> bytes:
    blob = bytearray(ACTION_REPORT.read_bytes())
    header = _HEADER.unpack_from(blob)
    captured_frames = header[6]
    blob[:8] = MAGIC
    struct.pack_into("<I", blob, 8, VERSION)
    struct.pack_into("<I", blob, 20, REQUIRED_FLAGS | RACE_LIFECYCLE_FLAG)
    # A9USR4 stores a maximum frame cap. A9USR5 stores the exact target.
    struct.pack_into("<I", blob, 24, captured_frames)
    return bytes(blob)


class LifecycleSteerDriftRecordingTests(unittest.TestCase):
    def test_action_capture_has_authoritative_overlap_and_release(self) -> None:
        result = verify_lifecycle_steer_drift_capture(
            action_lifecycle_envelope(), ACTION_RECORDING.read_bytes())
        self.assertEqual(result.frames, 438)
        self.assertGreater(result.steer_frames, 0)
        self.assertGreater(result.brake_frames, 0)
        self.assertGreater(result.overlap_frames, 0)
        self.assertGreater(result.brake_release_tick, result.first_brake_tick)

    def test_neutral_lifecycle_baseline_is_not_mislabeled_as_action(self) -> None:
        with self.assertRaisesRegex(ValueError, "steering action"):
            verify_lifecycle_steer_drift_capture(
                NEUTRAL_REPORT.read_bytes(), NEUTRAL_RECORDING.read_bytes())


if __name__ == "__main__":
    unittest.main()
