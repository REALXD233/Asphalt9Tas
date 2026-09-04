#!/usr/bin/env python3
"""Offline policy tests for the capture-to-conditional Gate 5 handoff."""

from __future__ import annotations

import hashlib
import subprocess
import unittest
from pathlib import Path


ANDROID_PORT = Path(__file__).resolve().parents[1]
RUNNER = ANDROID_PORT / "run-gate5-capture-conditional-handoff-v1.ps1"
RECORDER = ANDROID_PORT / "build" / "staging" / "a9tas_hwbp_native_physics_recorder_v1"
CONDITIONAL = ANDROID_PORT / "build" / "staging" / "a9tas_hwbp_conditional_executor_v1"
RECORDER_HASH = "ba5748752644cdc8d6630f2f281532d5a80e8263f65bd2f09d38a5d257cc516a"
CONDITIONAL_HASH = "57c74e137144f6d8a65d94caf35f2fc0d2a9ccf6520d6c77d5412399ba1e3660"


class Gate5HandoffRunnerPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = RUNNER.read_text(encoding="utf-8")

    def test_pins_both_reviewed_binaries(self) -> None:
        self.assertEqual(hashlib.sha256(RECORDER.read_bytes()).hexdigest(), RECORDER_HASH)
        self.assertEqual(hashlib.sha256(CONDITIONAL.read_bytes()).hexdigest(), CONDITIONAL_HASH)
        self.assertIn(RECORDER_HASH, self.source)
        self.assertIn(CONDITIONAL_HASH, self.source)

    def test_runtime_guards_precede_adb_discovery(self) -> None:
        adb_start = self.source.index("$deviceLine =")
        for guard in (
            "if (-not $Gate2Validated)",
            "if (-not $SameBytesValidated)",
            "if (-not $AcknowledgeStartPausedThenResume)",
            "if (-not $AcknowledgeImmediateDifferentValueHandoff)",
            "if (-not $AcknowledgeRollbackIsBestEffort)",
        ):
            self.assertLess(self.source.index(guard), adb_start)

    def test_handoff_is_fixed_to_five_frames(self) -> None:
        self.assertIn("$targetFrames = 5", self.source)
        self.assertIn("--minimum-corrected 1", self.source)
        self.assertIn("--maximum-frames $targetFrames", self.source)

    def test_conditional_starts_directly_after_capture(self) -> None:
        capture = self.source.index("$recorderExitCode = $LASTEXITCODE")
        conditional = self.source.index("$remoteConditional $gamePid")
        between = self.source[capture:conditional]
        self.assertNotIn("pull $remoteRecording", between)
        self.assertNotIn("Start-Sleep", between)

    def test_offline_validation_never_needs_adb(self) -> None:
        result = subprocess.run(
            [
                "powershell.exe",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(RUNNER),
                "-OfflineValidateOnly",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("GATE5_HANDOFF_OFFLINE_VALIDATION_OK", result.stdout)
        self.assertIn("target_frames=5", result.stdout)

    def test_root_owned_report_is_hashed_then_made_pullable(self) -> None:
        self.assertIn("su -c 'sha256sum $RemotePath'", self.source)
        detached = self.source.rindex("Assert-StableGameAndDetach")
        report_hash = self.source.rindex("$remoteReportHash = Get-RemoteSha256")
        chmod = self.source.index("chmod 644 $remoteReport")
        pull = self.source.index("pull $remoteReport $ReportOutputPath")
        self.assertLess(detached, report_hash)
        self.assertLess(report_hash, chmod)
        self.assertLess(chmod, pull)


if __name__ == "__main__":
    unittest.main()
