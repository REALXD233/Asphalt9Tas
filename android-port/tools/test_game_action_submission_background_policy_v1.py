#!/usr/bin/env python3

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = (
    ROOT / "start-game-action-submission-affinity-background-v1.ps1"
).read_text(encoding="utf-8")


class SubmissionBackgroundPolicyTests(unittest.TestCase):
    def test_hidden_background_process_and_armed_handshake(self) -> None:
        self.assertIn("Start-Process", LAUNCHER)
        self.assertIn("-WindowStyle Hidden", LAUNCHER)
        self.assertIn("GAME_ACTION_SUBMISSION_AFFINITY_V1_ARMED", LAUNCHER)
        self.assertIn("GAME_ACTION_SUBMISSION_BACKGROUND_ARMED", LAUNCHER)

    def test_never_sends_input_or_mutates_game(self) -> None:
        lowered = LAUNCHER.lower()
        for forbidden in (
            "input keyevent",
            "input tap",
            "force-stop",
            "am start",
            "pwrite",
        ):
            self.assertNotIn(forbidden, lowered)
        self.assertIn("input_sent=0", LAUNCHER)

    def test_requires_all_live_acknowledgements(self) -> None:
        for gate in (
            "AcknowledgeNaturallyRunningRace",
            "AcknowledgeExactlyOneManualSpacePress",
            "AcknowledgeNoPausedAttach",
            "AcknowledgeGuestMemoryReadOnly",
            "AcknowledgeDebugRegistersOnly",
            "AcknowledgeNoAutomatedInput",
            "AcknowledgeShortPtraceStallRisk",
        ):
            self.assertIn(gate, LAUNCHER)


if __name__ == "__main__":
    unittest.main()
