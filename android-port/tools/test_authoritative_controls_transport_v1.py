#!/usr/bin/env python3
"""Offline semantics for raw steering+brake authoritative pair transport."""
from __future__ import annotations

import math
import pathlib
import struct
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "android-port/src/authoritative_controls_transport_v1.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "android-port/src/authoritative_controls_transport_v1.h").read_text(encoding="utf-8")


def bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def plan(steering: float, brake: float, live: int) -> int:
    if not math.isfinite(steering) or abs(steering) > 1.0:
        raise ValueError("steering")
    if not math.isfinite(brake) or abs(brake) > 1.05:
        raise ValueError("brake")
    return (bits(steering) << 32) | bits(brake)


class AuthoritativeControlsTransportTests(unittest.TestCase):
    def test_both_halves_come_from_recording(self) -> None:
        intended = plan(-0.75, -1.0, 0x123456783F000000)
        self.assertEqual(intended >> 32, bits(-0.75))
        self.assertEqual(intended & 0xFFFFFFFF, bits(-1.0))

    def test_c98_and_c9c_converge_despite_different_live_pairs(self) -> None:
        self.assertEqual(
            plan(0.5, 0.0, 0x11111111BF800000),
            plan(0.5, 0.0, 0x222222223F800000),
        )

    def test_zero_is_an_enabled_control_write(self) -> None:
        self.assertEqual(plan(0.0, 0.0, 0xDEADBEEFCAFEBABE), 0)

    def test_invalid_control_values_fail(self) -> None:
        for steering, brake in ((1.1, 0.0), (0.0, 1.1), (math.nan, 0.0)):
            with self.assertRaises(ValueError):
                plan(steering, brake, 0)

    def test_cpp_core_is_pure_and_requires_exact_skip_scope(self) -> None:
        combined = SOURCE + HEADER
        self.assertIn("kSteeringBrakeSkipMask", combined)
        self.assertIn("frame.skip_override_flags != kSteeringBrakeSkipMask", combined)
        self.assertIn("plan.brake_bits", combined)
        for forbidden in ("ptrace(", "pwrite", "process_vm_writev", "socket(", "adb"):
            self.assertNotIn(forbidden, combined)


if __name__ == "__main__":
    unittest.main()
