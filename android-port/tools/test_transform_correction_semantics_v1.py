#!/usr/bin/env python3
"""Regression tests for the side-effect-free final-correction planner."""

from __future__ import annotations

import math
import struct
import unittest

from transform_correction_semantics_v1 import (
    LINEAR_VELOCITY_OFFSET,
    TRANSFORM_OFFSET,
    plan_transform_linear_correction,
)


def floats(*values: float) -> bytes:
    return struct.pack(f"<{len(values)}f", *values)


class TransformCorrectionSemanticsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.transform = floats(*range(16))
        self.linear = floats(1.25, -2.5, 3.75)

    def test_equal_packet_plans_no_write(self) -> None:
        self.assertEqual(
            plan_transform_linear_correction(
                self.transform, self.linear, self.transform, self.linear
            ),
            (),
        )

    def test_transform_mismatch_selects_both_exact_ranges(self) -> None:
        recorded_transform = floats(99.0, *range(1, 16))
        plan = plan_transform_linear_correction(
            self.transform, self.linear, recorded_transform, self.linear
        )
        self.assertEqual([item.native_offset for item in plan], [0x10, 0x150])
        self.assertEqual(plan[0].payload, recorded_transform)
        self.assertEqual(plan[1].payload, self.linear)

    def test_linear_mismatch_also_selects_both_exact_ranges(self) -> None:
        recorded_linear = floats(1.25, -2.5, 4.0)
        plan = plan_transform_linear_correction(
            self.transform, self.linear, self.transform, recorded_linear
        )
        self.assertEqual(
            [(item.native_offset, len(item.payload)) for item in plan],
            [(TRANSFORM_OFFSET, 64), (LINEAR_VELOCITY_OFFSET, 12)],
        )

    def test_signed_zero_is_equal_like_cpp_float_equality(self) -> None:
        current = floats(0.0, *range(1, 16))
        recorded = floats(-0.0, *range(1, 16))
        self.assertEqual(
            plan_transform_linear_correction(current, self.linear, recorded, self.linear),
            (),
        )

    def test_nan_is_a_mismatch_even_with_the_same_payload(self) -> None:
        transform_with_nan = floats(math.nan, *range(1, 16))
        plan = plan_transform_linear_correction(
            transform_with_nan,
            self.linear,
            transform_with_nan,
            self.linear,
        )
        self.assertEqual(len(plan), 2)

    def test_rejects_inexact_byte_sizes(self) -> None:
        with self.assertRaisesRegex(ValueError, "exactly 64 bytes"):
            plan_transform_linear_correction(
                self.transform[:-1], self.linear, self.transform, self.linear
            )


if __name__ == "__main__":
    unittest.main()
