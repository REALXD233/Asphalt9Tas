#!/usr/bin/env python3
"""Regression tests for the exact native physics recording ABI."""

from __future__ import annotations

import math
import struct
import unittest

from native_physics_recording_v1 import (
    FRAME_SIZE,
    HEADER_SIZE,
    NativePhysicsFrameV1,
    decode_recording,
    encode_recording,
    plan_frame_correction,
    validate_runtime_safe,
)


def floats(*values: float) -> bytes:
    return struct.pack(f"<{len(values)}f", *values)


class NativePhysicsRecordingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.transform = floats(*range(16))
        self.linear = floats(1.25, -2.5, 3.75)
        self.frame = NativePhysicsFrameV1(
            tick=41,
            monotonic_ns=123_456_789,
            transform=self.transform,
            linear_velocity=self.linear,
        )

    def test_round_trip_has_fixed_64_plus_96n_abi(self) -> None:
        blob = encode_recording([self.frame])
        self.assertEqual(len(blob), HEADER_SIZE + FRAME_SIZE)
        self.assertEqual(decode_recording(blob), (self.frame,))

    def test_raw_nan_and_signed_zero_bits_survive_codec(self) -> None:
        raw_transform = struct.pack("<I", 0x7FC01234) + struct.pack(
            "<15I", 0x80000000, *range(1, 15)
        )
        frame = NativePhysicsFrameV1(0, 1, raw_transform, self.linear)
        decoded = decode_recording(encode_recording([frame]))[0]
        self.assertEqual(decoded.transform, raw_transform)

    def test_runtime_validation_rejects_non_finite_payload(self) -> None:
        bad = NativePhysicsFrameV1(
            0, 1, floats(math.nan, *range(1, 16)), self.linear
        )
        with self.assertRaisesRegex(ValueError, "non-finite"):
            validate_runtime_safe([bad])

    def test_runtime_validation_accepts_contiguous_monotonic_trace(self) -> None:
        frames = [
            NativePhysicsFrameV1(8, 100, self.transform, self.linear),
            NativePhysicsFrameV1(9, 100, self.transform, self.linear),
            NativePhysicsFrameV1(10, 101, self.transform, self.linear),
        ]
        validate_runtime_safe(frames)

    def test_runtime_validation_rejects_tick_gap(self) -> None:
        frames = [self.frame, NativePhysicsFrameV1(43, 200, self.transform, self.linear)]
        with self.assertRaisesRegex(ValueError, "non-contiguous"):
            validate_runtime_safe(frames)

    def test_rejects_wrong_exact_build_id(self) -> None:
        blob = bytearray(encode_recording([self.frame]))
        blob[24] ^= 0xFF
        with self.assertRaisesRegex(ValueError, "build ID"):
            decode_recording(bytes(blob))

    def test_rejects_trailing_or_truncated_data(self) -> None:
        blob = encode_recording([self.frame])
        for malformed in (blob[:-1], blob + b"x"):
            with self.assertRaisesRegex(ValueError, "length"):
                decode_recording(malformed)

    def test_rejects_layout_substitution(self) -> None:
        blob = bytearray(encode_recording([self.frame]))
        struct.pack_into("<I", blob, 44, 0x14)
        with self.assertRaisesRegex(ValueError, "layout"):
            decode_recording(bytes(blob))

    def test_frame_feeds_exact_all_or_nothing_planner(self) -> None:
        recorded = NativePhysicsFrameV1(
            0,
            1,
            floats(99.0, *range(1, 16)),
            self.linear,
        )
        plan = plan_frame_correction(self.transform, self.linear, recorded)
        self.assertEqual([(item.native_offset, len(item.payload)) for item in plan], [(0x10, 64), (0x150, 12)])


if __name__ == "__main__":
    unittest.main()
