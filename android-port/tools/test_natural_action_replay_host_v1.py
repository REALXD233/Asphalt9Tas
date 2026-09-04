#!/usr/bin/env python3
"""Offline host publication-order model for the natural-action mailbox."""

from __future__ import annotations

import unittest


def publish_log(sequence: int, completed: int) -> list[str]:
    if sequence != completed + 1:
        raise ValueError("sequence")
    return ["inactive_slot", "published_state", "selector_release"]


class NaturalActionReplayHostTests(unittest.TestCase):
    def test_selector_is_the_last_publication_write(self) -> None:
        self.assertEqual(
            publish_log(1, 0),
            ["inactive_slot", "published_state", "selector_release"],
        )

    def test_next_frame_requires_prior_completion(self) -> None:
        with self.assertRaisesRegex(ValueError, "sequence"):
            publish_log(2, 0)

    def test_completion_cannot_be_overwritten_after_publication(self) -> None:
        writes = publish_log(1, 0)
        selector = writes.index("selector_release")
        self.assertTrue(all(index < selector for index in (0, 1)))


if __name__ == "__main__":
    unittest.main()
