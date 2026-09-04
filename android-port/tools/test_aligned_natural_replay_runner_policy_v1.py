#!/usr/bin/env python3
"""Static safety policy for the guarded aligned natural replay runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-hwbp-aligned-natural-replay-v1.ps1").read_text(
    encoding="utf-8"
)


class AlignedNaturalReplayRunnerPolicyTests(unittest.TestCase):
    def test_pins_binary_source_and_both_verifiers(self) -> None:
        for digest in (
            "6b69c6cab9f517102b733edb55ea5530eac097966b05c0da412ed26bac655f9f",
            "65f3b9cc3ed7bc1c3bc67dfa6fb4b66b641686bebfd78947529ecc1a87234be4",
            "5ff50df48f1412371628d8618ba185c54926b048da38a74820487bbe17e5a5ae",
            "8b67002d442dbc003e5a3bd39754fa7b01410ef1c5e1663888e5c1c8aa045391",
        ):
            self.assertIn(digest, RUNNER)

    def test_offline_validation_precedes_first_adb_call(self) -> None:
        self.assertLess(
            RUNNER.index("if ($OfflineValidateOnly)"),
            RUNNER.index("& $AdbPath devices"),
        )

    def test_requires_all_narrow_live_acknowledgements(self) -> None:
        for gate in (
            "$Gate9Validated",
            "$SynchronizedRecorderValidated",
            "$AcknowledgeFreshAlignedStartCapture",
            "$AcknowledgeFiveFixedDeltaWrites",
            "$AcknowledgeTenSteeringWrites",
            "$AcknowledgeUpToFiveTransformLinearCorrections",
            "$AcknowledgeFirstFrameGuardBeforePoseWrite",
            "$AcknowledgeFiveFrameGateNoTrajectoryClaim",
        ):
            self.assertIn(f"if (-not {gate})", RUNNER)

    def test_supports_exactly_one_start_mode(self) -> None:
        self.assertIn(
            "$startModeCount -ne 1",
            RUNNER,
        )
        self.assertIn("PAUSED_ANCHOR_ARMED", RUNNER)
        self.assertIn("120000", RUNNER)

    def test_host_trigger_waits_without_ptrace_then_rechecks_target(self) -> None:
        ready = RUNNER.index("HOST_TRIGGER_READY_NO_PTRACE")
        read = RUNNER.index("[Console]::ReadLine()", ready)
        accepted = RUNNER.index("HOST_TRIGGER_ACCEPTED", read)
        execute = RUNNER.index("& $AdbPath -s $Device shell", accepted)
        self.assertLess(ready, read)
        self.assertLess(read, accepted)
        self.assertLess(accepted, execute)
        self.assertIn("Assert-StableGameAndDetach", RUNNER[read:accepted])
        self.assertIn("$AcknowledgeResumeImmediatelyBeforeAttach", RUNNER)

    def test_paused_anchor_live_mode_is_quarantined_before_adb(self) -> None:
        quarantine = RUNNER.index("Paused-anchor live mode is quarantined")
        first_adb = RUNNER.index("& $AdbPath devices")
        self.assertLess(quarantine, first_adb)
        self.assertIn("tombstone_45", RUNNER)

    def test_requires_synchronized_source_and_guarded_result_verifiers(self) -> None:
        self.assertIn("synchronized_tick_recording_v1.py", RUNNER)
        self.assertIn("aligned_tick_replay_v1.py", RUNNER)
        self.assertIn("a9usr1_supported=1 frames=5 ticks=0\\.\\.4", RUNNER)

    def test_checks_remote_hash_pid_and_tracer(self) -> None:
        self.assertIn("Get-RemoteSha256", RUNNER)
        self.assertIn("Game already has a tracer", RUNNER)
        self.assertIn("left a tracer attached", RUNNER)
        self.assertIn("process exited or changed PID", RUNNER)

    def test_single_escape_resume_is_explicit_and_bounded(self) -> None:
        lowered = RUNNER.lower()
        self.assertIn("$AdbEscapeResumeImmediatelyBeforeAttach", RUNNER)
        self.assertIn("$AcknowledgeSingleEscapeResumeInput", RUNNER)
        self.assertEqual(lowered.count("shell input keyevent 111"), 1)
        self.assertNotIn("shell input tap", lowered)
        self.assertNotIn("shell input swipe", lowered)
        self.assertNotIn("motionevent", lowered)


if __name__ == "__main__":
    unittest.main()
