#!/usr/bin/env python3
"""Tests for the strict A9UER1 report verifier."""

from __future__ import annotations

import struct
import unittest

from parse_unified_executor_report_v1 import (
    AUDIT_EXACT,
    BUILD_ID,
    COMMITTED,
    CORRECTION_CORRECTED,
    CORRECTION_EQUAL,
    CORRECTION_SKIPPED,
    FRAME_SIZE,
    GATE2_COMPLETE,
    HEADER_SIZE,
    MAGIC,
    NATIVE_SIZE,
    STEERING_APPLIED,
    VERSION,
    _FRAME,
    _HEADER,
    decode_report,
)


def payload(seed: float) -> bytes:
    return struct.pack("<19f", *(seed + index for index in range(19)))


def snapshot(state: bytes) -> bytes:
    result = bytearray(804)
    result[0x10:0x50] = state[:64]
    result[0x150:0x15C] = state[64:]
    return bytes(result)


def make_report(modes: tuple[int, ...] = (CORRECTION_EQUAL, CORRECTION_CORRECTED, CORRECTION_SKIPPED)) -> bytes:
    frames = []
    equal = corrected = skipped = steering = 0
    event = 0
    for index, mode in enumerate(modes):
        current = payload(1.0 + index * 40)
        recorded = current if mode == CORRECTION_EQUAL else payload(20.0 + index * 40)
        before = snapshot(current)
        if mode == CORRECTION_CORRECTED:
            after = snapshot(recorded)
            corrected += 1
        elif mode == CORRECTION_EQUAL:
            after = before
            equal += 1
        else:
            after = before
            skipped += 1
        steer_flag = STEERING_APPLIED if index == 0 else 0
        steering += bool(steer_flag)
        events = tuple(event + offset for offset in range(1, 6))
        event = events[-1]
        frames.append(
            _FRAME.pack(
                100 + index,
                1000 + index,
                300 + index,
                mode | steer_flag | AUDIT_EXACT | GATE2_COMPLETE | COMMITTED,
                16667,
                16667,
                *events,
                recorded[:64],
                recorded[64:],
                before,
                after,
            )
        )
    count = len(frames)
    addresses = (
        2668,
        0x70000000,
        0x71000000,
        0x72000000,
        0x73000000,
        0x730001D0,
        0x730001A0,
        0x74000F64,
        0x75000000,
        0x75000188,
        0x74000000,
        0x76000000,
        0x77000000,
        0x77000010,
        0x77000150,
    )
    counters = (
        event,
        count,
        steering * 2,
        equal,
        corrected,
        skipped,
        0,
        0,
        0,
        corrected,
        0,
        0,
        0,
        4,
    )
    header = _HEADER.pack(
        MAGIC,
        VERSION,
        HEADER_SIZE,
        FRAME_SIZE,
        0x1F,
        count,
        count,
        BUILD_ID,
        0,
        *addresses,
        *counters,
        10,
        14,
    )
    return header + b"".join(frames)


class UnifiedExecutorReportTests(unittest.TestCase):
    def test_accepts_equal_corrected_and_skipped(self) -> None:
        summary = decode_report(make_report())
        self.assertEqual((summary.equal_frames, summary.corrected_frames, summary.skipped_frames), (1, 1, 1))
        self.assertEqual(summary.control_writes, 2)

    def test_rejects_trailing_byte(self) -> None:
        with self.assertRaisesRegex(ValueError, "exactly"):
            decode_report(make_report() + b"x")

    def test_rejects_gate2_event_reordering(self) -> None:
        blob = bytearray(make_report((CORRECTION_EQUAL,)))
        struct.pack_into("<Q", blob, HEADER_SIZE + 56, 1)
        with self.assertRaisesRegex(ValueError, "Gate 2"):
            decode_report(bytes(blob))

    def test_rejects_immediate_audit_mutation(self) -> None:
        blob = bytearray(make_report((CORRECTION_EQUAL,)))
        blob[HEADER_SIZE + FRAME_SIZE - 1] ^= 1
        with self.assertRaisesRegex(ValueError, "equal-frame"):
            decode_report(bytes(blob))

    def test_rejects_header_counter_mismatch(self) -> None:
        blob = bytearray(make_report())
        corrected_counter_offset = 8 + 24 + 20 + 4 + (15 + 4) * 8
        struct.pack_into("<Q", blob, corrected_counter_offset, 9)
        with self.assertRaisesRegex(ValueError, "cover all frames"):
            decode_report(bytes(blob))

    def test_rejects_noncontiguous_tick(self) -> None:
        blob = bytearray(make_report())
        struct.pack_into("<Q", blob, HEADER_SIZE + FRAME_SIZE, 999)
        with self.assertRaisesRegex(ValueError, "not contiguous"):
            decode_report(bytes(blob))

    def test_rejects_corrected_frame_that_is_equal(self) -> None:
        blob = bytearray(make_report((CORRECTION_CORRECTED,)))
        before_offset = HEADER_SIZE + 156
        before = bytes(blob[before_offset : before_offset + 804])
        blob[HEADER_SIZE + 80 : HEADER_SIZE + 144] = before[0x10:0x50]
        blob[HEADER_SIZE + 144 : HEADER_SIZE + 156] = before[0x150:0x15C]
        with self.assertRaisesRegex(ValueError, "already equal"):
            decode_report(bytes(blob))

    def test_rejects_nonzero_error_counter(self) -> None:
        blob = bytearray(make_report())
        read_errors_offset = 8 + 24 + 20 + 4 + (15 + 6) * 8
        struct.pack_into("<Q", blob, read_errors_offset, 1)
        with self.assertRaisesRegex(ValueError, "error/rollback"):
            decode_report(bytes(blob))


if __name__ == "__main__":
    unittest.main()
