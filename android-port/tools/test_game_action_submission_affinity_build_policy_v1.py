#!/usr/bin/env python3

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "src" / "hwbp_game_action_submission_affinity_v1.cpp"
).read_text(encoding="utf-8")
BUILD = (
    ROOT / "build-hwbp-game-action-submission-affinity-v1.ps1"
).read_text(encoding="utf-8")
RUNNER = (
    ROOT / "run-game-action-submission-affinity-v1.ps1"
).read_text(encoding="utf-8")


class GameActionSubmissionAffinityBuildPolicyTests(unittest.TestCase):
    def test_four_read_only_observation_targets_are_pinned(self) -> None:
        self.assertIn("kCommandVectorOffset = 0x1360", SOURCE)
        self.assertIn("owner + kCommandVectorOffset + 8", SOURCE)
        self.assertIn("state + kActiveOffset", SOURCE)
        self.assertIn("state + kModeOffset", SOURCE)
        self.assertIn("main_object + kAccumulatorOffset", SOURCE)

    def test_debug_register_scope_has_no_game_mutation_api(self) -> None:
        for forbidden in (
            "mprotect(",
            "pwrite(",
            "PTRACE_POKEDATA",
            "PTRACE_SETREGS",
            "__builtin___clear_cache",
        ):
            self.assertNotIn(forbidden, SOURCE)
        self.assertIn("writes=debug-registers-only", SOURCE)
        self.assertIn("game_calls=0 input_writes=0", SOURCE)

    def test_queue_shape_and_completion_lifetime_are_observed(self) -> None:
        self.assertIn("output->begin <= output->end", SOURCE)
        self.assertIn("output->end <= output->capacity", SOURCE)
        self.assertIn("output->last_token + 0x11", SOURCE)
        self.assertIn("queue.count == previous_count + 1", SOURCE)
        self.assertIn("queue.count < previous_count", SOURCE)

    def test_every_thread_gets_the_same_four_watchpoints(self) -> None:
        call = (
            "pid, queue_end, active, mode, delta, &threads, &ptrace_errors,\n"
            "        SubmissionAffinityDr7()"
        )
        self.assertIn(call, SOURCE)
        self.assertIn("(2UL << 18)", SOURCE)
        self.assertIn("(3UL << 26)", SOURCE)
        self.assertIn("(2UL << 30)", SOURCE)

    def test_build_is_isolated(self) -> None:
        self.assertIn(
            "a9tas_hwbp_game_action_submission_affinity_v1", BUILD
        )
        self.assertIn("scope=observe-only", BUILD)

    def test_runner_defaults_to_true_offline_validation(self) -> None:
        self.assertIn('[string]$Mode = "OfflineValidateOnly"', RUNNER)
        offline = 'if ($Mode -eq "OfflineValidateOnly")'
        first_adb = '$pidText = (& $AdbPath'
        self.assertIn("device_access=0 attached=0 input_sent=0", RUNNER)
        self.assertLess(RUNNER.index(offline), RUNNER.index(first_adb))

    def test_runner_requires_manual_input_but_never_sends_it(self) -> None:
        for gate in (
            "AcknowledgeNaturallyRunningRace",
            "AcknowledgeExactlyOneManualSpacePress",
            "AcknowledgeNoPausedAttach",
            "AcknowledgeGuestMemoryReadOnly",
            "AcknowledgeDebugRegistersOnly",
            "AcknowledgeNoAutomatedInput",
            "AcknowledgeShortPtraceStallRisk",
        ):
            self.assertIn(gate, RUNNER)
        lowered = RUNNER.lower()
        for forbidden in ("input keyevent", "input tap", "force-stop", "am start"):
            self.assertNotIn(forbidden, lowered)

    def test_runner_pulls_and_strictly_parses_remote_transcript(self) -> None:
        self.assertIn("$remoteReport", RUNNER)
        self.assertIn("parse_game_action_submission_affinity_v1.py", RUNNER)
        self.assertIn("A9ASA1 strict verification failed", RUNNER)


if __name__ == "__main__":
    unittest.main()
