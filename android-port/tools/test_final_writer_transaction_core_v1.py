#!/usr/bin/env python3
"""Offline policy/model proof for the final-writer finite transaction core."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "final_writer_transaction_core_v1.h"


class TransactionModel:
    def __init__(self):
        self.phase = "preflight"

    def move(self, expected: str, target: str) -> None:
        if self.phase != expected:
            self.phase = "faulted"
            raise ValueError("phase")
        self.phase = target

    def rollback(self, observed: str) -> None:
        self.move("rollback", "rolled_back")
        if observed != "original":
            self.phase = "faulted"
            raise ValueError("vptr")


class TransactionCoreTests(unittest.TestCase):
    def test_success_sequence(self) -> None:
        state = TransactionModel()
        state.move("preflight", "prepared")
        state.move("prepared", "published")
        state.move("published", "installed")
        state.move("installed", "complete")
        self.assertEqual(state.phase, "complete")

    def test_failure_requires_observed_original_after_rollback(self) -> None:
        state = TransactionModel()
        state.move("preflight", "prepared")
        state.move("prepared", "rollback")
        with self.assertRaisesRegex(ValueError, "vptr"):
            state.rollback("shadow")
        self.assertEqual(state.phase, "faulted")

    def test_policy_contains_publish_last_and_exact_identity(self) -> None:
        text = HEADER.read_text(encoding="utf-8")
        for needle in (
            "kExpectedPrefix[11]", "kExpectedSlots[4]",
            "observed_original_vptr != expected_vptr",
            "slot != expected_slot", "IsFresh(initial_control, initial_evidence)",
            "unpublished_control.flags = 0",
            "control.reserved[0] == kFramePermitDisarmed",
            "published_control.flags = kControlConfigured | kControlTargetsLoaded",
            "std::memset(&result, 0, sizeof(result))",
            "std::memcpy(output, &result, sizeof(result))",
            "kPrimaryPrefixSize + kCallbackSlotOffset",
            "EvidenceComplete", "evidence.wrapper_entries == count",
            "evidence.original_calls == count", "evidence.clean_returns == count",
            "evidence.correction_writes == evidence.corrected_frames * 2u",
            "observed_vptr != control.original_vptr",
            "Phase::kRollbackRequired", "MarkRolledBack",
        ):
            self.assertIn(needle, text)
        for forbidden in ("ptrace", "pwrite", "process_vm_writev", "dlopen",
                          "input keyevent", "Nitro"):
            self.assertNotIn(forbidden, text)


if __name__ == "__main__":
    unittest.main()
