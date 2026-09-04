#!/usr/bin/env python3
"""Source policy for direct start-line authoritative steering replay."""
from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "android-port/src/hwbp_authoritative_steering_v1.cpp").read_text(encoding="utf-8")
BUILD = (ROOT / "android-port/build-authoritative-steering-startline-v1.ps1").read_text(encoding="utf-8")


class AuthoritativeSteeringStartlinePolicyTests(unittest.TestCase):
    def test_startline_mode_is_distinct_from_anchor_search(self) -> None:
        self.assertIn("A9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT", SOURCE)
        self.assertIn("search-only and startline-direct modes are mutually exclusive", SOURCE)
        self.assertIn("I_ACCEPT_STARTLINE_DIRECT_DELTA_AND_2X_STEERING_WRITES_V1", SOURCE)
        self.assertIn("!PrimeDirectReplay(controller, &state)", SOURCE)
        self.assertIn("No A9NPA1 comparator is used", SOURCE)

    def test_prearm_opens_target_read_only_until_resume(self) -> None:
        direct_open = SOURCE.index("int mem = open(mem_path, O_RDONLY")
        ready = SOURCE.index("STARTLINE_READY_NO_ATTACH", direct_open)
        resume = SOURCE.index("STARTLINE_RESUME_OBSERVED", ready)
        write_open = SOURCE.index("mem = open(mem_path, O_RDWR", ready)
        attach = SOURCE.index("AttachNewThreads(", write_open)
        self.assertLess(direct_open, ready)
        self.assertLess(ready, write_open)
        self.assertLess(write_open, resume)
        self.assertLess(resume, attach)

    def test_direct_priming_selects_no_packet_before_live_delta(self) -> None:
        self.assertIn("state->mode == ModeV1::kReplay", SOURCE)
        self.assertIn("!state->replay.packet_selected", SOURCE)
        self.assertIn("alignment=startline_first_complete_cycle", SOURCE)

    def test_build_enables_only_startline_variant(self) -> None:
        self.assertIn("-DA9TAS_AUTHORITATIVE_STEERING_STARTLINE_DIRECT=1", BUILD)
        self.assertNotIn("-DA9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY", BUILD)


if __name__ == "__main__":
    unittest.main()
