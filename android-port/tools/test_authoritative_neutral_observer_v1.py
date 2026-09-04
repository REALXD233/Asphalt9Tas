#!/usr/bin/env python3
"""Offline semantics and source-contract tests for the five-frame observer."""

from __future__ import annotations

import pathlib
import unittest

from test_authoritative_unified_adapter_v1 import AdapterModel, finish_tick
from unified_tick_executor_semantics_v1 import Event, State


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (
    WORKSPACE
    / "android-port"
    / "src"
    / "hwbp_authoritative_neutral_observer_v1.cpp"
).read_text(encoding="utf-8")


class AuthoritativeNeutralObserverTests(unittest.TestCase):
    def test_five_observed_cycles_bind_five_exact_packets(self) -> None:
        model = AdapterModel.create(frame_count=5, target_tick=4)
        for tick in range(5):
            finish_tick(model, tick, prefix=True)
            self.assertEqual(model.next_tick, tick + 1)
        self.assertEqual(model.machine.state, State.COMPLETE)
        self.assertEqual(model.machine.frame_index, 5)
        self.assertFalse(model.control.block_mode_active)

    def test_pause_before_c98_does_not_consume_first_packet(self) -> None:
        model = AdapterModel.create(frame_count=5, target_tick=4)
        self.assertIsNotNone(model.step(Event.DT_NONZERO))
        model.step(Event.DT_ZERO)
        self.assertEqual((model.machine.state, model.machine.frame_index),
                         (State.WAITING, 0))
        for tick in range(5):
            finish_tick(model, tick, prefix=True)
        self.assertEqual(model.next_tick, 5)

    def test_target_memory_is_opened_read_only(self) -> None:
        self.assertIn("open(mem_path, O_RDONLY | O_CLOEXEC)", SOURCE)
        self.assertNotIn("O_RDWR", SOURCE)
        self.assertNotIn("pwrite", SOURCE)
        self.assertNotIn("WriteExactVerified", SOURCE)

    def test_runtime_uses_authoritative_adapter_not_a_shadow_machine(self) -> None:
        self.assertIn('#include "authoritative_unified_adapter_v1.cpp"', SOURCE)
        self.assertIn("InitializeAdapter(&authoritative", SOURCE)
        self.assertIn("AdvanceAdapter(", SOURCE)
        self.assertIn("authoritative.committed_frames == kNeutralFrameCount", SOURCE)

    def test_all_synthetic_packets_skip_every_mutating_capability(self) -> None:
        self.assertIn(
            "frames[index].skip_override_flags =\n"
            "            a9tas::unified_tick_v1::kSupportedSkipMask;",
            SOURCE,
        )
        self.assertIn("target_memory_write_attempts == 0", SOURCE)
        self.assertIn("gameplay_action_calls == 0", SOURCE)

    def test_dynamic_hwbp_layout_is_reused(self) -> None:
        self.assertIn("BoundaryDr7(true)", SOURCE)
        self.assertIn("PipelineDr7()", SOURCE)
        self.assertIn("ProgramStoppedThreadNeutral(", SOURCE)
        self.assertIn("RearmPostOwnerForInput(", SOURCE)


if __name__ == "__main__":
    unittest.main()
