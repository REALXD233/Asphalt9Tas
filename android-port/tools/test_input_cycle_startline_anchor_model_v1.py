#!/usr/bin/env python3
"""Behavioral model for the final-writer-compatible source start anchor."""

from __future__ import annotations

import enum
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_synchronized_tick_recorder_v1.cpp").read_text(
    encoding="utf-8"
)


class Stage(enum.Enum):
    AWAIT_C98 = 1
    AWAIT_C9C = 2
    AWAIT_ZERO = 3
    READY = 4


class Anchor:
    def __init__(self) -> None:
        self.stage = Stage.AWAIT_C98
        self.owner = 0
        self.skipped_positive_deltas = 0

    def observe(self, event: str, tid: int, delta: int = 0) -> str:
        if self.stage is Stage.READY:
            return "frame0" if event == "delta" and delta > 0 else "ready"
        if event == "delta":
            if self.stage is Stage.AWAIT_ZERO and tid == self.owner and delta == 0:
                self.stage = Stage.READY
                self.owner = 0
                return "complete"
            if self.stage is Stage.AWAIT_C98 and delta >= 0:
                self.skipped_positive_deltas += int(delta > 0)
                return "ignored"
            return "rejected"
        if event == "c98" and self.stage is Stage.AWAIT_C98:
            self.stage = Stage.AWAIT_C9C
            self.owner = tid
            return "advanced"
        if event == "c9c" and self.stage is Stage.AWAIT_C9C and tid == self.owner:
            self.stage = Stage.AWAIT_ZERO
            return "advanced"
        if event == "c9c" and self.stage is Stage.AWAIT_C98:
            return "ignored-tail"
        return "rejected"


class InputCycleStartlineAnchorModelTests(unittest.TestCase):
    def test_canonical_witness_arms_exactly_the_next_positive_delta(self) -> None:
        anchor = Anchor()
        self.assertEqual(anchor.observe("delta", 11, 16667), "ignored")
        self.assertEqual(anchor.observe("c98", 71), "advanced")
        self.assertEqual(anchor.observe("c9c", 71), "advanced")
        self.assertEqual(anchor.observe("delta", 71, 0), "complete")
        self.assertEqual(anchor.observe("delta", 71, 16667), "frame0")
        self.assertEqual(anchor.skipped_positive_deltas, 1)

    def test_cross_thread_or_wrong_order_cannot_arm(self) -> None:
        anchor = Anchor()
        self.assertEqual(anchor.observe("c9c", 9), "ignored-tail")
        self.assertEqual(anchor.observe("c98", 10), "advanced")
        self.assertEqual(anchor.observe("c9c", 11), "rejected")
        self.assertEqual(anchor.observe("delta", 10, 0), "rejected")
        self.assertIsNot(anchor.stage, Stage.READY)

    def test_source_contains_each_guarded_transition_and_report_flag(self) -> None:
        for token in (
            "InputCycleSourceAnchorStage::kAwaitC98",
            "InputCycleSourceAnchorStage::kAwaitC9C",
            "InputCycleSourceAnchorStage::kAwaitDeltaZero",
            "InputCycleSourceAnchorStage::kReady",
            "tid == input_cycle_anchor_tid",
            "report.flags |= kSyncInputCycleAnchorWitnessed",
        ):
            self.assertIn(token, SOURCE)


if __name__ == "__main__":
    unittest.main()
