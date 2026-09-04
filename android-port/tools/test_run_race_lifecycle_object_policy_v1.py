#!/usr/bin/env python3
"""Offline safety policy for the guarded lifecycle-object host runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-race-lifecycle-object-v1.ps1"


class RaceLifecycleObjectRunnerPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = RUNNER.read_text(encoding="utf-8")

    def test_default_is_offline(self) -> None:
        self.assertIn('[string]$Mode = "OfflineValidate"', self.text)
        self.assertIn('controller_emitted=0 device_access=0', self.text)
        offline = self.text.index('if ($Mode -eq "OfflineValidate")')
        adb_check = self.text.index('Test-Path -LiteralPath $AdbPath')
        self.assertLess(offline, adb_check)

    def test_live_mode_is_read_only_and_triply_gated(self) -> None:
        for needle in (
            'AcknowledgeAncientRuinsZl1Countdown3Paused',
            'AcknowledgeReadOnlyProcessScan',
            'ExecuteExactlyOneAttempt',
            'countdown_candidates=1',
            ' state=2 ',
            'gameplay_writes=0 ptrace_calls=0',
            'Get-TracerPid',
            'Get-StartTicks',
            'Assert-RemoteHash',
        ):
            self.assertIn(needle, self.text)

    def test_no_gameplay_or_tracing_commands(self) -> None:
        for forbidden in (
            'input keyevent', 'input tap', 'am force-stop', 'am start',
            'ptrace(', 'PTRACE_', 'process_vm_writev', 'pwrite', 'ESC',
        ):
            self.assertNotIn(forbidden, self.text)


if __name__ == "__main__":
    unittest.main()
