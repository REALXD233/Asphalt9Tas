#!/usr/bin/env python3
"""Static safety policy for the natural pre-roll replay runner."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-hwbp-natural-preroll-replay-v1.ps1").read_text(encoding="utf-8")


class NaturalPrerollReplayRunnerPolicyTests(unittest.TestCase):
    def test_pins_all_execution_and_verification_artifacts(self) -> None:
        for digest in (
            "978d791af0a0d3e617b1beacefc7ac2f53e083bb34f80057f2a96109e5502c32",
            "3c9eee24744b0abf8d6889d2262f71cc986ee8b60961533a6565047c5bd74750",
            "65f3b9cc3ed7bc1c3bc67dfa6fb4b66b641686bebfd78947529ecc1a87234be4",
            "dd832c6f7c849a5417bf1b07217b6f2b69235bc5481bbe22e8889a60776c4c75",
            "532527b786b2bb92a7041afb5ddeca7845ba4b99bbdbc1627cb269fbb0f537bf",
            "8b67002d442dbc003e5a3bd39754fa7b01410ef1c5e1663888e5c1c8aa045391",
            "38f658dfcac9f99e05c53b1c907378bd0adc29007eafe7b9d87c269bf2355c36",
        ):
            self.assertIn(digest, RUNNER)

    def test_source_and_anchor_validation_precede_offline_return_and_adb(self) -> None:
        source = RUNNER.index("$syncVerifier $SourceReportPath $RecordingPath")
        anchor = RUNNER.index("$anchorTool verify", source)
        offline = RUNNER.index("if($OfflineValidateOnly)", anchor)
        adb = RUNNER.index("& $AdbPath devices", offline)
        self.assertLess(source, anchor)
        self.assertLess(anchor, offline)
        self.assertLess(offline, adb)

    def test_search_and_replay_reports_are_both_strictly_verified(self) -> None:
        self.assertIn("$searchVerifier $SearchReportOutputPath $AnchorPath", RUNNER)
        self.assertIn("$replayVerifier $ReplayReportOutputPath $SourceReportPath $RecordingPath", RUNNER)
        self.assertIn("Assert-Clean", RUNNER)

    def test_sends_no_keyboard_or_touch_input(self) -> None:
        lowered = RUNNER.lower()
        self.assertNotIn("shell input", lowered)
        self.assertNotIn("keyevent", lowered)
        self.assertNotIn("motionevent", lowered)
        self.assertIsNone(re.search(r"\$pid\b", RUNNER, re.IGNORECASE))
        self.assertIn("$gamePid", RUNNER)

    def test_requires_separate_search_and_post_match_write_acknowledgements(self) -> None:
        for gate in (
            "$AcknowledgeReadOnlySearchUntilMatchOrTimeout",
            "$AcknowledgeFiveFixedDeltaWritesAfterMatch",
            "$AcknowledgeTenSteeringWritesAfterMatch",
            "$AcknowledgeUpToFivePhysicsCorrectionsAfterMatch",
            "$AcknowledgeFirstFrameGuard",
        ):
            self.assertIn(gate, RUNNER)
        self.assertIn("$AcknowledgeSearchOnlyNoGameplayWrites", RUNNER)

    def test_report_collection_uses_named_non_flattening_pairs(self) -> None:
        self.assertIn("[pscustomobject]@{Remote=$remoteSearch;Local=$SearchReportOutputPath}", RUNNER)
        self.assertIn("[pscustomobject]@{Remote=$remoteReplay;Local=$ReplayReportOutputPath}", RUNNER)
        self.assertIn("foreach($pair in $reportPairs)", RUNNER)
        self.assertIn("$pair.Remote", RUNNER)
        self.assertIn("$pair.Local", RUNNER)
        self.assertNotIn("foreach($pair in $(if($SearchOnly)", RUNNER)


if __name__ == "__main__":
    unittest.main()
