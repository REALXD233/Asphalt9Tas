#!/usr/bin/env python3
"""Synthetic positive/negative tests for the A9SBT1 report parser."""

from __future__ import annotations

import math
import struct
import tempfile
import unittest
from pathlib import Path

from parse_same_bytes_audit_v1 import (
    HEADER,
    MAGIC,
    REQUIRED_FLAGS,
    SNAPSHOT_SIZE,
    SUPPORTED_BUILD_ID,
    VERSION,
    assess,
    read_audit,
)


def snapshot() -> bytes:
    data = bytearray(SNAPSHOT_SIZE)
    transform = struct.pack("<16f", *([1.0] * 16))
    linear = struct.pack("<3f", 1.0, 2.0, 3.0)
    data[0x10:0x50] = transform
    data[0x150:0x15C] = linear
    return bytes(data)


def report_blob(*, flags: int = REQUIRED_FLAGS, errors: int = 0) -> bytes:
    native = 0x70000000
    header = HEADER.pack(
        MAGIC,
        VERSION,
        HEADER.size,
        SNAPSHOT_SIZE,
        flags,
        SUPPORTED_BUILD_ID,
        0,
        123,
        0x10000000,
        0x20000000,
        0x30000000,
        0x40000000,
        native,
        native + 0x10,
        native + 0x150,
        14,
        errors,
        0,
        0,
        1,
        0,
        2,
        10,
        12,
    )
    snap = snapshot()
    return header + snap + snap + snap


class SameBytesAuditParserTests(unittest.TestCase):
    def _read(self, blob: bytes):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "audit.bin"
            path.write_bytes(blob)
            return read_audit(path)

    def test_accepts_exact_clean_report(self) -> None:
        header, before, immediate, next_cycle = self._read(report_blob())
        self.assertEqual(assess(header, before, immediate, next_cycle), [])

    def test_rejects_any_immediate_side_change(self) -> None:
        blob = bytearray(report_blob())
        blob[HEADER.size + SNAPSHOT_SIZE + 0x15C] ^= 1
        header, before, immediate, next_cycle = self._read(bytes(blob))
        self.assertTrue(
            any("immediate audit changed" in item for item in assess(header, before, immediate, next_cycle))
        )

    def test_rejects_nonzero_transport_error(self) -> None:
        header, before, immediate, next_cycle = self._read(report_blob(errors=1))
        self.assertTrue(any("nonzero errors" in item for item in assess(header, before, immediate, next_cycle)))

    def test_rejects_nonfinite_next_payload(self) -> None:
        blob = bytearray(report_blob())
        next_start = HEADER.size + 2 * SNAPSHOT_SIZE
        blob[next_start + 0x10 : next_start + 0x14] = struct.pack("<f", math.nan)
        header, before, immediate, next_cycle = self._read(bytes(blob))
        self.assertTrue(any("next-cycle payload" in item for item in assess(header, before, immediate, next_cycle)))

    def test_rejects_trailing_data(self) -> None:
        with self.assertRaisesRegex(ValueError, "exactly"):
            self._read(report_blob() + b"x")


if __name__ == "__main__":
    unittest.main()
