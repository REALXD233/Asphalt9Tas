#!/usr/bin/env python3
"""Static fail-closed policy for the guarded A9AST1 runner."""
from __future__ import annotations
import pathlib, unittest

ROOT=pathlib.Path(__file__).resolve().parents[2]
RUNNER=(ROOT/"android-port/run-authoritative-steering-v1.ps1").read_text(encoding="utf-8")

class RunnerPolicyTests(unittest.TestCase):
    def test_offline_return_precedes_all_device_access(self) -> None:
        offline=RUNNER.index("if ($OfflineValidateOnly)")
        self.assertLess(offline,RUNNER.index("Test-Path $AdbPath",offline))
        self.assertLess(offline,RUNNER.index("& $AdbPath",offline))

    def test_all_live_acknowledgements_are_required(self) -> None:
        for token in ("ExecuteExactlyOneLiveAttempt","AcknowledgeReviewedHashPinnedCandidate","AcknowledgeNaturallyRunningRaceNoManualInput","AcknowledgeSearchHasZeroGameplayWrites","AcknowledgeRecordingDeclaredFixedDeltaWrites","AcknowledgeTwiceFrameCountSteeringPairWrites","AcknowledgeAllOtherCapabilitiesAbsent","AcknowledgeLongPtraceStallAndCrashRisk"):
            self.assertGreaterEqual(RUNNER.count(token),2)

    def test_candidate_recording_anchor_and_review_files_are_hash_pinned(self) -> None:
        for digest in ("eb08b960fb34a06fbab069292b0f4a2e8f67896918ce3473214310f91329cf1f","ee3d710a2ed68f464d85485a26935f04c49a6632c00f517ced34b817f9078e3e","9ac31fa042f56e1fb4dc5af45df8ddb61e9ea229c850c2fbe8b974778ef4a986","1eae53cbc459da77d8cb7bfad07cece3639e668a1106b093dac184f59335dd4e"):
            self.assertIn(digest,RUNNER)

    def test_runner_cannot_drive_or_unpause_game(self) -> None:
        lower=RUNNER.lower()
        for token in ("input keyevent","input tap","input swipe","sendevent","keyevent 111"):
            self.assertNotIn(token,lower)

    def test_strict_report_and_detach_checks_are_mandatory(self) -> None:
        self.assertIn("python -B $validator $OutputPath",RUNNER)
        self.assertIn("python -B $validator --diagnostic $OutputPath",RUNNER)
        self.assertGreaterEqual(RUNNER.count("Get-Tracer"),4)
        self.assertIn("pidAfter",RUNNER)

    def test_exact_internal_acknowledgement_is_not_user_parameter(self) -> None:
        self.assertIn('$ack="I_ACCEPT_RECORDING_DECLARED_DELTA_AND_2X_STEERING_WRITES_V2"',RUNNER)
        self.assertNotIn("[string]$Acknowledgement",RUNNER)

    def test_expected_live_counts_must_match_recording_header(self) -> None:
        self.assertIn("ExpectedFrameCount -ne $frameCount",RUNNER)
        self.assertIn("ExpectedPairWriteCount -ne $pairCount",RUNNER)

if __name__=="__main__": unittest.main()
