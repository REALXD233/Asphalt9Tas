#!/usr/bin/env python3
"""Offline tests for exact A9UTK1 -> A9UER6/A9UER8 control binding."""

from __future__ import annotations

import pathlib
import struct
import unittest

from parse_unified_executor_report_v2 import HEADER_SIZE, _HEADER
from parse_unified_executor_report_v5 import FRAME_SIZE, _FRAME
from validate_action_control_replay_v1 import validate_action_control_replay


ROOT = pathlib.Path(__file__).resolve().parents[1]
RECORDING = (
    ROOT / "evidence" /
    "a9tas_action_until_release_20260817_224943_800.a9utk1"
)
REPORT = (
    ROOT / "evidence" /
    "a9tas_natural_brake_replay_20260817_225614_890.a9uer6"
)


def as_a9uer8(blob: bytes) -> bytes:
    header = list(_HEADER.unpack_from(blob))
    header[0] = b"A9UER8\0\0"
    header[1] = 8
    return _HEADER.pack(*header) + blob[HEADER_SIZE:]


class ActionControlReplayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.recording = RECORDING.read_bytes()
        cls.report = REPORT.read_bytes()

    def test_proven_action_replay_is_raw_bit_exact(self) -> None:
        proof = validate_action_control_replay(self.recording, self.report)
        self.assertEqual(proof.frames, 344)
        self.assertEqual(proof.control_writes, 688)
        self.assertGreater(proof.overlap_frames, 0)

    def test_final_writer_a9uer8_envelope_retains_control_proof(self) -> None:
        proof = validate_action_control_replay(
            self.recording, as_a9uer8(self.report))
        self.assertEqual(proof.version, 8)

    def test_internally_consistent_but_wrong_brake_pair_is_rejected(self) -> None:
        damaged = bytearray(self.report)
        index = 240  # Known brake-active source tick in this immutable artifact.
        offset = HEADER_SIZE + index * FRAME_SIZE
        frame = list(_FRAME.unpack_from(damaged, offset))
        steering_half = frame[20] & 0xFFFFFFFF00000000
        frame[20] = steering_half
        frame[21] = steering_half
        damaged[offset:offset + FRAME_SIZE] = _FRAME.pack(*frame)
        with self.assertRaisesRegex(ValueError, "differs from A9UTK1"):
            validate_action_control_replay(self.recording, bytes(damaged))


if __name__ == "__main__":
    unittest.main()
