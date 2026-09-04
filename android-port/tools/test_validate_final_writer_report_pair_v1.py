from __future__ import annotations

import types
import unittest
from unittest import mock

import validate_final_writer_payload_report_v1 as payload_v1
import validate_final_writer_report_pair_v1 as pair_v1
from parse_unified_executor_report_v2 import HEADER_SIZE, _HEADER
from parse_unified_executor_report_v5 import FRAME_SIZE, _FRAME


class FinalWriterReportPairTest(unittest.TestCase):
    @staticmethod
    def fixtures() -> tuple[bytes, bytes]:
        pid, base, player = 123, 0x100000, 0x3300
        header = [b"A9UER8\0\0", 8, HEADER_SIZE, FRAME_SIZE,
                  0x1F, 2, 2, bytes(20), 0]
        counters = [pid, base, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60,
                    0x70, 0x80, player, 0x90, 0xA0, 0xB0, 0xC0,
                    20, 2, 4, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0]
        unified_header = _HEADER.pack(*(header + counters + [2, 2]))
        target0 = bytes(64); linear0 = bytes(12)
        target1 = bytes([1]) * 64; linear1 = bytes([2]) * 12
        before0 = bytearray(804)
        before1 = bytearray(804)
        before1[0x10:0x50] = bytes([3]) * 64
        before1[0x150:0x15C] = bytes([4]) * 12
        immediate0 = bytearray(before0)
        immediate1 = bytearray(before1)
        immediate1[0x10:0x50] = target1
        immediate1[0x150:0x15C] = linear1
        def frame(index, flags, transform, linear, before, immediate):
            return _FRAME.pack(
                index, index, 1, flags, 1, 16667,
                1, 2, 3, 4, 5, 6, 7, 8, 9,
                1, 2, b"\0\0", 0, bytes(4), 0, 0,
                transform, linear, bytes(before), bytes(immediate))
        unified = unified_header + frame(
            0, CORRECTION_EQUAL_FLAGS, target0, linear0, before0, immediate0)
        unified += frame(
            1, CORRECTION_CORRECTED_FLAGS, target1, linear1, before1, immediate1)

        evidence = bytes(payload_v1.EVIDENCE_SIZE)
        payload_header = payload_v1._HEADER.pack(
            payload_v1.MAGIC, 1, payload_v1.HEADER_SIZE,
            payload_v1.AUDIT_SIZE, payload_v1.REQUIRED_FLAGS, 2, 0,
            pid, base, player, 0x5000, 0x6000, 0x7000,
            0x8000, 0x9000, 0xA000, 0xB000,
            bytes(32), bytes(32), evidence)
        audit0 = payload_v1._AUDIT.pack(
            0, payload_v1.AUDIT_ORIGINAL_RETURNED | payload_v1.AUDIT_EQUAL |
            payload_v1.AUDIT_IMMEDIATE_EXACT,
            target0, linear0, target0, linear0)
        audit1 = payload_v1._AUDIT.pack(
            1, payload_v1.AUDIT_ORIGINAL_RETURNED | payload_v1.AUDIT_CORRECTED |
            payload_v1.AUDIT_IMMEDIATE_EXACT | payload_v1.AUDIT_FINAL_FRAME |
            payload_v1.AUDIT_VPTR_RESTORED,
            bytes([3]) * 64, bytes([4]) * 12, target1, linear1)
        return unified, payload_header + audit0 + audit1

    def test_pair_provenance_matches_per_frame(self) -> None:
        unified, payload = self.fixtures()
        summary = types.SimpleNamespace(frames=2)
        with mock.patch.object(pair_v1.unified_v8, "decode_report",
                               return_value=summary), \
             mock.patch.object(pair_v1.payload_v1, "validate_report",
                               return_value={"frames": 2, "equal": 1,
                                             "corrected": 1}):
            self.assertEqual(pair_v1.validate_pair(unified, payload, b"x"),
                             {"frames": 2, "equal": 1, "corrected": 1})

    def test_snapshot_mismatch_fails(self) -> None:
        unified, payload = self.fixtures()
        damaged = bytearray(unified)
        damaged[HEADER_SIZE + 220 + 0x10] ^= 1
        summary = types.SimpleNamespace(frames=2)
        with mock.patch.object(pair_v1.unified_v8, "decode_report",
                               return_value=summary), \
             mock.patch.object(pair_v1.payload_v1, "validate_report",
                               return_value={"frames": 2, "equal": 1,
                                             "corrected": 1}):
            with self.assertRaises(ValueError):
                pair_v1.validate_pair(bytes(damaged), payload, b"x")


CORRECTION_EQUAL_FLAGS = (1 << 1)
CORRECTION_CORRECTED_FLAGS = (1 << 2)


if __name__ == "__main__":
    unittest.main()
