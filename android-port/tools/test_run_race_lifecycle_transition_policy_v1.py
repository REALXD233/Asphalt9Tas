#!/usr/bin/env python3
"""Offline safety policy for the guarded race-lifecycle transition runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-race-lifecycle-transition-v1.ps1"


class RaceLifecycleTransitionRunnerPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = RUNNER.read_text(encoding="utf-8")

    def test_default_is_offline_before_any_adb_validation(self) -> None:
        self.assertIn('[string]$Mode = "OfflineValidate"', self.text)
        self.assertIn('deployed=0 device_access=0', self.text)
        offline = self.text.index('if ($Mode -eq "OfflineValidate")')
        adb_check = self.text.index('Test-Path -LiteralPath $AdbPath')
        self.assertLess(offline, adb_check)

    def test_live_attempt_is_explicitly_gated_and_identity_bound(self) -> None:
        for needle in (
            "AcknowledgeAncientRuinsZl1Countdown3Paused",
            "AcknowledgeCurrentReadOnlyLifecycleAddresses",
            "AcknowledgeSingleEscBeforeTracerRelease",
            "AcknowledgePtraceDebugRegistersOnly",
            "AcknowledgeStallCleanupAndCrashRisk",
            "ExecuteExactlyOneAttempt",
            "Get-StartTicks",
            "Get-TracerPid",
            "Assert-RemoteHash",
            "StateAddressHex must equal ObjectHex + 0x2D8",
        ):
            self.assertIn(needle, self.text)

    def test_single_esc_is_queued_before_marker_release(self) -> None:
        esc = self.text.index("$escProcess = Start-SingleEscInjection")
        release = self.text.index('"release lifecycle transition observer"')
        self.assertLess(esc, release)
        self.assertEqual(self.text.count("$escProcess = Start-SingleEscInjection"), 1)
        self.assertIn("Complete-SingleEscInjection $escProcess", self.text)
        self.assertIn("waits for the frozen app", self.text)
        self.assertIn("single_ESC_before_release=1", self.text)

    def test_success_requires_exact_authoritative_transition_and_cleanup(self) -> None:
        for needle in (
            "accepted=1 before=2 after=3 final=3 events=1",
            "read_errors=0 ptrace_errors=0 semantic_errors=0 unexpected_stops=0",
            "clean_detach=1",
            "target_memory_write_attempts=0 gameplay_writes=0",
            "TracerPid=0",
            "no retry",
        ):
            self.assertIn(needle, self.text)


if __name__ == "__main__":
    unittest.main()
