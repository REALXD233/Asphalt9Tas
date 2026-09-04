#!/usr/bin/env python3
"""Offline policy checks for the guarded A9ANO1 live runner."""

from __future__ import annotations

import pathlib
import unittest


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
RUNNER = (
    WORKSPACE / "android-port" / "run-authoritative-neutral-observer-v1.ps1"
).read_text(encoding="utf-8")


class AuthoritativeNeutralRunnerPolicyTests(unittest.TestCase):
    def test_offline_mode_returns_before_adb_checks(self) -> None:
        offline = RUNNER.index("if ($OfflineValidateOnly)")
        adb = RUNNER.index("& $AdbPath devices")
        self.assertLess(offline, adb)
        self.assertIn("deployed=0", RUNNER[offline:adb])

    def test_live_mode_requires_all_explicit_acknowledgements(self) -> None:
        for token in (
            "$ExecuteExactlyOneLiveAttempt",
            "$AcknowledgeReviewedBuildOnlyCandidate",
            "$AcknowledgeNaturallyRunningRaceNoManualInput",
            "$AcknowledgeNoPausedAttach",
            "$AcknowledgeFiveFramePhaseBindingOnly",
            "$AcknowledgeZeroGameplayWritesAndActions",
            "$AcknowledgeShortPtraceStallRisk",
        ):
            self.assertIn(token, RUNNER)

    def test_runner_pins_and_verifies_remote_candidate(self) -> None:
        self.assertIn(
            "98f977d04fe2c399cc544fa5b0dac96c5fd92932ea476da8dd80653e116410e5",
            RUNNER,
        )
        self.assertIn("Get-RemoteSha256", RUNNER)
        self.assertIn("Remote neutral observer hash differs after push", RUNNER)

    def test_pre_and_post_detach_checks_are_mandatory(self) -> None:
        self.assertIn("Game already has a tracer", RUNNER)
        self.assertGreaterEqual(RUNNER.count("Assert-StableGameAndDetach"), 3)
        self.assertIn("Game process exited or changed PID", RUNNER)

    def test_success_requires_strict_report_validator(self) -> None:
        validation = RUNNER.index("python -B $validator $OutputPath")
        success = RUNNER.index("AUTHORITATIVE_NEUTRAL_OBSERVER_LIVE_PASSED")
        self.assertLess(validation, success)
        self.assertIn("A9ANO1 report validation failed", RUNNER)

    def test_no_automatic_resume_or_input_command(self) -> None:
        for token in ("KEYCODE_ESCAPE", "input keyevent", "input tap", "sendevent"):
            self.assertNotIn(token, RUNNER)


if __name__ == "__main__":
    unittest.main()
