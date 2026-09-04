#!/usr/bin/env python3
"""Offline closure tests for the guarded 0/1/0/2/0 live candidate."""

from __future__ import annotations

import hashlib
import pathlib
import unittest

import validate_natural_action_replay_sequence_report_v1 as validator


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "natural_action_lifecycle_controller_v1.cpp"
RUNNER = ROOT / "run-natural-action-lifecycle-v1.ps1"
PAYLOAD = (ROOT / "build" / "natural-action-replay-payload-v1" /
           "liba9tas_natural_action_replay_v1_review_only.so")
CANDIDATE = (ROOT / "build" / "natural-action-replay-sequence-live-candidate-v1" /
             "natural_action_replay_sequence_candidate_v1")
BOOTSTRAP = (ROOT / "build" / "natural-action-replay-sequence-live-candidate-v1" /
             "liba9tas_bootstrap_nar5_v1.so")


def sha(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class NaturalActionReplaySequenceGateTests(unittest.TestCase):
    def test_report_validator_rejects_partial_or_wrong_totals(self) -> None:
        validator.selftest()

    def test_controller_sequence_and_cleanup_are_fixed(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        for token in (
            "kReplaySequence[] = {0, 1, 0, 2, 0}",
            "kReplaySequenceFrames = std::size(kReplaySequence)",
            "kReplaySequenceActionFrames = 2",
            "kReplaySequenceActionCalls = 3",
            "command.sequence = static_cast<std::uint64_t>(frame) + 1u",
            "RequestCleanRemoval",
            "PollForNaturalRemoval",
        ):
            self.assertIn(token, text)

    def test_artifact_pins_are_exact(self) -> None:
        self.assertEqual(sha(PAYLOAD),
            "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f")
        self.assertEqual(sha(CANDIDATE),
            "0be4583e0fe336c6e8d73a3cb07690b1913ca04b5ed6736f225a1c62611a555c")
        self.assertEqual(sha(BOOTSTRAP),
            "85995fa56ef4de5f7f8507179aa50f5fd7891454b29f5dfcddfd5d9dcdef0e5a")

    def test_runner_is_separate_opt_in_and_fail_closed(self) -> None:
        text = RUNNER.read_text(encoding="utf-8")
        for token in (
            "[switch]$SequenceReplayReview",
            "[switch]$AcknowledgeFiveFrameNaturalNitroSequence",
            "ActionSchedulerReview, SequenceReplayReview and RecordingSourceReview are mutually exclusive",
            '"natural_nitro_sequence_0_1_0_2_0"',
            "I_ACCEPT_FIVE_FRAME_ACTION_SEQUENCE_REVIEW_V1",
            "five consecutive natural action frames 0/1/0/2/0 with three total calls",
            "$waitMultiplier = if ($SequenceReplayReview) { 8 } else { 4 }",
            "-not $passed -and $mutationRisk",
            "Force-StopGame",
            "device_access=0 deployed=0",
        ):
            self.assertIn(token, text)


if __name__ == "__main__":
    unittest.main()
