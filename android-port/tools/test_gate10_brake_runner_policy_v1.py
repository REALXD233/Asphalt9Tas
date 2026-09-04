#!/usr/bin/env python3
import pathlib
import re
import unittest

ROOT=pathlib.Path(__file__).resolve().parents[1]
RUNNER=(ROOT/"run-gate10-unified-brake-v1.ps1").read_text(encoding="utf-8")

class Gate10RunnerPolicyTests(unittest.TestCase):
    def test_pins_binary_source_input_and_verifiers(self):
        for digest in (
            "8f21c4820900876c2e7aa5b0c11e7df3768210f81188b27f2bbb8a17736b736f",
            "65f3b9cc3ed7bc1c3bc67dfa6fb4b66b641686bebfd78947529ecc1a87234be4",
            "d66d6772c48b86be17c67ffd935ca954ca2a74323b18ad9bf905de71d2310f45",
            "9ea083241553ff30bb454ebb38f099ef85c9998dbf0bd24834441e6df209474c",
            "adfe3640515ba8fd67fdf940637cde4ee77fd1a5c5e1e1ac500637cf0d248523",
        ): self.assertIn(digest,RUNNER)
    def test_no_input_or_automatic_resume(self):
        lowered=RUNNER.lower();self.assertNotIn("shell input",lowered);self.assertNotIn("keyevent",lowered);self.assertIsNone(re.search(r"\$pid\b",RUNNER,re.I))
    def test_live_acknowledgements_are_explicit(self):
        for token in ("AcknowledgeTwoBrakePairWrites","AcknowledgeRawNegativeOneBrake","AcknowledgeTransformAndOtherActionsSkipped","AcknowledgeRunningRaceNoManualInput"):self.assertIn(token,RUNNER)
    def test_verification_and_clean_detach_are_required(self):
        self.assertIn("Gate 10 report verification failed",RUNNER);self.assertIn("TracerPid=0",RUNNER);self.assertIn("failed closed",RUNNER)

if __name__=="__main__":unittest.main()
