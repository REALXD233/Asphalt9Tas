#!/usr/bin/env python3
"""Static fail-closed policy for the natural pre-roll replay executor."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_unified_tick_executor_v1.cpp").read_text(
    encoding="utf-8"
)
BUILD = (ROOT / "build-hwbp-natural-preroll-replay-v1.ps1").read_text(
    encoding="utf-8"
)


class NaturalPrerollReplayPolicyTests(unittest.TestCase):
    def test_is_compile_time_isolated_and_keeps_first_frame_guard(self) -> None:
        self.assertIn('"-DA9TAS_NATURAL_PREROLL_REPLAY_V1"', BUILD)
        self.assertIn('"-DA9TAS_REPLAY_ALIGNMENT_GUARD_V1"', BUILD)
        self.assertIn("a9tas_hwbp_natural_preroll_replay_v1", BUILD)
        self.assertIn("first_frame_alignment_guard", SOURCE)

    def test_bound_anchor_is_loaded_before_target_process_is_opened(self) -> None:
        load = SOURCE.index("LoadNaturalPrerollAnchor(argv[5]")
        verify_target = SOURCE.index("VerifyTargetBuild(pid, base)")
        open_mem = SOURCE.index("open(mem_path, O_RDWR")
        self.assertLess(load, verify_target)
        self.assertLess(load, open_mem)
        self.assertIn("kBoundFlags", SOURCE[:verify_target])

    def test_search_delta_branch_has_no_gameplay_write(self) -> None:
        branch = SOURCE.index("if (searching_anchor) {", SOURCE.index("pending.delta_event"))
        end = SOURCE.index("} else", branch)
        search = SOURCE[branch:end]
        self.assertIn("pending.applied_delta_us = 0", search)
        self.assertNotIn("WriteExactVerified", search)
        self.assertNotIn("ApplySteering", search)

    def test_search_reads_steering_pair_without_applying_it(self) -> None:
        marker = SOURCE.index("if (searching_anchor) {", SOURCE.index("bool steering_ok"))
        end = SOURCE.index("} else", marker)
        search = SOURCE[marker:end]
        self.assertIn("ReadExact", search)
        self.assertNotIn("ApplySteering", search)

    def test_match_is_committed_before_frame0_machine_is_enabled(self) -> None:
        commit = SOURCE.index("pending.world_commit_event")
        matched = SOURCE.index("if (pending_anchor_match)", commit)
        enable = SOURCE.index("Stage::kWaiting, 0, frames.size()", matched)
        self.assertLess(commit, matched)
        self.assertLess(matched, enable)
        self.assertIn("searching_anchor = false", SOURCE[matched:enable])

    def test_search_cycles_are_persisted_with_zero_write_audits(self) -> None:
        push = SOURCE.index("anchor_search_audits.push_back(pending)")
        reset = SOURCE.index("pending = {}", push)
        self.assertLess(push, reset)
        self.assertIn("NaturalSearchReportHeaderV1", SOURCE)
        self.assertIn("A9NPR1", SOURCE)
        self.assertIn("kNaturalSearchZeroGameplayWrites", SOURCE)
        self.assertIn("WriteNaturalSearchReport", SOURCE)

    def test_timeout_success_requires_a_committed_anchor_match(self) -> None:
        self.assertIn(
            "success = success && !searching_anchor && anchor_search_cycles > 0",
            SOURCE,
        )
        self.assertIn("search_gameplay_writes=0", SOURCE)

    def test_build_keeps_warnings_as_errors(self) -> None:
        self.assertIn('"-Wall", "-Wextra", "-Werror"', BUILD)


if __name__ == "__main__":
    unittest.main()
