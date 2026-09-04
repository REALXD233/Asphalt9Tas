#!/usr/bin/env python3
"""Semantic model for A9UTK1 -> natural action command -> receipt gating."""

from __future__ import annotations

import dataclasses
import unittest


SKIP_NITRO = 1 << 2


@dataclasses.dataclass(frozen=True)
class Frame:
    count: int
    skip: int


@dataclasses.dataclass
class Gate:
    next_sequence: int = 1
    next_frame: int = 0
    pending: tuple[int, int, int, bool] | None = None
    published: bool = False

    def plan(self, frame: Frame, index: int):
        if self.pending is not None:
            return "pending", None
        if index != self.next_frame or self.next_sequence != index + 1:
            return "sequence", None
        if not 0 <= frame.count <= 2:
            return "invalid", None
        enabled = not frame.skip & SKIP_NITRO
        count = frame.count if enabled else 0
        self.pending = (self.next_sequence, index, count, enabled)
        self.published = False
        return "ok", self.pending

    def mark_published(self):
        if self.pending is None or self.published:
            return "invalid"
        self.published = True
        return "ok"

    def cancel(self):
        if self.pending is None or self.published:
            return "invalid"
        self.pending = None
        return "ok"

    def receipt(self, sequence: int, frame: int, calls: int):
        if not self.published:
            return "invalid"
        if self.pending != (sequence, frame, calls, self.pending[3]):
            return "mismatch"
        self.pending = None
        self.published = False
        self.next_sequence += 1
        self.next_frame += 1
        return "ok"


class NaturalActionHostGateTests(unittest.TestCase):
    def test_skip_zeros_count_without_collapsing_enabled_state(self):
        gate = Gate()
        result, command = gate.plan(Frame(2, SKIP_NITRO), 0)
        self.assertEqual(result, "ok")
        self.assertEqual(command, (1, 0, 0, False))
        self.assertEqual(gate.mark_published(), "ok")
        self.assertEqual(gate.receipt(1, 0, 0), "ok")
        result, command = gate.plan(Frame(0, 0), 1)
        self.assertEqual(result, "ok")
        self.assertEqual(command, (2, 1, 0, True))

    def test_enabled_zero_one_two_are_exact(self):
        gate = Gate()
        for index, count in enumerate((0, 1, 2)):
            result, command = gate.plan(Frame(count, 0), index)
            self.assertEqual(result, "ok")
            self.assertEqual(command[2:], (count, True))
            self.assertEqual(gate.mark_published(), "ok")
            self.assertEqual(gate.receipt(index + 1, index, count), "ok")

    def test_pending_frame_blocks_next_publication(self):
        gate = Gate()
        self.assertEqual(gate.plan(Frame(1, 0), 0)[0], "ok")
        self.assertEqual(gate.plan(Frame(0, 0), 1)[0], "pending")

    def test_bad_receipt_does_not_advance(self):
        gate = Gate()
        self.assertEqual(gate.plan(Frame(2, 0), 0)[0], "ok")
        self.assertEqual(gate.mark_published(), "ok")
        self.assertEqual(gate.receipt(1, 0, 1), "mismatch")
        self.assertEqual((gate.next_sequence, gate.next_frame), (1, 0))
        self.assertIsNotNone(gate.pending)

    def test_count_and_sequence_bounds(self):
        gate = Gate()
        self.assertEqual(gate.plan(Frame(3, 0), 0)[0], "invalid")
        self.assertEqual(gate.plan(Frame(0, 0), 1)[0], "sequence")

    def test_pre_c98_cancel_reuses_same_sequence(self):
        gate = Gate()
        self.assertEqual(gate.plan(Frame(2, 0), 0)[0], "ok")
        self.assertEqual(gate.cancel(), "ok")
        self.assertEqual((gate.next_sequence, gate.next_frame), (1, 0))
        result, command = gate.plan(Frame(1, 0), 0)
        self.assertEqual(result, "ok")
        self.assertEqual(command[:3], (1, 0, 1))


if __name__ == "__main__":
    unittest.main()
