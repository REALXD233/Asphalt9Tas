#!/usr/bin/env python3
"""Offline semantics/source tests for the authoritative fixed-delta gate."""

from __future__ import annotations

import hashlib
import pathlib
import unittest

from test_authoritative_unified_adapter_v1 import AdapterModel, finish_tick
from unified_tick_executor_semantics_v1 import State


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE_PATH = (
    WORKSPACE / "android-port" / "src" / "hwbp_authoritative_fixed_delta_v1.cpp"
)
SOURCE = SOURCE_PATH.read_text(encoding="utf-8")
NEUTRAL_PATH = (
    WORKSPACE
    / "android-port"
    / "src"
    / "hwbp_authoritative_neutral_observer_v1.cpp"
)


class AuthoritativeFixedDeltaTests(unittest.TestCase):
    def test_five_packet_lifecycle_remains_exact(self) -> None:
        model = AdapterModel.create(frame_count=5, target_tick=4)
        for tick in range(5):
            finish_tick(model, tick, prefix=True)
        self.assertEqual(model.machine.state, State.COMPLETE)
        self.assertEqual((model.machine.frame_index, model.next_tick), (5, 5))
        self.assertFalse(model.control.block_mode_active)

    def test_exactly_one_narrow_pwrite_primitive_exists(self) -> None:
        self.assertEqual(SOURCE.count("pwrite("), 1)
        self.assertIn("pwrite(mem, &fixed_delta_us, sizeof(fixed_delta_us)", SOURCE)
        self.assertIn("(delta_address & 7u) != 0", SOURCE)
        self.assertIn("verify == fixed_delta_us", SOURCE)
        self.assertNotIn("WriteExactVerified", SOURCE)
        self.assertNotIn("process_vm_writev", SOURCE)

    def test_only_call_site_targets_delta_address(self) -> None:
        self.assertEqual(SOURCE.count("WriteFixedDeltaVerified("), 2)
        call = SOURCE.index("if (!WriteFixedDeltaVerified(")
        window = SOURCE[call : call + 300]
        self.assertIn("mem, delta_address", window)
        self.assertIn("fixed_interval_us", window)

    def test_every_other_packet_capability_is_skipped(self) -> None:
        self.assertIn("a9tas::unified_tick_v1::kSupportedSkipMask", SOURCE)
        self.assertIn("kAllOtherCapabilitiesSkipped", SOURCE)
        self.assertIn("report.gameplay_action_calls == 0", SOURCE)
        for token in ("ApplySteering", "RemoteCall", "NitroEnable"):
            self.assertNotIn(token, SOURCE)

    def test_success_requires_exactly_five_verified_writes(self) -> None:
        self.assertIn("report.target_memory_write_attempts == kFrameCount", SOURCE)
        self.assertIn("report.delta_writes == kFrameCount", SOURCE)
        self.assertIn("report.delta_write_failures == 0", SOURCE)

    def test_write_neutral_live_pass_source_remains_frozen(self) -> None:
        digest = hashlib.sha256(NEUTRAL_PATH.read_bytes()).hexdigest()
        self.assertEqual(
            digest,
            "3ad4aac949fa9fdc74ec517e7fb5bdd3f843d5a0d460e6a9a5fcf0fa9242058d",
        )


if __name__ == "__main__":
    unittest.main()
