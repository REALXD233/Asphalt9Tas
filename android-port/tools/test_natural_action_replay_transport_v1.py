#!/usr/bin/env python3
"""Executable semantic model for AluTasV2 per-frame Nitro events."""

from __future__ import annotations

import unittest


SKIP_NITRO = 1 << 2
REPLAY_FRAME = 1 << 0
NITRO_OVERRIDE = 1 << 1


def command(tick: int, sequence: int, count: int, skip: int = 0) -> tuple[int, int]:
    if tick < 0 or sequence != tick + 1 or count not in (0, 1, 2):
        raise ValueError("invalid authoritative Nitro frame")
    if skip & SKIP_NITRO:
        return 0, REPLAY_FRAME
    return count, REPLAY_FRAME | NITRO_OVERRIDE


def transition(before: tuple[int, int], after: tuple[int, int], count: int) -> bool:
    if count not in (1, 2):
        return False
    return before != after


class NaturalActionReplayTransportTests(unittest.TestCase):
    def test_upstream_counts_are_preserved_without_colour_label(self) -> None:
        self.assertEqual(command(10, 11, 0), (0, 3))
        self.assertEqual(command(10, 11, 1), (1, 3))
        self.assertEqual(command(10, 11, 2), (2, 3))

    def test_skip_keeps_cursor_but_submits_no_action(self) -> None:
        self.assertEqual(command(10, 11, 2, SKIP_NITRO), (0, 1))

    def test_yellow_and_second_click_use_state_transition_not_forced_mode(self) -> None:
        self.assertTrue(transition((0, 0), (1, 1), 1))
        self.assertTrue(transition((1, 1), (1, 2), 1))
        self.assertTrue(transition((0, 0), (1, 3), 2))
        self.assertFalse(transition((1, 1), (1, 1), 1))

    def test_tick_sequence_is_exact(self) -> None:
        with self.assertRaises(ValueError):
            command(10, 12, 1)


if __name__ == "__main__":
    unittest.main()
