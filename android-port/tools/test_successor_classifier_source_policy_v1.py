#!/usr/bin/env python3
"""Offline source-contract checks for the read-only successor classifier."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


SOURCE = (Path(__file__).parents[1] / "src" /
          "successor_classifier_v1.cpp")
SCHEDULER_DEPENDENCY = (Path(__file__).parents[1] / "src" /
                        "hwbp_scheduler_observer_v1.cpp")


class SuccessorClassifierSourcePolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = SOURCE.read_text(encoding="utf-8")
        cls.combined_text = (cls.text + "\n" +
                             SCHEDULER_DEPENDENCY.read_text(encoding="utf-8"))

    def test_process_memory_is_opened_read_only(self) -> None:
        self.assertRegex(self.text, r"open\(mem_path,\s*O_RDONLY\s*\|")
        self.assertNotIn("O_RDWR", self.text)
        self.assertNotIn("O_WRONLY", self.text)

    def test_no_game_write_or_register_mutation_primitive(self) -> None:
        forbidden_calls = (
            r"\bpwrite\s*\(",
            r"\bwritev\s*\(",
            r"process_vm_writev\s*\(",
            r"PTRACE_POKEDATA",
            r"PTRACE_POKETEXT",
            r"PTRACE_SETREGS",
            r"PTRACE_SETFPREGS",
        )
        for pattern in forbidden_calls:
            with self.subTest(pattern=pattern):
                self.assertIsNone(re.search(pattern, self.combined_text))

    def test_no_input_or_guest_call_path(self) -> None:
        forbidden_tokens = (
            "KEYCODE_ESCAPE", "input keyevent", "adb shell input",
            "PTRACE_SINGLESTEP",
        )
        lowered = self.combined_text.lower()
        for token in forbidden_tokens:
            with self.subTest(token=token):
                self.assertNotIn(token.lower(), lowered)
        for pattern in (r"\bremote_call\s*\(", r"\bguest_call\s*\("):
            with self.subTest(pattern=pattern):
                self.assertIsNone(re.search(pattern, self.combined_text,
                                            flags=re.IGNORECASE))

    def test_exact_four_slot_contract_and_stack_depth(self) -> None:
        required = (
            "kRbxFirstWordOffset = 0x1968",
            "CheckedAdd(backend.native_angular_address, 0x0C",
            "CheckedAdd(final_owner, kC9COffset",
            "CheckedAdd(vehicle.physics_base, kF64Offset",
            "kStackWordCount = 64",
            "AttachNewThreads(\n        pid, rbx_watch, angular_aux_watch, "
            "c9c_watch, f64_watch",
        )
        for token in required:
            with self.subTest(token=token):
                self.assertIn(token, self.text)

    def test_exact_guest_return_markers_are_declared(self) -> None:
        for rva in ("0x369CC08", "0x369E45C", "0x369E044"):
            self.assertIn(rva, self.text)

    def test_signal_stops_are_classified_and_real_sigtrap_is_redelivered(self) -> None:
        for token in (
            "PTRACE_GETSIGINFO",
            "kTrapHardwareBreakpointCode",
            "ptrace_event == 0",
            "ptrace_event == PTRACE_EVENT_STOP",
            "const int deliver = ptrace_stop ? 0 : signal",
            "event == PTRACE_EVENT_STOP",
            "deliver = signal",
            "clear_and_detach_stopped(deliver)",
            "ClearDebugRegistersExact",
            "observed != 0",
            "if (!ClearDebugRegistersExact(tid)) return false",
        ):
            with self.subTest(token=token):
                self.assertIn(token, self.combined_text)


if __name__ == "__main__":
    unittest.main()
