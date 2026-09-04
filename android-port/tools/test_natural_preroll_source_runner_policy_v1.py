#!/usr/bin/env python3
"""Static safety policy for the natural pre-roll source runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-hwbp-natural-preroll-source-v1.ps1").read_text(encoding="utf-8")


class NaturalPrerollSourceRunnerPolicyTests(unittest.TestCase):
    def test_pins_binary_source_header_and_both_verifiers(self) -> None:
        for digest in (
            "af414aec974b491a090c8ce70ae563651bb4fb2a0042b5f0849d5af3804d606c",
            "65a4bdfc3debd5df302f0d41c76e82a028c09b5044ab00ce19052f7a13cfbde2",
            "dd832c6f7c849a5417bf1b07217b6f2b69235bc5481bbe22e8889a60776c4c75",
            "5ff50df48f1412371628d8618ba185c54926b048da38a74820487bbe17e5a5ae",
            "532527b786b2bb92a7041afb5ddeca7845ba4b99bbdbc1627cb269fbb0f537bf",
        ):
            self.assertIn(digest, RUNNER)

    def test_offline_validation_precedes_adb(self) -> None:
        self.assertLess(RUNNER.index("if ($OfflineValidateOnly)"), RUNNER.index("& $AdbPath devices"))

    def test_live_requires_running_race_and_narrow_acknowledgements(self) -> None:
        for gate in (
            "$Gate9Validated", "$AcknowledgeRaceAlreadyRunning",
            "$AcknowledgeOneReadOnlyWarmupCycle", "$AcknowledgeFiveFixedDeltaWrites",
            "$AcknowledgeNoControlOrPhysicsWrites", "$AcknowledgeShortPtraceStallRisk",
        ):
            self.assertIn(f"if (-not {gate})", RUNNER)

    def test_sends_no_keyboard_or_touch_input(self) -> None:
        lowered = RUNNER.lower()
        self.assertNotIn("shell input", lowered)
        self.assertNotIn("keyevent", lowered)
        self.assertNotIn("motionevent", lowered)

    def test_binds_and_reverifies_anchor_after_cross_validation(self) -> None:
        sync = RUNNER.index("$syncVerifier $ReportOutputPath $RecordingOutputPath")
        bind = RUNNER.index("$anchorTool bind", sync)
        verify = RUNNER.index("$anchorTool verify", bind)
        self.assertLess(sync, bind)
        self.assertLess(bind, verify)
        self.assertIn("Assert-CleanDetach", RUNNER)


if __name__ == "__main__":
    unittest.main()
