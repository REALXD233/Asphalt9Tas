#!/usr/bin/env python3
"""Regression tests for the same-bytes-only transport policy."""

from __future__ import annotations

import unittest

from same_bytes_transport_semantics_v1 import (
    LINEAR_OFFSET,
    NATIVE_BODY_SIZE,
    TRANSFORM_OFFSET,
    plan_same_bytes_transport,
    verify_immediate_audit,
)


class SameBytesTransportTests(unittest.TestCase):
    def setUp(self) -> None:
        self.native = bytes((index * 37 + 11) & 0xFF for index in range(NATIVE_BODY_SIZE))

    def test_only_exact_transform_and_linear_ranges_are_planned(self) -> None:
        writes = plan_same_bytes_transport(self.native)
        self.assertEqual(
            [(item.native_offset, len(item.payload)) for item in writes],
            [(TRANSFORM_OFFSET, 64), (LINEAR_OFFSET, 12)],
        )
        self.assertEqual(writes[0].payload, self.native[0x10:0x50])
        self.assertEqual(writes[1].payload, self.native[0x150:0x15C])

    def test_planned_payloads_are_detached_exact_copies(self) -> None:
        mutable = bytearray(self.native)
        writes = plan_same_bytes_transport(mutable)
        mutable[0x10] ^= 0xFF
        self.assertNotEqual(writes[0].payload[0], mutable[0x10])

    def test_rejects_inexact_native_body_size(self) -> None:
        with self.assertRaisesRegex(ValueError, "exactly 656 bytes"):
            plan_same_bytes_transport(self.native[:-1])

    def test_accepts_byte_identical_full_audit(self) -> None:
        verify_immediate_audit(self.native, bytes(self.native))

    def test_rejects_change_inside_payload(self) -> None:
        changed = bytearray(self.native)
        changed[0x20] ^= 1
        with self.assertRaisesRegex(ValueError, "byte 32"):
            verify_immediate_audit(self.native, bytes(changed))

    def test_rejects_change_outside_payload(self) -> None:
        changed = bytearray(self.native)
        changed[0x15C] ^= 1
        with self.assertRaisesRegex(ValueError, "byte 348"):
            verify_immediate_audit(self.native, bytes(changed))


if __name__ == "__main__":
    unittest.main()
