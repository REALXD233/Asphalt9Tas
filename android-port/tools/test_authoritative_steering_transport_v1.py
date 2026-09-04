#!/usr/bin/env python3
"""Offline semantics tests for the authoritative steering-pair transport."""

from __future__ import annotations

import pathlib
import struct
import unittest

from unified_tick_recording_v1 import decode_recording


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
RECORDING = (
    WORKSPACE
    / "android-port"
    / "evidence"
    / "a9tas_authoritative_steering_projection_344f_20260820.a9utk1"
)
SOURCE = (
    WORKSPACE
    / "android-port"
    / "src"
    / "authoritative_steering_transport_v1.cpp"
).read_text(encoding="utf-8")
HEADER = (
    WORKSPACE
    / "android-port"
    / "src"
    / "authoritative_steering_transport_v1.h"
).read_text(encoding="utf-8")


def raw_f32(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def plan_pair(steering: float, live_pair: int) -> int:
    return (raw_f32(steering) << 32) | (live_pair & 0xFFFFFFFF)


class AuthoritativeSteeringTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixed_us, cls.frames = decode_recording(RECORDING.read_bytes())

    def test_projection_is_strict_steering_only(self) -> None:
        self.assertEqual((len(self.frames), self.fixed_us), (344, 16667))
        self.assertTrue(all(frame.skip_flags == 0xFE for frame in self.frames))
        for frame in self.frames:
            self.assertEqual(raw_f32(frame.brake), 0)
            self.assertEqual(raw_f32(frame.accelerator), 0)
            self.assertEqual(frame.nitro_activations, 0)
            self.assertFalse(frame.respawn)
            self.assertTrue(all(raw_f32(value) == 0 for value in frame.barrel_angular))
            self.assertTrue(all(raw_f32(value) == 0 for value in frame.barrel_rbx))

    def test_every_packet_preserves_each_boundary_brake_bits(self) -> None:
        for index, frame in enumerate(self.frames):
            c98_before = (0xDEADBEEF << 32) | ((index * 0x10201) & 0xFFFFFFFF)
            c9c_before = (0xA5A5A5A5 << 32) | ((~index) & 0xFFFFFFFF)
            c98_after = plan_pair(frame.steering, c98_before)
            c9c_after = plan_pair(frame.steering, c9c_before)
            self.assertEqual(c98_after & 0xFFFFFFFF, c98_before & 0xFFFFFFFF)
            self.assertEqual(c9c_after & 0xFFFFFFFF, c9c_before & 0xFFFFFFFF)
            self.assertEqual(c98_after >> 32, raw_f32(frame.steering))
            self.assertEqual(c9c_after >> 32, raw_f32(frame.steering))

    def test_real_nonzero_sequence_is_not_weakened(self) -> None:
        indices = [index for index, frame in enumerate(self.frames) if raw_f32(frame.steering) != 0]
        self.assertEqual(len(indices), 94)
        self.assertEqual((indices[0], indices[-1]), (250, 343))

    def test_zero_steering_is_still_an_enabled_packet_write(self) -> None:
        frame = self.frames[0]
        self.assertEqual(frame.skip_flags & 1, 0)
        self.assertEqual(raw_f32(frame.steering), 0)
        self.assertEqual(plan_pair(frame.steering, 0xFFFFFFFF12345678), 0x12345678)

    def test_cpp_contract_is_narrow_and_dual_boundary(self) -> None:
        combined = SOURCE + HEADER
        self.assertIn("frame.skip_override_flags != kSteeringOnlySkipMask", combined)
        self.assertIn("live_pair_before & 0xffffffffULL", combined)
        self.assertIn("c98.steering_bits != c9c.steering_bits", combined)
        for forbidden in ("pwrite(", "ptrace(", "process_vm_", "RemoteCall"):
            self.assertNotIn(forbidden, combined)


if __name__ == "__main__":
    unittest.main()

