#!/usr/bin/env python3
"""Static safety policy for the host-triggered natural action window."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-hwbp-natural-action-window-source-v1.ps1").read_text(
    encoding="utf-8"
)
TRIGGER = (ROOT / "trigger-hwbp-natural-action-window-v1.ps1").read_text(
    encoding="utf-8"
)
BUILD = (ROOT / "build-hwbp-natural-action-window-recorder-v1.ps1").read_text(
    encoding="utf-8"
)


class NaturalActionWindowRunnerPolicyTests(unittest.TestCase):
    def test_build_is_triple_macro_isolated(self) -> None:
        for macro in (
            '"-DA9TAS_NATURAL_PREROLL_ANCHOR_V1"',
            '"-DA9TAS_SYNC_BRAKE_CAPTURE_V1"',
            '"-DA9TAS_SYNC_ACTION_WINDOW_V1"',
        ):
            self.assertIn(macro, BUILD)

    def test_runner_pins_binary_source_and_all_verifiers(self) -> None:
        for digest in (
            "809c0d762400bc1a89d8ac095c5b421fc698ec8dee0af88767114c857b9add85",
            "65a4bdfc3debd5df302f0d41c76e82a028c09b5044ab00ce19052f7a13cfbde2",
            "22047bd85181a479ab22d97f5f88a95daca67a953d356c1113b54a5dcc40434d",
            "532527b786b2bb92a7041afb5ddeca7845ba4b99bbdbc1627cb269fbb0f537bf",
            "5afbcba34c50995712aa13c48e38596cad6ab354c77a875c0509e29d20b48237",
        ):
            self.assertIn(digest, RUNNER)
        self.assertLess(
            RUNNER.index("if ($OfflineValidateOnly)"),
            RUNNER.index("& $AdbPath devices"),
        )

    def test_runner_requires_exact_live_scope_and_180_frames(self) -> None:
        self.assertIn("$targetFrames = 180", RUNNER)
        for gate in (
            "$Gate10Validated",
            "$BrakeSourceLiveValidated",
            "$AcknowledgeHostTriggerWaitHasZeroGameplayWrites",
            "$AcknowledgeNeutralControlAnchor",
            "$Acknowledge180FixedDeltaWrites",
            "$AcknowledgeReadOnlySteeringBrakeCapture",
            "$AcknowledgeNoControlOrPhysicsWrites",
            "$AcknowledgeExtendedPtraceStallRisk",
        ):
            self.assertIn(gate, RUNNER)
        self.assertIn("$syncVerifier $ReportOutputPath $RecordingOutputPath", RUNNER)
        self.assertIn("$anchorVerifier $ReportOutputPath $RecordingOutputPath", RUNNER)
        self.assertIn("Assert-CleanDetach", RUNNER)

    def test_trigger_is_external_to_game_and_requires_attached_recorder(self) -> None:
        self.assertIn("AcknowledgeNeutralControlsAndStart180FrameWindow", TRIGGER)
        self.assertIn("AcknowledgeNeutralControlsAndStartActionCapture", TRIGGER)
        self.assertIn("TracerPid", TRIGGER)
        self.assertIn("a9tas_action_window_start_", TRIGGER)
        self.assertIn("touch $triggerPath", TRIGGER)
        self.assertIn("state=consumed", TRIGGER)
        for script in (RUNNER, TRIGGER):
            lowered = script.lower()
            self.assertNotIn("shell input", lowered)
            self.assertNotIn("keyevent", lowered)
            self.assertNotIn("motionevent", lowered)


if __name__ == "__main__":
    unittest.main()
