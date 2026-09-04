#!/usr/bin/env python3
"""Static safety policy for the paired start-line host runners."""
from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE_RUNNER = (ROOT / "android-port/run-startline-tick-source-v1.ps1").read_text(encoding="utf-8")
REPLAY_RUNNER = (ROOT / "android-port/run-authoritative-steering-startline-v1.ps1").read_text(encoding="utf-8")
UNIFIED_RUNNER = (ROOT / "android-port/run-startline-unified-replay-v1.ps1").read_text(encoding="utf-8")


class StartlineRunnerPolicyTests(unittest.TestCase):
    def test_each_runner_requires_one_attempt_and_single_escape_ack(self) -> None:
        for runner in (SOURCE_RUNNER, REPLAY_RUNNER, UNIFIED_RUNNER):
            self.assertIn("ExecuteExactlyOneLiveAttempt", runner)
            self.assertIn("AcknowledgeExactlyOneEscapeResumeInput", runner)
            self.assertIn("input keyevent 111", runner)
            self.assertEqual(runner.count("input keyevent 111"), 1)
            self.assertIn("no retry was attempted", runner)

    def test_ready_marker_and_tracer_are_checked_before_escape(self) -> None:
        for runner in (SOURCE_RUNNER, REPLAY_RUNNER, UNIFIED_RUNNER):
            ready = runner.index("READY_NO_ATTACH_V1")
            tracer = runner.index("Candidate attached before ESC")
            escape = runner.index("input keyevent 111")
            self.assertLess(ready, tracer)
            self.assertLess(tracer, escape)

    def test_offline_mode_returns_before_adb_access(self) -> None:
        for runner in (SOURCE_RUNNER, REPLAY_RUNNER, UNIFIED_RUNNER):
            offline = runner.index("if ($OfflineValidateOnly)")
            first_device = runner.index("Invoke-AdbText @")
            self.assertLess(offline, first_device)

    def test_replay_requires_recording_hash_and_declares_scope(self) -> None:
        self.assertIn("[Parameter(Mandatory=$true)][string]$RecordingSha256", REPLAY_RUNNER)
        self.assertIn("AcknowledgeSteeringOnlyBrakePreserved", REPLAY_RUNNER)
        self.assertIn("AcknowledgeRecordedBrakePairWrites", REPLAY_RUNNER)
        self.assertIn("ReplayRecordedBrake", REPLAY_RUNNER)
        self.assertIn("scope=steering+recorded-brake", REPLAY_RUNNER)
        self.assertIn("physical_anchor_search=0", REPLAY_RUNNER)

    def test_unified_runner_declares_bounded_physics_and_no_actions(self) -> None:
        self.assertIn("AcknowledgeUpToTwiceFrameCountPhysicsWrites", UNIFIED_RUNNER)
        self.assertIn("AcknowledgeNoNitroOrGameplayActionCalls", UNIFIED_RUNNER)
        self.assertIn("aligned_brake_replay_v1.py", UNIFIED_RUNNER)
        self.assertIn("physical_anchor_search=0 nitro_action_calls=0", UNIFIED_RUNNER)


if __name__ == "__main__":
    unittest.main()
