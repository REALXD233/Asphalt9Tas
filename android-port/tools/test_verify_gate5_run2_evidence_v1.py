#!/usr/bin/env python3
"""Regression tests for the Gate 5 Retry Run 2 evidence verifier."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from verify_gate5_run2_evidence_v1 import verify_run2


ANDROID_PORT = Path(__file__).resolve().parents[1]
RECORDING = (
    ANDROID_PORT
    / "evidence"
    / "a9tas_gate5_handoff_input_run2_retry_20260817.a9nps1"
)
REPORT = (
    ANDROID_PORT
    / "evidence"
    / "a9tas_gate5_handoff_audit_run2_retry_20260817.a9cdt1"
)


class Gate5Run2EvidenceVerifierTests(unittest.TestCase):
    def test_accepts_the_exact_retry_pair(self) -> None:
        self.assertEqual(verify_run2(RECORDING, REPORT), [])

    def test_rejects_report_hash_drift(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "audit.a9cdt1"
            changed = bytearray(REPORT.read_bytes())
            changed[-1] ^= 1
            path.write_bytes(changed)
            problems = verify_run2(RECORDING, path)
        self.assertTrue(any("A9CDT1 hash mismatch" in item for item in problems))
        self.assertTrue(any("immediate audit differs" in item for item in problems))


if __name__ == "__main__":
    unittest.main()
