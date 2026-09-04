#!/usr/bin/env python3
"""Tests for the strict A9UER2 prefix-certified report verifier."""

from __future__ import annotations

import struct
import unittest

from parse_unified_executor_report_v2 import (
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
    PREFIX_CERTIFIED,
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


def make_report(
    modes: tuple[int, ...] = (
        CORRECTION_EQUAL,
        CORRECTION_CORRECTED,
        CORRECTION_SKIPPED,
    ),
) -> bytes:
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
        events = tuple(event + offset for offset in range(1, 7))
        event = events[-1]
        frames.append(
            _FRAME.pack(
                100 + index,
                1000 + index,
                300 + index,
                mode
                | steer_flag
                | AUDIT_EXACT
                | GATE2_COMPLETE
                | COMMITTED
                | PREFIX_CERTIFIED,
                16667,
                16667,
                *events,
                1000 + index,
                1001 + index,
                1,
                bytes(6),
                recorded[:64],
                recorded[64:],
                before,
                after,
            )
        )
    count = len(frames)
    native = 0x77000000
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
        native,
        native + 0x10,
        native + 0x150,
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


class UnifiedExecutorReportV2Tests(unittest.TestCase):
    def test_accepts_prefix_certified_frames(self) -> None:
        summary = decode_report(make_report())
        self.assertEqual(
            (summary.equal_frames, summary.corrected_frames, summary.skipped_frames),
            (1, 1, 1),
        )

    def test_rejects_missing_prefix_flag(self) -> None:
        blob = bytearray(make_report((CORRECTION_EQUAL,)))
        struct.pack_into("<I", blob, HEADER_SIZE + 20, 0x10 | 0x20 | 0x40 | 0x02)
        with self.assertRaisesRegex(ValueError, "audit flags"):
            decode_report(bytes(blob))

    def test_rejects_unchanged_completion_token(self) -> None:
        blob = bytearray(make_report((CORRECTION_EQUAL,)))
        before = struct.unpack_from("<Q", blob, HEADER_SIZE + 88)[0]
        struct.pack_into("<Q", blob, HEADER_SIZE + 96, before)
        with self.assertRaisesRegex(ValueError, "completion token"):
            decode_report(bytes(blob))

    def test_rejects_closed_callback_at_c9c(self) -> None:
        blob = bytearray(make_report((CORRECTION_EQUAL,)))
        struct.pack_into("<H", blob, HEADER_SIZE + 104, 0)
        with self.assertRaisesRegex(ValueError, "not open"):
            decode_report(bytes(blob))

    def test_rejects_nonzero_reserved_bytes(self) -> None:
        blob = bytearray(make_report((CORRECTION_EQUAL,)))
        blob[HEADER_SIZE + 106] = 1
        with self.assertRaisesRegex(ValueError, "reserved"):
            decode_report(bytes(blob))

    def test_rejects_c9c_after_f64(self) -> None:
        blob = bytearray(make_report((CORRECTION_EQUAL,)))
        struct.pack_into("<Q", blob, HEADER_SIZE + 56, 4)
        with self.assertRaisesRegex(ValueError, "event order"):
            decode_report(bytes(blob))


if __name__ == "__main__":
    unittest.main()
