#!/usr/bin/env python3
"""Model tests for Delta-plan, C98-publish, callback-receipt, world-commit."""

from __future__ import annotations

import unittest


class PhaseModel:
    def __init__(self):
        self.phase = "idle"
        self.sequence = 1
        self.frame = 0
        self.rollbacks = 0
        self.commits = 0

    def select(self):
        if self.phase != "idle":
            self.phase = "poisoned"
            return "invalid"
        self.phase = "planned"
        return "ok"

    def delta_zero(self):
        if self.phase != "planned":
            self.phase = "poisoned"
            return "invalid"
        self.rollbacks += 1
        self.phase = "idle"
        return "ok"

    def c98_publish(self):
        if self.phase != "planned":
            self.phase = "poisoned"
            return "invalid"
        self.phase = "published"
        return "ok"

    def callback_close(self, receipt: tuple[int, int] | None):
        if self.phase != "published" or receipt != (self.sequence, self.frame):
            self.phase = "poisoned"
            return "missing"
        self.phase = "receipt"
        self.sequence += 1
        self.frame += 1
        return "ok"

    def world_commit(self):
        if self.phase != "receipt":
            self.phase = "poisoned"
            return "missing"
        self.commits += 1
        self.phase = "idle"
        return "ok"


class AuthoritativeNaturalActionPhaseTests(unittest.TestCase):
    def test_complete_transaction(self):
        model = PhaseModel()
        self.assertEqual(model.select(), "ok")
        self.assertEqual(model.c98_publish(), "ok")
        self.assertEqual(model.callback_close((1, 0)), "ok")
        self.assertEqual(model.world_commit(), "ok")
        self.assertEqual((model.commits, model.sequence, model.frame), (1, 2, 1))

    def test_pre_c98_delta_zero_rolls_back_without_consuming_frame(self):
        model = PhaseModel()
        self.assertEqual(model.select(), "ok")
        self.assertEqual(model.delta_zero(), "ok")
        self.assertEqual((model.sequence, model.frame, model.rollbacks), (1, 0, 1))
        self.assertEqual(model.select(), "ok")

    def test_publish_before_c98_is_not_representable(self):
        model = PhaseModel()
        self.assertEqual(model.c98_publish(), "invalid")
        self.assertEqual(model.phase, "poisoned")

    def test_missing_or_wrong_receipt_poisons_before_world_commit(self):
        for receipt in (None, (2, 0), (1, 1)):
            model = PhaseModel()
            model.select()
            model.c98_publish()
            self.assertEqual(model.callback_close(receipt), "missing")
            self.assertEqual(model.world_commit(), "missing")
            self.assertEqual(model.commits, 0)

    def test_world_commit_cannot_precede_callback_receipt(self):
        model = PhaseModel()
        model.select()
        model.c98_publish()
        self.assertEqual(model.world_commit(), "missing")
        self.assertEqual(model.commits, 0)


if __name__ == "__main__":
    unittest.main()
