#!/usr/bin/env python3
"""Static safety policy for the bounded A9USR4 source runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-hwbp-natural-action-until-release-source-v1.ps1").read_text(
    encoding="utf-8"
)
BUILD = (ROOT / "build-hwbp-natural-action-until-release-recorder-v1.ps1").read_text(
    encoding="utf-8"
)


class NaturalActionUntilReleaseRunnerPolicyTests(unittest.TestCase):
    def test_build_is_isolated_by_all_required_macros(self) -> None:
        for macro in (
            '"-DA9TAS_NATURAL_PREROLL_ANCHOR_V1"',
            '"-DA9TAS_SYNC_BRAKE_CAPTURE_V1"',
            '"-DA9TAS_SYNC_ACTION_WINDOW_V1"',
            '"-DA9TAS_SYNC_ACTION_UNTIL_RELEASE_V1"',
        ):
            self.assertIn(macro, BUILD)

    def test_runner_pins_artifacts_and_requires_expanded_authorization(self) -> None:
        for digest in (
            "e1630d2e539054a0b86438fab5c6bbe95c95989294070c6d68cef7824a378659",
            "65a4bdfc3debd5df302f0d41c76e82a028c09b5044ab00ce19052f7a13cfbde2",
            "08361285e33d9d587406b37dcf97266d857b51cce9a1ec320f9f2a17dc657bba",
            "532527b786b2bb92a7041afb5ddeca7845ba4b99bbdbc1627cb269fbb0f537bf",
            "5afbcba34c50995712aa13c48e38596cad6ab354c77a875c0509e29d20b48237",
        ):
            self.assertIn(digest, RUNNER)
        for gate in (
            "$AcknowledgeA9USR3TimingFailure",
            "$AcknowledgeUpTo3600FixedDeltaWrites",
            "$AcknowledgeStopsOnlyAfterActionReleaseAnd30Frames",
            "$AcknowledgeNoControlOrPhysicsWrites",
            "$AcknowledgeExtendedPtraceStallRisk",
        ):
            self.assertIn(gate, RUNNER)
        self.assertIn("$maximumFrames = 3600", RUNNER)
        self.assertLess(RUNNER.index("if ($OfflineValidateOnly)"), RUNNER.index("& $AdbPath devices"))

    def test_runner_has_only_one_explicitly_gated_game_input(self) -> None:
        lowered = RUNNER.lower()
        self.assertEqual(lowered.count("shell input"), 1)
        self.assertEqual(lowered.count("keyevent 111"), 1)
        self.assertNotIn("motionevent", lowered)
        self.assertGreaterEqual(RUNNER.count("AdbEscapeResumeImmediatelyBeforeAttach"), 4)
        self.assertGreaterEqual(RUNNER.count("AcknowledgeSingleEscapeResumeInput"), 3)
        self.assertLess(
            RUNNER.index("shell input keyevent 111"),
            RUNNER.index("'$remoteBinary $gamePid"),
        )
        self.assertIn("TracerPid", RUNNER)


if __name__ == "__main__":
    unittest.main()
