#!/usr/bin/env python3
"""Offline safety policy for the lifecycle-bound final-writer host runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-lifecycle-final-writer-v1.ps1"


class LifecycleFinalWriterRunnerPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = RUNNER.read_text(encoding="utf-8")

    def test_default_is_offline_before_any_adb_validation(self) -> None:
        self.assertIn('[string]$Mode = "OfflineValidate"', self.text)
        self.assertIn("deployed=0 device_access=0", self.text)
        offline = self.text.index('if ($Mode -eq "OfflineValidate")')
        adb_check = self.text.index("Test-Path -LiteralPath $AdbPath")
        self.assertLess(offline, adb_check)

    def test_live_replay_is_one_shot_identity_and_source_bound(self) -> None:
        for needle in (
            "ExecuteExactlyOneAttempt",
            "AcknowledgePreparedFreshProcess",
            "AcknowledgeAncientRuinsZl1Countdown3Paused",
            "AcknowledgeCurrentReadOnlyLifecycleAddresses",
            "AcknowledgeLifecycleSourceAndTargetHashes",
            "AcknowledgeSingleEscBeforeReleaseAndNoManualInput",
            "Acknowledge360Delta720ControlAndConditionalPhysicsWrites",
            "AcknowledgeSteerDriftSourceAndExactControlReplay",
            '[ValidateSet("Neutral", "SteerDrift")]',
            "AcknowledgePtraceStallRollbackAndCrashRisk",
            "StateAddressHex must equal ObjectHex + 0x2D8",
            "A9USR5/A9UTK1 source validation failed",
            "A9FWT1 is not SHA-bound to the lifecycle A9UTK1",
            "Prepared process identity mismatch",
        ):
            self.assertIn(needle, self.text)

    def test_steer_drift_profile_is_prevalidated_and_raw_bit_cross_bound(self) -> None:
        for needle in (
            '$ReplayProfile -eq "SteerDrift"',
            "lifecycle_steer_drift_recording_v1.py",
            "validate_action_control_replay_v1.py",
            "Lifecycle SteerDrift source validation failed; no device opened",
            "Lifecycle SteerDrift exact control replay validation failed; no retry",
            "Neutral replay cannot acknowledge a SteerDrift action source",
            "profile=$ReplayProfile",
        ):
            self.assertIn(needle, self.text)

    def test_ready_proof_and_esc_release_order_are_exact(self) -> None:
        for needle in (
            "READY_ARMED_RACE_LIFECYCLE_FINAL_WRITER_V1",
            "state_address=0x$escapedState state=2",
            "vptr_writes=1 gameplay_state_writes=0",
            "payload_mapped=1 all_target_threads_frozen=1",
            "host_resume_gate=marker_removal",
        ):
            self.assertIn(needle, self.text)
        esc = self.text.index("$escProcess = Start-SingleEscInjection")
        release = self.text.index('"release lifecycle final-writer candidate"')
        self.assertLess(esc, release)
        self.assertEqual(self.text.count("$escProcess = Start-SingleEscInjection"), 1)
        self.assertIn("Complete-SingleEscInjection $escProcess", self.text)
        self.assertIn("synchronous input command", self.text)

    def test_success_requires_authoritative_handoff_reports_and_cleanup(self) -> None:
        for needle in (
            "AUTHORITATIVE_RACE_START_V1 state=3 events=1",
            "tick_watchpoints_armed_before_continue=1",
            "parse_unified_executor_report_v8.py",
            "validate_final_writer_report_pair_v1.py",
            "A9UER8/A9FWR1 cross-validation failed",
            "LIFECYCLE_FINAL_WRITER_HOST_WITNESS_V1",
            "source_report_sha256=$expectedSourceReportHash",
            "Host witness: $hostWitness sha256=$hostWitnessHash",
            "TracerPid=0",
            "no retry",
        ):
            self.assertIn(needle, self.text)


if __name__ == "__main__":
    unittest.main()
