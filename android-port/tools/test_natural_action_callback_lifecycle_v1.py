#!/usr/bin/env python3
"""Reference model for persistent natural callback registration/removal."""

from __future__ import annotations

import unittest


class Lifecycle:
    def __init__(self):
        self.phase = "cold"
        self.claimed = 0
        self.completed = 0
        self.armed = False
        self.remove_requested = False

    def register(self, evidence=(1, 1, 1, 1, 1)):
        if self.phase != "cold" or evidence != (1, 1, 1, 1, 1):
            self.phase = "fault"
            return "reject"
        self.phase = "registered"
        return "ok"

    def arm(self):
        if self.phase != "registered":
            self.phase = "fault"
            return "reject"
        self.armed = True
        self.phase = "armed"
        return "ok"

    def prepare_remove(self, action_idle=True, host_pending=False):
        if self.phase != "armed":
            self.phase = "fault"
            return "reject"
        if not action_idle or host_pending or self.claimed != self.completed:
            return "pending"
        self.armed = False
        self.remove_requested = True
        self.phase = "removal_prepared"
        return "ok"

    def removal_callback(self, natural=True, once=True, deferred=True):
        if self.phase != "removal_prepared" or not (natural and once and deferred):
            self.phase = "fault"
            return "reject"
        self.phase = "removal_requested"
        return "ok"

    def removed(self, absent=True, stable=True):
        if self.phase != "removal_requested" or not (absent and stable):
            self.phase = "fault"
            return "reject"
        self.phase = "removed"
        return "ok"


class NaturalActionCallbackLifecycleTests(unittest.TestCase):
    def test_clean_full_lifecycle(self):
        model = Lifecycle()
        self.assertEqual(model.register(), "ok")
        self.assertEqual(model.arm(), "ok")
        self.assertEqual(model.prepare_remove(), "ok")
        self.assertFalse(model.armed)
        self.assertEqual(model.removal_callback(), "ok")
        self.assertEqual(model.removed(), "ok")

    def test_pending_frame_blocks_disarm_and_remove(self):
        model = Lifecycle()
        model.register()
        model.arm()
        model.claimed = 4
        model.completed = 3
        self.assertEqual(model.prepare_remove(), "pending")
        self.assertTrue(model.armed)
        self.assertFalse(model.remove_requested)

    def test_action_phase_and_host_pending_are_both_required_clear(self):
        for idle, pending in ((False, False), (True, True), (False, True)):
            model = Lifecycle()
            model.register()
            model.arm()
            self.assertEqual(model.prepare_remove(idle, pending), "pending")

    def test_ambiguous_registration_evidence_faults(self):
        for index in range(5):
            model = Lifecycle()
            evidence = [1, 1, 1, 1, 1]
            evidence[index] = 0
            self.assertEqual(model.register(tuple(evidence)), "reject")
            self.assertEqual(model.phase, "fault")

    def test_removal_requires_natural_deferred_once_and_absence(self):
        for evidence in ((False, True, True), (True, False, True),
                         (True, True, False)):
            model = Lifecycle()
            model.register()
            model.arm()
            model.prepare_remove()
            self.assertEqual(model.removal_callback(*evidence), "reject")
        model = Lifecycle()
        model.register()
        model.arm()
        model.prepare_remove()
        model.removal_callback()
        self.assertEqual(model.removed(absent=False), "reject")


if __name__ == "__main__":
    unittest.main()
