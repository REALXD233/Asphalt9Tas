#!/usr/bin/env python3
"""Static safety policy for the guarded synchronized capture runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-hwbp-synchronized-tick-recorder-v1.ps1").read_text(
    encoding="utf-8"
)


class SynchronizedTickRunnerPolicyTests(unittest.TestCase):
    def test_pins_reviewed_binary_source_and_verifier(self) -> None:
        for digest in (
            "30bc9b0d8b16a372e3e85eddd8d8a51bd1eee684b7bc7fb04bc80fca58e12a93",
            "976b5c73c3de2544c7b79dfba33cf759f8b10af53b287da23e0decab43b676e8",
            "65a4bdfc3debd5df302f0d41c76e82a028c09b5044ab00ce19052f7a13cfbde2",
            "5ff50df48f1412371628d8618ba185c54926b048da38a74820487bbe17e5a5ae",
        ):
            self.assertIn(digest, RUNNER)

    def test_offline_validation_precedes_first_adb_call(self) -> None:
        self.assertLess(
            RUNNER.index("if ($OfflineValidateOnly)"),
            RUNNER.index("& $AdbPath devices"),
        )

    def test_requires_narrow_live_acknowledgements(self) -> None:
        for gate in (
            "$Gate9Validated",
            "$AcknowledgeFiveFixedDeltaWrites",
            "$AcknowledgeCaptureOnlyNoControlOrPhysicsWrites",
            "$AcknowledgeFiveFrameCaptureCanStallGame",
            "$AcknowledgeNaturalSameRaceFrames",
        ):
            self.assertIn(f"if (-not {gate})", RUNNER)

    def test_supports_exactly_one_start_mode(self) -> None:
        self.assertIn(
            "$startModeCount -ne 1",
            RUNNER,
        )
        self.assertIn("PAUSED_ANCHOR_ARMED", RUNNER)
        self.assertIn("120000", RUNNER)
        self.assertIn("a9tas_hwbp_paused_anchor_recorder_v1", RUNNER)

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

    def test_fixed_scope_and_strict_cross_verifier(self) -> None:
        self.assertIn("$targetFrames = 5", RUNNER)
        self.assertIn("I_ACCEPT_SYNC_RECORDER_FIXED_DELTA_V1", RUNNER)
        self.assertIn("synchronized_tick_recording_v1.py", RUNNER)
        self.assertIn("writes=fixed-delta-only", RUNNER)

    def test_checks_tracer_before_and_after(self) -> None:
        self.assertIn("Game already has a tracer", RUNNER)
        self.assertIn("left a tracer attached", RUNNER)

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
