#!/usr/bin/env python3
"""Static safety and semantic policy for the synchronized recorder."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_synchronized_tick_recorder_v1.cpp").read_text(
    encoding="utf-8"
)
BUILD = (ROOT / "build-hwbp-synchronized-tick-recorder-v1.ps1").read_text(
    encoding="utf-8"
)


class SynchronizedTickRecorderPolicyTests(unittest.TestCase):
    def test_reuses_proven_machine_without_second_main(self) -> None:
        self.assertIn("#define A9TAS_UNIFIED_TICK_EXECUTOR_NO_MAIN", SOURCE)
        self.assertIn('#include "hwbp_unified_tick_executor_v1.cpp"', SOURCE)

    def test_only_game_write_is_verified_fixed_delta(self) -> None:
        self.assertEqual(SOURCE.count("WriteExactVerified("), 1)
        write = SOURCE[SOURCE.index("WriteExactVerified(") :]
        self.assertIn("mem, delta_address, &fixed_delta", write[:240])
        for forbidden in (
            "ApplySteering(",
            "CorrectTransform",
            "process_vm_writev",
            "ptrace_singlestep",
        ):
            self.assertNotIn(forbidden, SOURCE)

    def test_requires_explicit_fixed_delta_acknowledgement(self) -> None:
        self.assertIn("I_ACCEPT_SYNC_RECORDER_FIXED_DELTA_V1", SOURCE)
        self.assertIn("fixed-delta-only", SOURCE)
        self.assertIn("kSyncRecordedSkipFlags == 0x7e", SOURCE)

    def test_captures_after_callback_and_commits_at_world_boundary(self) -> None:
        callback_case = SOURCE.index("machine.stage == Stage::kWaitCallbackClose")
        capture = SOURCE.index("ReadSynchronizedFrame(", callback_case)
        callback_advance = SOURCE.index("Event::kCallbackClose", capture)
        world_case = SOURCE.index("machine.stage == Stage::kWaitWorldCommit")
        world_advance = SOURCE.index("Event::kWorldCommit", world_case)
        append = SOURCE.index("frames.push_back(pending_frame)", world_advance)
        self.assertLess(callback_case, capture)
        self.assertLess(capture, callback_advance)
        self.assertLess(world_case, world_advance)
        self.assertLess(world_advance, append)
        for audit in (
            "completion_before",
            "completion_after",
            "callback_flags_at_c9c",
            "callback_deferred_clear_event",
            "cycle_tid",
            "commit_tid",
        ):
            self.assertIn(audit, SOURCE)

    def test_live_runner_is_separately_guarded(self) -> None:
        runner = (ROOT / "run-hwbp-synchronized-tick-recorder-v1.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn("This file is not authorization", runner)
        self.assertIn("$OfflineValidateOnly", runner)

    def test_single_escape_resume_is_explicit_and_bounded(self) -> None:
        runner = (ROOT / "run-hwbp-synchronized-tick-recorder-v1.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn("$AdbEscapeResumeImmediatelyBeforeAttach", runner)
        self.assertIn("$AcknowledgeSingleEscapeResumeInput", runner)
        self.assertEqual(runner.count("shell input keyevent 111"), 1)
        self.assertNotIn("shell input tap", runner)
        self.assertNotIn("shell input swipe", runner)

    def test_build_keeps_warnings_as_errors(self) -> None:
        self.assertIn('"-Wall", "-Wextra", "-Werror"', BUILD)
        self.assertIn("a9tas_hwbp_synchronized_tick_recorder_v1", BUILD)


if __name__ == "__main__":
    unittest.main()
