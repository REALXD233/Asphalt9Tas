#!/usr/bin/env python3
"""Offline model for natural action plus final-writer frame commitment."""

from __future__ import annotations

import unittest


class Combined:
    def __init__(self, counts: list[int]):
        self.counts = counts
        self.index = 0
        self.action = False
        self.writer = False
        self.open = False

    def begin(self, index: int) -> None:
        if self.open or index != self.index:
            raise ValueError("begin")
        self.open = True

    def action_receipt(self, index: int, sequence: int, calls: int) -> None:
        if not self.open or index != self.index or sequence != index + 1:
            raise ValueError("action cursor")
        if calls != self.counts[index]:
            raise ValueError("action count")
        self.action = True

    def writer_receipt(self, index: int, processed: int) -> None:
        if not self.open or index != self.index or processed != index + 1:
            raise ValueError("writer cursor")
        self.writer = True

    def commit(self, index: int) -> None:
        if index != self.index or not self.action or not self.writer:
            raise ValueError("dual receipt")
        self.index += 1
        self.open = self.action = self.writer = False


class FinalWriterNaturalActionBindingTests(unittest.TestCase):
    def test_five_frame_mixed_action_counts(self) -> None:
        combined = Combined([0, 1, 0, 2, 1])
        for index, count in enumerate(combined.counts):
            combined.begin(index)
            combined.writer_receipt(index, index + 1)
            combined.action_receipt(index, index + 1, count)
            combined.commit(index)
        self.assertEqual(combined.index, 5)

    def test_writer_receipt_alone_cannot_commit(self) -> None:
        combined = Combined([1])
        combined.begin(0)
        combined.writer_receipt(0, 1)
        with self.assertRaisesRegex(ValueError, "dual receipt"):
            combined.commit(0)

    def test_action_receipt_alone_cannot_commit(self) -> None:
        combined = Combined([1])
        combined.begin(0)
        combined.action_receipt(0, 1, 1)
        with self.assertRaisesRegex(ValueError, "dual receipt"):
            combined.commit(0)

    def test_receipts_cannot_cross_frames(self) -> None:
        combined = Combined([0, 2])
        combined.begin(0)
        with self.assertRaisesRegex(ValueError, "writer cursor"):
            combined.writer_receipt(0, 2)
        with self.assertRaisesRegex(ValueError, "action cursor"):
            combined.action_receipt(1, 2, 2)


if __name__ == "__main__":
    unittest.main()
