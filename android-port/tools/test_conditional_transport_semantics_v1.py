#!/usr/bin/env python3
"""Regression tests for conditional correction immediate-audit policy."""

from __future__ import annotations

import struct
import unittest

from conditional_transport_semantics_v1 import (
    AUDIT_SNAPSHOT_SIZE,
    expected_immediate_audit,
    verify_conditional_immediate,
)


def floats(*values: float) -> bytes:
    return struct.pack(f"<{len(values)}f", *values)


class ConditionalTransportTests(unittest.TestCase):
    def setUp(self) -> None:
        self.before = bytearray(AUDIT_SNAPSHOT_SIZE)
        self.transform = floats(*range(16))
        self.linear = floats(1.0, 2.0, 3.0)
        self.before[0x10:0x50] = self.transform
        self.before[0x150:0x15C] = self.linear

    def test_equal_packet_produces_zero_write_expectation(self) -> None:
        expected, corrected = expected_immediate_audit(
            bytes(self.before), self.transform, self.linear
        )
        self.assertFalse(corrected)
        self.assertEqual(expected, self.before)

    def test_signed_zero_remains_zero_write_like_source(self) -> None:
        current = floats(0.0, *range(1, 16))
        recorded = floats(-0.0, *range(1, 16))
        self.before[0x10:0x50] = current
        expected, corrected = expected_immediate_audit(
            bytes(self.before), recorded, self.linear
        )
        self.assertFalse(corrected)
        self.assertEqual(expected[0x10:0x50], current)

    def test_transform_mismatch_replaces_both_ranges_only(self) -> None:
        recorded_transform = floats(99.0, *range(1, 16))
        expected, corrected = expected_immediate_audit(
            bytes(self.before), recorded_transform, self.linear
        )
        self.assertTrue(corrected)
        self.assertEqual(expected[0x10:0x50], recorded_transform)
        self.assertEqual(expected[0x150:0x15C], self.linear)
        self.assertEqual(expected[:0x10], self.before[:0x10])
        self.assertEqual(expected[0x50:0x150], self.before[0x50:0x150])
        self.assertEqual(expected[0x15C:], self.before[0x15C:])

    def test_linear_mismatch_also_replaces_both_ranges(self) -> None:
        recorded_linear = floats(4.0, 5.0, 6.0)
        expected, corrected = expected_immediate_audit(
            bytes(self.before), self.transform, recorded_linear
        )
        self.assertTrue(corrected)
        self.assertEqual(expected[0x10:0x50], self.transform)
        self.assertEqual(expected[0x150:0x15C], recorded_linear)

    def test_rejects_side_effect_outside_allowed_ranges(self) -> None:
        expected, _ = expected_immediate_audit(
            bytes(self.before), floats(99.0, *range(1, 16)), self.linear
        )
        immediate = bytearray(expected)
        immediate[0x15C] ^= 1
        with self.assertRaisesRegex(ValueError, "byte 348"):
            verify_conditional_immediate(expected, bytes(immediate))

    def test_rejects_wrong_audit_size(self) -> None:
        with self.assertRaisesRegex(ValueError, "exactly 804"):
            expected_immediate_audit(
                bytes(self.before[:-1]), self.transform, self.linear
            )


if __name__ == "__main__":
    unittest.main()
