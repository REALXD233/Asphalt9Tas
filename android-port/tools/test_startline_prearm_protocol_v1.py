#!/usr/bin/env python3
"""Offline behavior and source policy for the start-line prearm protocol."""
from __future__ import annotations

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "android-port/src/startline_prearm_protocol_v1.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "android-port/src/startline_prearm_protocol_v1.h").read_text(encoding="utf-8")


class Model:
    def __init__(self, initial_delta: int):
        self.phase = "ready" if initial_delta == 0 else "poisoned"
        self.zeros = int(initial_delta == 0)
        self.attach_permissions = 0
        self.complete_cycles = 0

    def poll(self, delta: int) -> tuple[bool, bool]:
        if self.phase != "ready":
            self.phase = "poisoned"
            return False, False
        if delta == 0:
            self.zeros += 1
            return True, False
        if delta < 0 or delta > 1_000_000:
            self.phase = "poisoned"
            return False, False
        self.phase = "resume"
        self.attach_permissions = 1
        return True, True

    def attach(self) -> bool:
        if self.phase != "resume" or self.attach_permissions != 1:
            self.phase = "poisoned"
            return False
        self.phase = "attached"
        return True

    def complete(self) -> bool:
        if self.phase != "attached":
            self.phase = "poisoned"
            return False
        self.phase = "tick0"
        self.complete_cycles = 1
        return True


class StartlinePrearmProtocolTests(unittest.TestCase):
    def test_happy_path_is_ready_resume_attach_tick_zero(self) -> None:
        protocol = Model(0)
        for _ in range(8):
            self.assertEqual(protocol.poll(0), (True, False))
        self.assertEqual(protocol.poll(16667), (True, True))
        self.assertTrue(protocol.attach())
        self.assertTrue(protocol.complete())
        self.assertEqual((protocol.attach_permissions, protocol.complete_cycles), (1, 1))

    def test_moving_initial_state_never_publishes_ready(self) -> None:
        self.assertEqual(Model(1).phase, "poisoned")
        self.assertEqual(Model(-1).phase, "poisoned")

    def test_attach_before_resume_fails_closed(self) -> None:
        protocol = Model(0)
        self.assertFalse(protocol.attach())
        self.assertEqual(protocol.phase, "poisoned")

    def test_tick_zero_cannot_commit_twice(self) -> None:
        protocol = Model(0)
        protocol.poll(16667)
        protocol.attach()
        self.assertTrue(protocol.complete())
        self.assertFalse(protocol.complete())

    def test_protocol_core_has_no_runtime_or_write_primitives(self) -> None:
        combined = SOURCE + HEADER
        for token in (
            "kReadyNoAttach", "kResumeObserved", "permit_attach",
            "kAttachedWaitingForCompleteCycle", "commit_tick_zero",
        ):
            self.assertIn(token, combined)
        for forbidden in (
            "ptrace(", "pwrite", "process_vm_writev", "socket(", "system(",
            "adb", "keyevent",
        ):
            self.assertNotIn(forbidden, combined)


if __name__ == "__main__":
    unittest.main()
