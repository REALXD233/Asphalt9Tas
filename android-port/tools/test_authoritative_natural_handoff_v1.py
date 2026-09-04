#!/usr/bin/env python3
"""Offline semantics tests for natural-search to authoritative-tick handoff."""

from __future__ import annotations

import pathlib
import unittest

from authoritative_tick_state_machine_v1 import SelectionOutcome
from test_authoritative_unified_adapter_v1 import AdapterModel
from unified_tick_executor_semantics_v1 import Event


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (WORKSPACE / "android-port" / "src" / "authoritative_natural_handoff_v1.cpp").read_text(encoding="utf-8")


class HandoffModel:
    def __init__(self) -> None:
        self.searching = True
        self.search_cycles = 0
        self.matched_cycle: int | None = None
        self.replay = AdapterModel.create(344, 343)

    def commit_search_cycle(self, matched: bool) -> None:
        if not self.searching:
            raise ValueError("search already completed")
        if matched:
            self.matched_cycle = self.search_cycles
            self.searching = False
        self.search_cycles += 1

    def next_positive_delta(self) -> SelectionOutcome:
        if self.searching:
            raise ValueError("anchor not matched")
        return self.replay.step(Event.DT_NONZERO)


class AuthoritativeNaturalHandoffTests(unittest.TestCase):
    def test_matched_world_commit_does_not_select_tick_zero(self) -> None:
        model = HandoffModel()
        model.commit_search_cycle(False)
        model.commit_search_cycle(True)
        self.assertEqual((model.search_cycles, model.matched_cycle), (2, 1))
        self.assertIsNone(model.replay.selected_tick)
        self.assertEqual(model.next_positive_delta(), SelectionOutcome.EXACT_PACKET)
        self.assertEqual(model.replay.selected_tick, 0)

    def test_cancelled_search_prefix_does_not_consume_replay(self) -> None:
        model = HandoffModel()
        for _ in range(20):
            self.assertIsNone(model.replay.selected_tick)
        model.commit_search_cycle(True)
        self.assertEqual(model.next_positive_delta(), SelectionOutcome.EXACT_PACKET)
        self.assertEqual(model.replay.selected_tick, 0)

    def test_rejected_cycles_do_not_shift_first_packet(self) -> None:
        model = HandoffModel()
        for _ in range(100):
            model.commit_search_cycle(False)
        model.commit_search_cycle(True)
        self.assertEqual(model.next_positive_delta(), SelectionOutcome.EXACT_PACKET)
        self.assertEqual(model.replay.selected_tick, 0)

    def test_cpp_handoff_is_offline_and_does_not_forward_match_commit(self) -> None:
        self.assertIn("state->mode = ModeV1::kReplay", SOURCE)
        self.assertIn("output.replay = authoritative_bridge_v1::Advance", SOURCE)
        search_branch = SOURCE.index("if (controller->mode == ModeV1::kSearching)")
        replay_branch = SOURCE.index("output.replay =", search_branch)
        self.assertLess(search_branch, replay_branch)
        for forbidden in ("ptrace(", "pwrite(", "pread(", "/proc/", "RemoteCall"):
            self.assertNotIn(forbidden, SOURCE)


if __name__ == "__main__":
    unittest.main()

