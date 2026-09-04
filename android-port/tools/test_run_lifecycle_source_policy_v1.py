#!/usr/bin/env python3
"""Offline safety policy for the lifecycle-bound source host runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-lifecycle-source-v1.ps1"


class LifecycleSourceRunnerPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = RUNNER.read_text(encoding="utf-8")

    def test_default_is_offline_before_any_adb_validation(self) -> None:
        self.assertIn('[string]$Mode = "OfflineValidate"', self.text)
        self.assertIn("deployed=0 device_access=0", self.text)
        offline = self.text.index('if ($Mode -eq "OfflineValidate")')
        adb_check = self.text.index("Test-Path -LiteralPath $AdbPath")
        self.assertLess(offline, adb_check)

    def test_live_attempt_is_single_and_bound_to_current_lifecycle_object(self) -> None:
        for needle in (
            "ExecuteExactlyOneAttempt",
            "AcknowledgeAncientRuinsZl1Countdown3Paused",
            "AcknowledgeCurrentReadOnlyLifecycleAddresses",
            "AcknowledgeSingleEscBeforeRelease",
            "Acknowledge360FixedDeltaWrites",
            "AcknowledgeReadOnlyControlAndPhysicsCapture",
            "AcknowledgeNoManualInput",
            "AcknowledgeManualSteerDriftSequenceAfterResume",
            "AcknowledgePtraceStallRollbackAndCrashRisk",
            '[ValidateSet("Neutral", "SteerDrift", "NaturalActions", "NaturalActionsBarrel")]',
            "StateAddressHex must equal ObjectHex + 0x2D8",
            "Get-StartTicks",
            "Get-TracerPid",
            "Assert-RemoteHash",
        ):
            self.assertIn(needle, self.text)

    def test_ready_proof_and_esc_release_order_are_exact(self) -> None:
        for needle in (
            "READY_ARMED_RACE_LIFECYCLE_SOURCE_V1",
            "state_address=0x$escapedState state=2",
            "all_target_threads_frozen=1 thread_set_stable=1",
            "gameplay_state_writes=0",
            "host_resume_gate=marker_removal",
        ):
            self.assertIn(needle, self.text)
        esc = self.text.index("$escProcess = Start-SingleEscInjection")
        release = self.text.index('"release lifecycle source candidate"')
        self.assertLess(esc, release)
        self.assertEqual(self.text.count("$escProcess = Start-SingleEscInjection"), 1)
        self.assertIn("Complete-SingleEscInjection $escProcess", self.text)
        self.assertIn("recipient is frozen", self.text)
        self.assertIn("single_ESC_before_release=1", self.text)

    def test_success_requires_authoritative_handoff_artifacts_and_cleanup(self) -> None:
        for needle in (
            "AUTHORITATIVE_RACE_SOURCE_START_V1 state=3 events=1",
            "tick_watchpoints_armed_before_continue=1",
            '"SYNCHRONIZED_TICK_RECORDER_${doneMagic}_DONE complete=1',
            '"captured=$TargetFrames/$TargetFrames delta_writes=$TargetFrames',
            "lifecycle_source_recording_v1.py",
            "Lifecycle $doneMagic/A9UTK1 validation failed",
            "TracerPid=0",
            "no retry",
        ):
            self.assertIn(needle, self.text)

    def test_action_profile_is_separate_and_content_validated(self) -> None:
        for needle in (
            '$CaptureProfile -eq "Neutral"',
            "Neutral capture cannot acknowledge a manual steer/drift sequence",
            "SteerDrift capture cannot acknowledge no manual input",
            "lifecycle_steer_drift_recording_v1.py",
            "Lifecycle steer/drift action-content validation failed; no retry",
            "profile=$CaptureProfile",
        ):
                self.assertIn(needle, self.text)

    def test_barrel_profile_is_separate_read_capture_and_strictly_validated(self) -> None:
        for needle in (
            '$barrelCapture = $CaptureProfile -eq "NaturalActionsBarrel"',
            "lifecycle-natural-action-barrel-source-v1",
            "test_lifecycle_barrel_source_policy_v1.py",
            "lifecycle_natural_action_barrel_recording_v1.py",
            "Lifecycle barrel source binary policy failed",
        ):
            self.assertIn(needle, self.text)


if __name__ == "__main__":
    unittest.main()
