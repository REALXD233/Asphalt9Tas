#!/usr/bin/env python3
"""Offline semantic model for the natural-action/final-writer dual cursor."""

from __future__ import annotations

import unittest


class Cursor:
    def __init__(self, counts: list[int]):
        if not counts or any(count not in (0, 1, 2) for count in counts):
            raise ValueError("configuration")
        self.counts = counts
        self.index = 0
        self.phase = "ready"
        self.action = False
        self.writer = False

    def prepare(self, index: int) -> tuple[int, int]:
        if self.phase != "ready" or index != self.index:
            raise ValueError("prepare")
        self.phase = "published"
        return index + 1, self.counts[index]

    def action_receipt(self, index: int, sequence: int, calls: int) -> None:
        if index != self.index or sequence != index + 1:
            raise ValueError("action cursor")
        if calls != self.counts[index]:
            raise ValueError("action count")
        self.action = True
        self._phase()

    def writer_receipt(self, index: int, processed: int) -> None:
        if index != self.index or processed != index + 1:
            raise ValueError("writer cursor")
        self.writer = True
        self._phase()

    def commit(self, index: int) -> None:
        if index != self.index or self.phase != "commit":
            raise ValueError("commit")
        self.index += 1
        self.action = self.writer = False
        self.phase = "complete" if self.index == len(self.counts) else "ready"

    def _phase(self) -> None:
        self.phase = "commit" if self.action and self.writer else (
            "action" if self.action else "writer"
        )


class NaturalActionReplayCursorTests(unittest.TestCase):
    def test_mixed_five_frame_sequence_and_both_receipt_orders(self) -> None:
        cursor = Cursor([0, 1, 0, 2, 1])
        for index, count in enumerate(cursor.counts):
            self.assertEqual(cursor.prepare(index), (index + 1, count))
            if index % 2:
                cursor.action_receipt(index, index + 1, count)
                cursor.writer_receipt(index, index + 1)
            else:
                cursor.writer_receipt(index, index + 1)
                cursor.action_receipt(index, index + 1, count)
            cursor.commit(index)
        self.assertEqual((cursor.index, cursor.phase), (5, "complete"))

    def test_cannot_advance_while_action_is_pending(self) -> None:
        cursor = Cursor([1, 0])
        cursor.prepare(0)
        cursor.writer_receipt(0, 1)
        with self.assertRaisesRegex(ValueError, "commit"):
            cursor.commit(0)
        with self.assertRaisesRegex(ValueError, "prepare"):
            cursor.prepare(1)

    def test_stale_or_mismatched_action_receipt_is_rejected(self) -> None:
        cursor = Cursor([0, 2])
        cursor.prepare(0)
        with self.assertRaisesRegex(ValueError, "action cursor"):
            cursor.action_receipt(0, 2, 0)
        with self.assertRaisesRegex(ValueError, "action count"):
            cursor.action_receipt(0, 1, 1)

    def test_zero_action_frame_still_consumes_one_sequence(self) -> None:
        cursor = Cursor([0])
        self.assertEqual(cursor.prepare(0), (1, 0))
        cursor.action_receipt(0, 1, 0)
        cursor.writer_receipt(0, 1)
        cursor.commit(0)
        self.assertEqual(cursor.phase, "complete")


if __name__ == "__main__":
    unittest.main()
