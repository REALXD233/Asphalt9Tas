#!/usr/bin/env python3
"""Executable model for non-blocking AluTasV2 action-count replay."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_natural_action_callback_lifecycle_v1.cpp"


class Consumer:
    def __init__(self) -> None:
        self.completed = 0
        self.queue_count = 0
        self.calls = 0

    def callback(self, sequence: int, frame: int, activations: int) -> None:
        if sequence != self.completed + 1 or frame != self.completed:
            raise ValueError("sequence")
        if activations not in (0, 1, 2):
            raise ValueError("count")
        before = self.queue_count
        for _ in range(activations):
            self.queue_count += 1
            self.calls += 1
        if self.queue_count != before + activations:
            raise ValueError("queue growth")
        # Completion is the exact game-owned call receipt. Nitro colour/state
        # may transition later and is not allowed to stall the frame cursor.
        self.completed = sequence


class NaturalActionReplayPayloadSemanticsTests(unittest.TestCase):
    def test_mixed_sequence_completes_one_command_per_callback(self) -> None:
        consumer = Consumer()
        counts = [0, 1, 0, 2, 1]
        for frame, count in enumerate(counts):
            consumer.callback(frame + 1, frame, count)
            self.assertEqual(consumer.completed, frame + 1)
        self.assertEqual((consumer.completed, consumer.calls), (5, 4))

    def test_state_transition_is_not_a_cursor_prerequisite(self) -> None:
        consumer = Consumer()
        consumer.callback(1, 0, 1)
        # No synthetic Nitro state is supplied to the model; submission still
        # completes because that is exactly what upstream records and replays.
        consumer.callback(2, 1, 0)
        self.assertEqual(consumer.completed, 2)

    def test_source_replay_branch_has_no_pending_sequence_wait(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        marker = "#if A9TAS_NAL_REPLAY_EXECUTE == 1\n        // AluTasV2"
        start = text.index(marker)
        end = text.index("#else", start)
        replay_branch = text[start:end]
        self.assertIn("SubmitGameOwnedActions", text[:start])
        self.assertIn("CompleteNaturalCallback", replay_branch)
        self.assertIn("kActionSubmissionProof", replay_branch)
        self.assertNotIn("g_pending_action_sequence", replay_branch)
        self.assertNotIn("kMaximumPendingObservationCallbacks", text)


if __name__ == "__main__":
    unittest.main()
