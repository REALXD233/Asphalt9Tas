#!/usr/bin/env python3
"""Offline behavioral model and source policy for the start-line pause gate."""
from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "android-port/src/startline_pause_gate_v1.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "android-port/src/startline_pause_gate_v1.h").read_text(encoding="utf-8")


class Model:
    def __init__(self, initial: int):
        self.phase = "armed" if initial == 0 else "poisoned"
        self.zeros = int(initial == 0)
        self.triggers = 0

    def observe(self, value: int) -> tuple[bool, bool]:
        if self.phase != "armed":
            return False, False
        if value == 0:
            self.zeros += 1
            return True, False
        if value < 0 or value > 1_000_000:
            self.phase = "poisoned"
            return False, False
        self.phase = "triggered"
        self.triggers = 1
        return True, True


class StartlinePauseGateTests(unittest.TestCase):
    def test_requires_verified_paused_baseline(self) -> None:
        self.assertEqual(Model(0).phase, "armed")
        self.assertEqual(Model(1).phase, "poisoned")

    def test_first_positive_after_any_zero_run_fires_once(self) -> None:
        gate = Model(0)
        for _ in range(5):
            self.assertEqual(gate.observe(0), (True, False))
        self.assertEqual(gate.observe(16667), (True, True))
        self.assertEqual(gate.observe(16667), (False, False))
        self.assertEqual(gate.triggers, 1)

    def test_invalid_delta_poisons_without_trigger(self) -> None:
        for value in (-1, 1_000_001):
            gate = Model(0)
            self.assertEqual(gate.observe(value), (False, False))
            self.assertEqual((gate.phase, gate.triggers), ("poisoned", 0))

    def test_cpp_contract_is_one_shot_and_has_no_runtime_primitives(self) -> None:
        for token in ("initial_delta_us != 0", "kPausedArmed", "kTriggered", "fire_tick_zero", "trigger_count = 1"):
            self.assertIn(token, SOURCE + HEADER)
        for forbidden in ("ptrace", "pwrite", "process_vm_writev", "socket", "system("):
            self.assertNotIn(forbidden, SOURCE + HEADER)


if __name__ == "__main__":
    unittest.main()
