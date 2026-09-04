#!/usr/bin/env python3
"""Regression tests for A9UTK1 and upstream replay-packet semantics."""

from __future__ import annotations

import math
import struct
import unittest

from unified_tick_recording_v1 import (
    FRAME_SIZE,
    HEADER_SIZE,
    SKIP_ACCELERATOR,
    SKIP_BARREL_ANGULAR,
    SKIP_BARREL_RBX,
    SKIP_BRAKE,
    SKIP_NITRO,
    SKIP_RESPAWN,
    SKIP_STEER,
    SKIP_TRANSFORM,
    UnifiedTickFrameV1,
    decode_recording,
    encode_recording,
    plan_tick,
)


def floats(*values: float) -> bytes:
    return struct.pack(f"<{len(values)}f", *values)


def frame(tick: int = 9, *, skip_flags: int = 0) -> UnifiedTickFrameV1:
    return UnifiedTickFrameV1(
        tick=tick,
        monotonic_ns=1000 + tick,
        steering=0.25,
        brake=-0.5,
        accelerator=1.0,
        nitro_activations=2,
        skip_flags=skip_flags,
        respawn=True,
        barrel_angular=(1.0, 2.0, 3.0),
        barrel_rbx=(4.0, 5.0),
        transform=floats(*range(16)),
        linear_velocity=floats(6.0, 7.0, 8.0),
    )


class UnifiedTickRecordingTests(unittest.TestCase):
    def test_round_trip_fixed_abi(self) -> None:
        blob = encode_recording([frame()], fixed_interval_us=16667)
        self.assertEqual(len(blob), HEADER_SIZE + FRAME_SIZE)
        self.assertEqual(decode_recording(blob), (16667, (frame(),)))

    def test_contiguous_multi_frame_round_trip(self) -> None:
        frames = [frame(20), frame(21), frame(22)]
        self.assertEqual(decode_recording(encode_recording(frames, fixed_interval_us=8333))[1], tuple(frames))

    def test_rejects_tick_gap_and_time_reversal(self) -> None:
        with self.assertRaisesRegex(ValueError, "contiguous"):
            encode_recording([frame(1), frame(3)], fixed_interval_us=16667)
        later = frame(2)
        earlier = UnifiedTickFrameV1(**{**later.__dict__, "tick": 3, "monotonic_ns": 1})
        with self.assertRaisesRegex(ValueError, "backward"):
            encode_recording([later, earlier], fixed_interval_us=16667)

    def test_rejects_invalid_interval_nitro_and_nan(self) -> None:
        with self.assertRaisesRegex(ValueError, "fixed_interval"):
            encode_recording([frame()], fixed_interval_us=999)
        bad_nitro = UnifiedTickFrameV1(**{**frame().__dict__, "nitro_activations": 3})
        with self.assertRaisesRegex(ValueError, "nitro"):
            encode_recording([bad_nitro], fixed_interval_us=16667)
        bad_axis = UnifiedTickFrameV1(**{**frame().__dict__, "steering": math.nan})
        with self.assertRaisesRegex(ValueError, "control"):
            encode_recording([bad_axis], fixed_interval_us=16667)

    def test_rejects_unknown_skip_flag_trailing_and_reserved_data(self) -> None:
        unknown = UnifiedTickFrameV1(**{**frame().__dict__, "skip_flags": 0x100})
        with self.assertRaisesRegex(ValueError, "unknown"):
            encode_recording([unknown], fixed_interval_us=16667)
        blob = encode_recording([frame()], fixed_interval_us=16667)
        with self.assertRaisesRegex(ValueError, "exactly"):
            decode_recording(blob + b"x")
        changed = bytearray(blob)
        changed[80] = 1
        with self.assertRaisesRegex(ValueError, "reserved"):
            decode_recording(bytes(changed))

    def test_plan_preserves_all_original_packet_fields(self) -> None:
        current_transform = floats(99.0, *range(1, 16))
        current_linear = floats(6.0, 7.0, 8.0)
        plan = plan_tick(
            frame(),
            current_transform=current_transform,
            current_linear_velocity=current_linear,
        )
        self.assertEqual((plan.steering, plan.brake, plan.accelerator), (0.25, -0.5, 1.0))
        self.assertEqual(plan.nitro_activations, 2)
        self.assertTrue(plan.respawn)
        self.assertEqual(plan.barrel_angular, floats(1.0, 2.0, 3.0))
        self.assertEqual(plan.barrel_rbx, (4.0, 5.0))
        self.assertEqual([(item.native_offset, len(item.payload)) for item in plan.transform_linear_writes], [(0x10, 64), (0x150, 12)])

    def test_every_skip_flag_disables_only_its_original_field(self) -> None:
        skips = (
            SKIP_STEER
            | SKIP_BRAKE
            | SKIP_NITRO
            | SKIP_ACCELERATOR
            | SKIP_BARREL_ANGULAR
            | SKIP_BARREL_RBX
            | SKIP_RESPAWN
            | SKIP_TRANSFORM
        )
        plan = plan_tick(
            frame(skip_flags=skips),
            current_transform=bytes(64),
            current_linear_velocity=bytes(12),
        )
        self.assertIsNone(plan.steering)
        self.assertIsNone(plan.brake)
        self.assertIsNone(plan.accelerator)
        self.assertEqual(plan.nitro_activations, 0)
        self.assertFalse(plan.respawn)
        self.assertIsNone(plan.barrel_angular)
        self.assertIsNone(plan.barrel_rbx)
        self.assertEqual(plan.transform_linear_writes, ())

    def test_equal_transform_linear_plans_zero_writes(self) -> None:
        item = frame()
        plan = plan_tick(
            item,
            current_transform=item.transform,
            current_linear_velocity=item.linear_velocity,
        )
        self.assertEqual(plan.transform_linear_writes, ())

    def test_signed_zero_remains_component_equal(self) -> None:
        item = frame()
        recorded = struct.pack("<I", 0x80000000) + item.transform[4:]
        with_negative_zero = UnifiedTickFrameV1(**{**item.__dict__, "transform": recorded})
        current = struct.pack("<I", 0x00000000) + item.transform[4:]
        plan = plan_tick(
            with_negative_zero,
            current_transform=current,
            current_linear_velocity=item.linear_velocity,
        )
        self.assertEqual(plan.transform_linear_writes, ())


if __name__ == "__main__":
    unittest.main()
