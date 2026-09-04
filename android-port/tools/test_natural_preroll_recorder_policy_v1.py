#!/usr/bin/env python3
"""Static safety policy for the natural pre-roll source recorder."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_synchronized_tick_recorder_v1.cpp").read_text(
    encoding="utf-8"
)
HEADER = (ROOT / "src" / "natural_preroll_anchor_v1.h").read_text(
    encoding="utf-8"
)
BUILD = (ROOT / "build-hwbp-natural-preroll-recorder-v1.ps1").read_text(
    encoding="utf-8"
)


class NaturalPrerollRecorderPolicyTests(unittest.TestCase):
    def test_is_compile_time_isolated_from_live_proven_recorder(self) -> None:
        self.assertIn("A9TAS_NATURAL_PREROLL_ANCHOR_V1", SOURCE)
        self.assertIn('"-DA9TAS_NATURAL_PREROLL_ANCHOR_V1"', BUILD)
        self.assertIn("a9tas_hwbp_natural_preroll_recorder_v1", BUILD)

    def test_warmup_is_read_only_and_only_fixed_delta_write_remains(self) -> None:
        self.assertEqual(SOURCE.count("WriteExactVerified("), 1)
        write = SOURCE[SOURCE.index("WriteExactVerified(") :]
        self.assertIn("mem, delta_address, &fixed_delta", write[:240])
        self.assertIn("else if (warmup_pending)", SOURCE)
        self.assertIn("leave the candidate delta", SOURCE)

    def test_anchor_is_certified_at_world_commit_before_recorded_frame0(self) -> None:
        world = SOURCE.index("pending_audit.world_commit_event")
        anchor = SOURCE.index("natural_anchor.cycle_tid", world)
        captured = SOURCE.index("natural_anchor_captured = true", anchor)
        reset = SOURCE.index("machine = Machine", captured)
        frame0 = SOURCE.index("frames.front().transform_bits", reset)
        self.assertLess(world, anchor)
        self.assertLess(anchor, captured)
        self.assertLess(captured, reset)
        self.assertLess(reset, frame0)

    def test_raw_anchor_is_unbound_and_exclusively_created(self) -> None:
        self.assertIn("kUnboundFlags", SOURCE)
        self.assertIn("O_EXCL", SOURCE)
        self.assertIn("recording_sha256[32]", HEADER)
        self.assertIn("report_sha256[32]", HEADER)
        self.assertIn('static_assert(sizeof(AnchorV1) == 424', HEADER)

    def test_build_keeps_warnings_as_errors(self) -> None:
        self.assertIn('"-Wall", "-Wextra", "-Werror"', BUILD)


if __name__ == "__main__":
    unittest.main()
