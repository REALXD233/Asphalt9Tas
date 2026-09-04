#!/usr/bin/env python3
"""Host model for verified publication plus callback-close dual receipts."""

from __future__ import annotations

import unittest


class Runtime:
    def __init__(self, counts: list[int]):
        self.counts = counts
        self.index = 0
        self.pending: tuple[int, int] | None = None
        self.claimed = 0
        self.completed = 0
        self.writer_processed = 0
        self.faulted = False

    def begin_publish(self, index: int) -> None:
        if self.faulted or self.pending is not None or index != self.index:
            raise ValueError("begin")
        self.pending = (index + 1, self.counts[index])

    def consume(self, calls: int) -> None:
        if self.pending is None:
            raise ValueError("missing publication")
        sequence, expected = self.pending
        if calls != expected:
            self.faulted = True
            raise ValueError("call count")
        self.claimed = self.completed = sequence

    def callback_close(self, index: int, writer_processed: int) -> None:
        if (self.pending is None or index != self.index or
                self.completed != index + 1 or writer_processed != index + 1):
            self.faulted = True
            raise ValueError("dual receipt")
        self.writer_processed = writer_processed

    def world_commit(self, index: int) -> None:
        if (self.faulted or self.pending is None or index != self.index or
                self.completed != index + 1 or
                self.writer_processed != index + 1):
            raise ValueError("commit")
        self.index += 1
        self.pending = None


class RuntimeTests(unittest.TestCase):
    def test_five_frame_original_sequence(self) -> None:
        runtime = Runtime([0, 1, 0, 2, 0])
        for index, count in enumerate(runtime.counts):
            runtime.begin_publish(index)
            runtime.consume(count)
            runtime.callback_close(index, index + 1)
            runtime.world_commit(index)
        self.assertEqual(runtime.index, 5)

    def test_callback_close_rejects_missing_action(self) -> None:
        runtime = Runtime([1])
        runtime.begin_publish(0)
        with self.assertRaisesRegex(ValueError, "dual receipt"):
            runtime.callback_close(0, 1)
        self.assertTrue(runtime.faulted)

    def test_callback_close_rejects_writer_from_next_frame(self) -> None:
        runtime = Runtime([0, 2])
        runtime.begin_publish(0)
        runtime.consume(0)
        with self.assertRaisesRegex(ValueError, "dual receipt"):
            runtime.callback_close(0, 2)

    def test_world_commit_cannot_precede_callback_close(self) -> None:
        runtime = Runtime([2])
        runtime.begin_publish(0)
        runtime.consume(2)
        with self.assertRaisesRegex(ValueError, "commit"):
            runtime.world_commit(0)


if __name__ == "__main__":
    unittest.main()
