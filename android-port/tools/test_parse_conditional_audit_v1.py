#!/usr/bin/env python3
"""Synthetic tests for the A9CDT1 conditional audit parser."""

from __future__ import annotations

import struct
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from parse_conditional_audit_v1 import (
    FRAME_AUDIT_EXACT,
    FRAME_CORRECTED,
    FRAME_EQUAL,
    FRAME_PREFIX,
    FRAME_SIZE,
    HEADER,
    MAGIC,
    REQUIRED_HEADER_FLAGS,
    SNAPSHOT_SIZE,
    SUPPORTED_BUILD_ID,
    VERSION,
    assess,
    read_report,
)


def payload(value: float) -> tuple[bytes, bytes]:
    return struct.pack("<16f", *([value] * 16)), struct.pack("<3f", value, value, value)


def audit_frame(tick: int, *, corrected: bool) -> bytes:
    before = bytearray(SNAPSHOT_SIZE)
    current_transform, current_linear = payload(1.0)
    recorded_transform, recorded_linear = payload(2.0 if corrected else 1.0)
    before[0x10:0x50] = current_transform
    before[0x150:0x15C] = current_linear
    immediate = bytearray(before)
    flags = FRAME_AUDIT_EXACT | (FRAME_CORRECTED if corrected else FRAME_EQUAL)
    if corrected:
        immediate[0x10:0x50] = recorded_transform
        immediate[0x150:0x15C] = recorded_linear
    prefix = FRAME_PREFIX.pack(
        tick,
        1000 + tick,
        flags,
        0,
        recorded_transform,
        recorded_linear,
    )
    return prefix + before + immediate


def report_blob() -> bytes:
    native = 0x70000000
    frames = audit_frame(10, corrected=False) + audit_frame(11, corrected=True)
    header = HEADER.pack(
        MAGIC,
        VERSION,
        HEADER.size,
        FRAME_SIZE,
        REQUIRED_HEADER_FLAGS,
        2,
        2,
        1,
        1,
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
        20,
        0,
        0,
        0,
        1,
        0,
        0,
        0,
        3,
        10,
        13,
    )
    return header + frames


class ConditionalAuditParserTests(unittest.TestCase):
    def _read(self, blob: bytes):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.bin"
            path.write_bytes(blob)
            return read_report(path)

    def test_accepts_equal_and_corrected_frames(self) -> None:
        header, frames = self._read(report_blob())
        self.assertEqual(assess(header, frames), [])

    def test_rejects_side_effect_outside_payload(self) -> None:
        blob = bytearray(report_blob())
        second_immediate = HEADER.size + FRAME_SIZE + FRAME_PREFIX.size + SNAPSHOT_SIZE
        blob[second_immediate + 0x15C] ^= 1
        header, frames = self._read(bytes(blob))
        self.assertTrue(any("snapshot byte 348" in item for item in assess(header, frames)))

    def test_rejects_wrong_comparator_flag(self) -> None:
        blob = bytearray(report_blob())
        first_flags = HEADER.size + 16
        struct.pack_into("<I", blob, first_flags, FRAME_CORRECTED | FRAME_AUDIT_EXACT)
        header, frames = self._read(bytes(blob))
        self.assertTrue(any("comparator result" in item for item in assess(header, frames)))

    def test_rejects_tick_gap(self) -> None:
        blob = bytearray(report_blob())
        second_tick = HEADER.size + FRAME_SIZE
        struct.pack_into("<Q", blob, second_tick, 12)
        header, frames = self._read(bytes(blob))
        self.assertTrue(any("not contiguous" in item for item in assess(header, frames)))

    def test_rejects_nonzero_rollback_counter(self) -> None:
        header, frames = self._read(report_blob())
        header = replace(header, rollback_attempts=1)
        self.assertTrue(any("rollback counters" in item for item in assess(header, frames)))

    def test_can_require_multiple_corrected_frames(self) -> None:
        header, frames = self._read(report_blob())
        self.assertTrue(
            any(
                "corrected frames" in item
                for item in assess(header, frames, minimum_corrected=2)
            )
        )

    def test_can_enforce_live_frame_cap(self) -> None:
        header, frames = self._read(report_blob())
        self.assertTrue(
            any(
                "maximum" in item
                for item in assess(header, frames, maximum_frames=1)
            )
        )

    def test_rejects_trailing_data(self) -> None:
        with self.assertRaisesRegex(ValueError, "exactly"):
            self._read(report_blob() + b"x")


if __name__ == "__main__":
    unittest.main()
