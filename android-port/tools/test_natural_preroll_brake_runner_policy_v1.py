#!/usr/bin/env python3
"""Static safety policy for natural brake source and replay runners."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_RUNNER = (ROOT / "run-hwbp-natural-preroll-brake-source-v1.ps1").read_text(
    encoding="utf-8"
)
REPLAY_RUNNER = (ROOT / "run-hwbp-natural-preroll-brake-replay-v1.ps1").read_text(
    encoding="utf-8"
)
RECORDER_BUILD = (ROOT / "build-hwbp-natural-preroll-brake-recorder-v1.ps1").read_text(
    encoding="utf-8"
)
REPLAY_BUILD = (ROOT / "build-hwbp-natural-preroll-brake-replay-v1.ps1").read_text(
    encoding="utf-8"
)


class NaturalPrerollBrakeRunnerPolicyTests(unittest.TestCase):
    def test_builds_are_isolated_by_both_required_macros(self) -> None:
        self.assertIn('"-DA9TAS_SYNC_BRAKE_CAPTURE_V1"', RECORDER_BUILD)
        self.assertIn('"-DA9TAS_NATURAL_PREROLL_ANCHOR_V1"', RECORDER_BUILD)
        self.assertIn('"-DA9TAS_UNIFIED_BRAKE_V1"', REPLAY_BUILD)
        self.assertIn('"-DA9TAS_NATURAL_PREROLL_REPLAY_V1"', REPLAY_BUILD)

    def test_source_pins_every_artifact_and_requires_narrow_scope(self) -> None:
        for digest in (
            "e2382d280dd46aece092c3a042adc6b1042d21a047d7fbe66b0a294433032614",
            "65a4bdfc3debd5df302f0d41c76e82a028c09b5044ab00ce19052f7a13cfbde2",
            "85cffee892202948a3064f4d89845dac96267545d9769dd39a36c100d5b4b251",
            "532527b786b2bb92a7041afb5ddeca7845ba4b99bbdbc1627cb269fbb0f537bf",
        ):
            self.assertIn(digest, SOURCE_RUNNER)
        for gate in (
            "$Gate10Validated",
            "$AcknowledgeRaceAlreadyRunning",
            "$AcknowledgeReadOnlyWaitUntilBrakePress",
            "$AcknowledgeFiveFixedDeltaWrites",
            "$AcknowledgeReadOnlySteeringBrakeCapture",
            "$AcknowledgeNoControlOrPhysicsWrites",
            "$AcknowledgeShortPtraceStallRisk",
        ):
            self.assertIn(gate, SOURCE_RUNNER)
        self.assertLess(
            SOURCE_RUNNER.index("if ($OfflineValidateOnly)"),
            SOURCE_RUNNER.index("& $AdbPath devices"),
        )

    def test_replay_pins_a9uer6_chain_and_separates_search_from_writes(self) -> None:
        for digest in (
            "836ba11bfa181a845ffd0dea478ae8de955bc6568e6b135b2a6a2d423312b798",
            "65f3b9cc3ed7bc1c3bc67dfa6fb4b66b641686bebfd78947529ecc1a87234be4",
            "22047bd85181a479ab22d97f5f88a95daca67a953d356c1113b54a5dcc40434d",
            "08361285e33d9d587406b37dcf97266d857b51cce9a1ec320f9f2a17dc657bba",
            "532527b786b2bb92a7041afb5ddeca7845ba4b99bbdbc1627cb269fbb0f537bf",
            "5afbcba34c50995712aa13c48e38596cad6ab354c77a875c0509e29d20b48237",
            "f15d23825d736206bd3fca4e0fd4ea841c8d96665e8a6514bb379b110139146c",
            "7df6868dd8e02b0e0647ba84d35b13e27fcba643d7bb8732896d128c60372289",
            "38f658dfcac9f99e05c53b1c907378bd0adc29007eafe7b9d87c269bf2355c36",
        ):
            self.assertIn(digest, REPLAY_RUNNER)
        for gate in (
            "$AcknowledgeReadOnlySearchUntilMatchOrTimeout",
            "$AcknowledgeFiveFixedDeltaWritesAfterMatch",
            "$AcknowledgeTenCombinedSteeringBrakePairWritesAfterMatch",
            "$AcknowledgeUpToFivePhysicsCorrectionsAfterMatch",
            "$AcknowledgeFirstFrameGuard",
            "$AcknowledgeShortPtraceStallRisk",
        ):
            self.assertIn(gate, REPLAY_RUNNER)
        for gate in (
            "$ActionWindowSourceLiveValidated",
            "$ActionUntilReleaseSourceLiveValidated",
            "$AcknowledgeRecordingFrameCountDeltaWritesAfterMatch",
            "$AcknowledgeTwiceFrameCountCombinedPairWritesAfterMatch",
            "$AcknowledgeUpToFrameCountPhysicsCorrectionsAfterMatch",
            "$AcknowledgeExtendedPtraceStallRisk",
        ):
            self.assertIn(gate, REPLAY_RUNNER)
        self.assertIn('$sourceMagic.StartsWith("A9USR3")', REPLAY_RUNNER)
        self.assertIn('$sourceMagic.StartsWith("A9USR4")', REPLAY_RUNNER)
        self.assertIn(
            "$recordingFrameCount = [BitConverter]::ToUInt32($recordingBytes, 20)",
            REPLAY_RUNNER,
        )
        self.assertIn(
            "$selectedSyncVerifier = if ($isActionRelease)", REPLAY_RUNNER
        )
        self.assertIn(
            "$actionAnchorVerifier $SourceReportPath $RecordingPath $AnchorPath",
            REPLAY_RUNNER,
        )
        self.assertIn("$searchVerifier $SearchReportOutputPath $AnchorPath", REPLAY_RUNNER)
        self.assertIn(
            "$replayVerifier $ReplayReportOutputPath $SourceReportPath $RecordingPath",
            REPLAY_RUNNER,
        )
        self.assertLess(
            REPLAY_RUNNER.index("if ($OfflineValidateOnly)"),
            REPLAY_RUNNER.index("& $AdbPath devices"),
        )

    def test_neither_runner_sends_game_input(self) -> None:
        for runner in (SOURCE_RUNNER, REPLAY_RUNNER):
            lowered = runner.lower()
            self.assertNotIn("shell input", lowered)
            self.assertNotIn("keyevent", lowered)
            self.assertNotIn("motionevent", lowered)
            self.assertIn("TracerPid", runner)


if __name__ == "__main__":
    unittest.main()
