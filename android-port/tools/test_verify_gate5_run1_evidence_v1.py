#!/usr/bin/env python3
"""Regression tests for the Gate 5 Run 1 evidence verifier."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from native_physics_recording_v1 import (
    NativePhysicsFrameV1,
    decode_recording,
    encode_recording,
)
from verify_gate5_run1_evidence_v1 import verify_evidence


ANDROID_PORT = Path(__file__).resolve().parents[1]
RECORDING = ANDROID_PORT / "evidence" / "a9tas_gate5_handoff_input_run1_20260817.a9nps1"
REPORT = ANDROID_PORT / "evidence" / "a9tas_gate5_handoff_audit_run1_20260817.a9cdt1"


class Gate5Run1EvidenceVerifierTests(unittest.TestCase):
    def test_accepts_the_exact_archived_pair(self) -> None:
        self.assertEqual(verify_evidence(RECORDING, REPORT), [])

    def test_rejects_hash_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "recording.a9nps1"
            path.write_bytes(RECORDING.read_bytes() + b"x")
            problems = verify_evidence(path, REPORT)
        self.assertTrue(any("hash mismatch" in item for item in problems))
        self.assertTrue(any("A9NPS1 rejected" in item for item in problems))

    def test_cross_binds_recorded_payload_to_audit(self) -> None:
        frames = list(decode_recording(RECORDING.read_bytes()))
        original = frames[0]
        changed_transform = bytes([original.transform[0] ^ 1]) + original.transform[1:]
        frames[0] = NativePhysicsFrameV1(
            original.tick,
            original.monotonic_ns,
            changed_transform,
            original.linear_velocity,
            original.flags,
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "recording.a9nps1"
            path.write_bytes(encode_recording(frames))
            problems = verify_evidence(
                path,
                REPORT,
                expected_recording_hash=None,
                expected_report_hash=None,
            )
        self.assertTrue(any("recording/report transform mismatch" in item for item in problems))

    def test_cross_binds_recorded_time_to_audit(self) -> None:
        frames = list(decode_recording(RECORDING.read_bytes()))
        first = frames[0]
        frames[0] = NativePhysicsFrameV1(
            first.tick,
            first.monotonic_ns - 1,
            first.transform,
            first.linear_velocity,
            first.flags,
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "recording.a9nps1"
            path.write_bytes(encode_recording(frames))
            problems = verify_evidence(
                path,
                REPORT,
                expected_recording_hash=None,
                expected_report_hash=None,
            )
        self.assertTrue(any("recording/report time mismatch" in item for item in problems))


if __name__ == "__main__":
    unittest.main()
