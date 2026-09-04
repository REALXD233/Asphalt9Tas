#!/usr/bin/env python3
"""Static and parser policy tests for Gate 10 brake isolation."""

from __future__ import annotations

import pathlib
import struct
import unittest

from parse_unified_executor_report_v2 import STEERING_APPLIED, _HEADER
from parse_unified_executor_report_v5 import HEADER_SIZE, _FRAME
from parse_unified_executor_report_v6 import BRAKE_APPLIED, decode_report
from unified_tick_recording_v1 import decode_recording
from verify_unified_brake_gate_v1 import verify


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_unified_tick_executor_v1.cpp").read_text(encoding="utf-8")
BUILD = (ROOT / "build-hwbp-unified-brake-v1.ps1").read_text(encoding="utf-8")
PARSER = (ROOT / "tools" / "parse_unified_executor_report_v6.py").read_text(encoding="utf-8")
VERIFY = (ROOT / "tools" / "verify_unified_brake_gate_v1.py").read_text(encoding="utf-8")


class UnifiedBrakeGateTests(unittest.TestCase):
    @staticmethod
    def synthetic_report() -> tuple[bytes, bytes]:
        recording = (ROOT / "evidence" / "a9tas_gate10_brake_only_1f_draft_20260817.a9utk1").read_bytes()
        _, inputs = decode_recording(recording)
        original = (ROOT / "evidence" / "a9tas_gate7_steering_report_20260817_182513_762.a9uer5").read_bytes()
        header = list(_HEADER.unpack_from(original))
        frame = list(_FRAME.unpack_from(original, HEADER_SIZE))
        header[0], header[1] = b"A9UER6\0\0", 6
        frame[3] = (frame[3] & ~STEERING_APPLIED) | BRAKE_APPLIED
        brake_bits = struct.unpack("<I", struct.pack("<f", inputs[0].brake))[0]
        frame[20] = (frame[20] & 0xFFFFFFFF00000000) | brake_bits
        frame[21] = (frame[21] & 0xFFFFFFFF00000000) | brake_bits
        frame[22], frame[23] = inputs[0].transform, inputs[0].linear_velocity
        return _HEADER.pack(*header) + _FRAME.pack(*frame), recording

    def test_separate_macro_and_binary(self) -> None:
        self.assertIn("A9TAS_UNIFIED_BRAKE_V1", BUILD)
        self.assertIn("a9tas_hwbp_unified_brake_v1", BUILD)
        self.assertIn("#ifdef A9TAS_UNIFIED_BRAKE_V1", SOURCE)

    def test_brake_replaces_only_low_pair_bits(self) -> None:
        self.assertIn("0xffffffff00000000ULL", SOURCE)
        self.assertIn("| brake_bits", SOURCE)
        self.assertIn("kUnifiedBrakeApplied", SOURCE)

    def test_a9uer6_requires_both_low_write_audits(self) -> None:
        self.assertIn("BRAKE_APPLIED = 1 << 8", PARSER)
        self.assertIn("c9c_pair & 0xFFFFFFFF", PARSER)
        self.assertIn("target_bits", VERIFY)
        self.assertIn("frame[20]", VERIFY)
        self.assertIn("frame[21]", VERIFY)

    def test_gate_is_transform_and_other_action_isolated(self) -> None:
        generator = (ROOT / "tools" / "make_unified_brake_gate_v1.py").read_text(encoding="utf-8")
        self.assertIn('"skip_flags": "0xfd"', generator)
        self.assertIn('"brake": -1.0', generator)
        self.assertIn('"transform"', generator)

    def test_strict_parser_and_cross_binding_accept_canonical_report(self) -> None:
        report, recording = self.synthetic_report()
        self.assertEqual(decode_report(report).control_writes, 2)
        verify(report, recording)

    def test_rejects_missing_brake_flag_and_mismatched_second_write(self) -> None:
        report, recording = self.synthetic_report()
        frame = list(_FRAME.unpack_from(report, HEADER_SIZE))
        frame[3] &= ~BRAKE_APPLIED
        with self.assertRaisesRegex(ValueError, "brake audit flag"):
            decode_report(report[:HEADER_SIZE] + _FRAME.pack(*frame))
        frame = list(_FRAME.unpack_from(report, HEADER_SIZE))
        frame[21] ^= 1
        changed = report[:HEADER_SIZE] + _FRAME.pack(*frame)
        with self.assertRaisesRegex(ValueError, "brake pair writes differ"):
            verify(changed, recording)


if __name__ == "__main__":
    unittest.main()
