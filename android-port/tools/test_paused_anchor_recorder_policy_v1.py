#!/usr/bin/env python3
"""Static safety tests for the paused-anchor natural warmup recorder."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_synchronized_tick_recorder_v1.cpp").read_text(
    encoding="utf-8"
)
BUILD = (ROOT / "build-hwbp-paused-anchor-recorder-v1.ps1").read_text(
    encoding="utf-8"
)


class PausedAnchorRecorderPolicyTests(unittest.TestCase):
    def test_warmup_is_compile_time_isolated(self) -> None:
        self.assertGreaterEqual(SOURCE.count("A9TAS_PAUSED_ANCHOR_WARMUP_V1"), 4)
        self.assertGreaterEqual(SOURCE.count("A9TAS_NATURAL_PREROLL_ANCHOR_V1"), 4)
        self.assertIn('"-DA9TAS_PAUSED_ANCHOR_WARMUP_V1"', BUILD)
        self.assertIn("a9tas_hwbp_paused_anchor_recorder_v1", BUILD)

    def test_warmup_candidate_has_no_fixed_delta_write(self) -> None:
        no_write = SOURCE.index("Deliberately leave the candidate delta")
        fixed_write = SOURCE.index("WriteExactVerified(", no_write)
        self.assertLess(no_write, fixed_write)
        self.assertIn("else if (warmup_pending)", SOURCE[no_write - 160 : no_write])

    def test_cancelled_paused_candidate_returns_to_waiting(self) -> None:
        marker = SOURCE.index("warmup_cancelled_cycle_rejected")
        branch = SOURCE[marker - 500 : marker + 500]
        self.assertIn("Event::kDeltaZero", branch)
        self.assertIn("cycle_tid = 0", branch)
        self.assertIn("have_pending_frame = false", branch)

    def test_complete_natural_warmup_is_discarded_before_recording(self) -> None:
        world = SOURCE.index("Event::kWorldCommit")
        append = SOURCE.index("frames.push_back", world)
        world_branch = SOURCE[world : append + 40]
        self.assertIn("warmup_pending = false", world_branch)
        self.assertIn("Stage::kWaiting, 0, target_frames", world_branch)
        self.assertLess(world_branch.index("warmup_pending = false"), world_branch.index("frames.push_back"))


if __name__ == "__main__":
    unittest.main()
