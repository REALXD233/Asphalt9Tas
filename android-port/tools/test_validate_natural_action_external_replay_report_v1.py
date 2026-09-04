#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import unittest

from validate_natural_action_lifecycle_report_v1 import valid_report
from validate_natural_action_external_replay_report_v1 import (
    MAGIC,
    PAYLOAD_BUILD_ID,
    PAYLOAD_SHA256,
    validate,
)


ROOT = pathlib.Path(__file__).resolve().parents[1]
RECORDING = (
    ROOT / "evidence" /
    "a9tas_composite_natural_action_gate_360f_20260822.a9utk1"
)


def valid_external_report():
    report = valid_report()
    report.magic = MAGIC
    report.zero_call_frames = 360
    report.mailbox_claimed_sequence = 360
    report.mailbox_completed_sequence = 360
    evidence = report.evidence
    evidence.claimed_commands = 360
    evidence.zero_call_completions = 358
    evidence.last_sequence = 360
    evidence.last_frame = 359
    evidence.reserved[0] = 2       # action_command_completions
    evidence.reserved[1] = 3       # action_calls_submitted
    evidence.reserved[2] = 0x1000  # queue end before
    evidence.reserved[3] = 0x1010  # queue end after
    evidence.reserved[4] = 0xA9E2000000000000
    report.payload_sha256[:] = PAYLOAD_SHA256
    report.payload_build_id[:] = PAYLOAD_BUILD_ID
    return report


class ExternalReplayReportValidatorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.recording = RECORDING.read_bytes()

    def test_accepts_exact_360_frame_dual_receipt_aggregate(self) -> None:
        validate(valid_external_report(), self.recording)

    def test_rejects_missing_action_call(self) -> None:
        report = valid_external_report()
        report.evidence.reserved[1] = 2
        with self.assertRaisesRegex(ValueError, "action totals"):
            validate(report, self.recording)

    def test_rejects_stale_final_sequence(self) -> None:
        report = valid_external_report()
        report.mailbox_completed_sequence = 359
        with self.assertRaisesRegex(ValueError, "external replay cursor"):
            validate(report, self.recording)

    def test_rejects_missing_game_queue_proof(self) -> None:
        report = valid_external_report()
        report.evidence.reserved[4] = 0
        with self.assertRaisesRegex(ValueError, "game-owned action queue proof"):
            validate(report, self.recording)

    def test_rejects_unclean_natural_removal(self) -> None:
        report = valid_external_report()
        report.evidence.removal_returns = 0
        with self.assertRaisesRegex(ValueError, "natural registration/removal"):
            validate(report, self.recording)


if __name__ == "__main__":
    unittest.main()

