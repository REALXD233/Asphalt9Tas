#!/usr/bin/env python3
"""Strict A9USR2 brake-capture codec and policy tests."""

from __future__ import annotations

import pathlib
import struct
import unittest

from synchronized_brake_recording_v1 import (
    INPUT_CYCLE_ANCHOR_FLAG,
    MAGIC,
    RECORDED_SKIP_FLAGS,
    REQUIRED_FLAGS,
    VERSION,
    decode_sync_brake_report,
    verify_synchronized_brake_capture,
)
from synchronized_tick_recording_v1 import (
    FRAME_AUDIT_SIZE,
    HEADER_SIZE,
    _FRAME as REPORT_FRAME,
    _HEADER as REPORT_HEADER,
)
from unified_tick_recording_v1 import FRAME_SIZE, HEADER_SIZE as INPUT_HEADER_SIZE


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_REPORT = ROOT / "evidence" / "a9tas_natural_preroll_source_run1_20260817.a9usr1"
SOURCE_INPUT = ROOT / "evidence" / "a9tas_natural_preroll_source_run1_20260817.a9utk1"
RECORDER_SOURCE = (ROOT / "src" / "hwbp_synchronized_tick_recorder_v1.cpp").read_text(
    encoding="utf-8"
)


def make_brake_source(brake_bits: int = 0xBF800000) -> tuple[bytes, bytes]:
    report_blob = SOURCE_REPORT.read_bytes()
    header = list(REPORT_HEADER.unpack_from(report_blob))
    header[0], header[1], header[4] = MAGIC, VERSION, REQUIRED_FLAGS
    report_frames: list[bytes] = []
    for index in range(header[6]):
        frame = list(REPORT_FRAME.unpack_from(report_blob, HEADER_SIZE + index * FRAME_AUDIT_SIZE))
        frame[17] = (frame[17] & 0xFFFFFFFF00000000) | brake_bits
        frame[18] = (frame[18] & 0xFFFFFFFF00000000) | brake_bits
        report_frames.append(REPORT_FRAME.pack(*frame))

    recording = bytearray(SOURCE_INPUT.read_bytes())
    for index in range(header[6]):
        offset = INPUT_HEADER_SIZE + index * FRAME_SIZE
        struct.pack_into("<I", recording, offset + 20, brake_bits)
        skip_flags = struct.unpack_from("<I", recording, offset + 32)[0]
        struct.pack_into("<I", recording, offset + 32, skip_flags & ~(1 << 1))
    return REPORT_HEADER.pack(*header) + b"".join(report_frames), bytes(recording)


class SynchronizedBrakeRecordingTests(unittest.TestCase):
    def test_macro_uses_independent_report_and_fail_closed_pair_check(self) -> None:
        self.assertIn("A9TAS_SYNC_BRAKE_CAPTURE_V1", RECORDER_SOURCE)
        self.assertIn("A9USR2", RECORDER_SOURCE)
        self.assertIn("brake_pair_changed_within_tick", RECORDER_SOURCE)
        self.assertIn("kSyncBrakeCaptured", RECORDER_SOURCE)
        self.assertIn("pending_frame.brake <= -0.5f", RECORDER_SOURCE)
        self.assertIn("warmup_pending = !accept_warmup", RECORDER_SOURCE)

    def test_accepts_raw_nonzero_brake_and_cross_binds_every_frame(self) -> None:
        report, recording = make_brake_source()
        parsed = decode_sync_brake_report(report)
        self.assertEqual(parsed.captured_frames, 5)
        verified = verify_synchronized_brake_capture(report, recording)
        self.assertEqual(verified.captured_frames, 5)
        for index in range(5):
            skip = struct.unpack_from(
                "<I", recording, INPUT_HEADER_SIZE + index * FRAME_SIZE + 32
            )[0]
            self.assertEqual(skip, RECORDED_SKIP_FLAGS)

    def test_rejects_pair_drift_and_recording_bit_tamper(self) -> None:
        report, recording = make_brake_source()
        changed = bytearray(report)
        first = list(REPORT_FRAME.unpack_from(changed, HEADER_SIZE))
        first[18] ^= 1
        changed[HEADER_SIZE : HEADER_SIZE + FRAME_AUDIT_SIZE] = REPORT_FRAME.pack(*first)
        with self.assertRaisesRegex(ValueError, "brake bits differ"):
            decode_sync_brake_report(bytes(changed))
        changed_input = bytearray(recording)
        changed_input[INPUT_HEADER_SIZE + 20] ^= 1
        with self.assertRaisesRegex(ValueError, "brake cross-bind"):
            verify_synchronized_brake_capture(report, bytes(changed_input))

    def test_input_cycle_gate_requires_dedicated_anchor_flag(self) -> None:
        report, recording = make_brake_source()
        with self.assertRaisesRegex(ValueError, "success flags"):
            verify_synchronized_brake_capture(
                report, recording, require_input_cycle_anchor=True
            )
        anchored = bytearray(report)
        header = list(REPORT_HEADER.unpack_from(anchored))
        header[4] |= INPUT_CYCLE_ANCHOR_FLAG
        anchored[:HEADER_SIZE] = REPORT_HEADER.pack(*header)
        verified = verify_synchronized_brake_capture(
            bytes(anchored), recording, require_input_cycle_anchor=True
        )
        self.assertEqual(verified.captured_frames, 5)
        with self.assertRaisesRegex(ValueError, "success flags"):
            verify_synchronized_brake_capture(bytes(anchored), recording)


if __name__ == "__main__":
    unittest.main()
