#!/usr/bin/env python3
"""Offline policy checks for the guarded A9AFD1 runner."""

from __future__ import annotations

import pathlib
import unittest


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
RUNNER = (
    WORKSPACE / "android-port" / "run-authoritative-fixed-delta-v1.ps1"
).read_text(encoding="utf-8")


class FixedDeltaRunnerPolicyTests(unittest.TestCase):
    def test_offline_return_precedes_any_adb_access(self) -> None:
        offline = RUNNER.index("if ($OfflineValidateOnly)")
        adb = RUNNER.index("& $AdbPath devices")
        self.assertLess(offline, adb)
        self.assertIn("deployed=0", RUNNER[offline:adb])

    def test_all_live_acknowledgements_are_required(self) -> None:
        for token in (
            "$ExecuteExactlyOneLiveAttempt",
            "$AcknowledgeReviewedBuildOnlyCandidate",
            "$AcknowledgeNaturallyRunningRaceNoManualInput",
            "$AcknowledgeNoPausedAttach",
            "$AcknowledgeExactlyFiveFixedDeltaWrites",
            "$AcknowledgeAllOtherCapabilitiesSkipped",
            "$AcknowledgeFiveFrameBindingNotTrajectoryClaim",
            "$AcknowledgeShortPtraceStallRisk",
        ):
            self.assertIn(token, RUNNER)

    def test_candidate_and_remote_copy_are_hash_pinned(self) -> None:
        self.assertIn(
            "e3e4306a4e8d23093c4a0a49f17f85b24cdb113bdf549be5ed9d4b06fa6a865c",
            RUNNER,
        )
        self.assertIn("Get-RemoteSha256", RUNNER)
        self.assertIn("Remote fixed-delta candidate hash differs", RUNNER)

    def test_process_and_detach_are_checked_before_and_after(self) -> None:
        self.assertIn("Game already has a tracer", RUNNER)
        self.assertGreaterEqual(RUNNER.count("Assert-StableGameAndDetach"), 3)
        self.assertIn("Game process exited or changed PID", RUNNER)

    def test_strict_validator_precedes_success(self) -> None:
        validation = RUNNER.index("python -B $validator $OutputPath")
        success = RUNNER.index("AUTHORITATIVE_FIXED_DELTA_LIVE_PASSED")
        self.assertLess(validation, success)
        self.assertIn("A9AFD1 report validation failed", RUNNER)

    def test_no_resume_or_input_injection_exists(self) -> None:
        for token in ("KEYCODE_ESCAPE", "input keyevent", "input tap", "sendevent"):
            self.assertNotIn(token, RUNNER)


if __name__ == "__main__":
    unittest.main()
