#!/usr/bin/env python3
"""Cross-binding and source policy for authoritative start-line controls."""
from __future__ import annotations

import pathlib
import unittest

from validate_authoritative_controls_startline_v1 import make_selftest, validate

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "android-port/src/hwbp_authoritative_steering_v1.cpp").read_text(encoding="utf-8")
BUILD = (ROOT / "android-port/build-authoritative-controls-startline-v1.ps1").read_text(encoding="utf-8")


class AuthoritativeControlsStartlineTests(unittest.TestCase):
    def test_synthetic_report_cross_binds_both_controls(self) -> None:
        report, recording = make_selftest()
        result = validate(report, recording)
        self.assertEqual(result["frames"], 7)
        self.assertEqual(result["pair_writes"], 14)
        self.assertEqual(result["nonzero_brake"], 3)

    def test_one_boundary_tamper_is_rejected(self) -> None:
        report, recording = make_selftest()
        changed = bytearray(report)
        changed[-8] ^= 1
        with self.assertRaises(ValueError):
            validate(bytes(changed), recording)

    def test_source_selects_controls_transport_only_in_controls_variant(self) -> None:
        self.assertIn("A9TAS_AUTHORITATIVE_CONTROLS_STARTLINE_DIRECT", SOURCE)
        self.assertIn("authoritative_controls_v1::FrameIsSteeringBrake", SOURCE)
        self.assertIn("authoritative_controls_v1::PlanPair", SOURCE)
        self.assertIn("I_ACCEPT_STARTLINE_DIRECT_DELTA_AND_2X_STEERING_BRAKE_WRITES_V1", SOURCE)

    def test_build_enables_startline_and_controls_together(self) -> None:
        self.assertIn("-DA9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT=1", BUILD)
        self.assertIn("-DA9TAS_AUTHORITATIVE_CONTROLS_STARTLINE_DIRECT=1", BUILD)


if __name__ == "__main__":
    unittest.main()
