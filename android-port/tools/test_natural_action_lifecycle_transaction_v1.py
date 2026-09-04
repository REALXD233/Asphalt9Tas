#!/usr/bin/env python3
"""Reference model for the guarded zero-call lifecycle transaction."""

from __future__ import annotations

import unittest


class Transaction:
    def __init__(self):
        self.phase = "cold"
        self.mutated = False
        self.pending = False
        self.frames = 0

    def reject(self):
        self.phase = "force_stop" if self.mutated else "fault"
        return "reject"

    def preflight(self, identity=True):
        if self.phase != "cold" or not identity:
            return self.reject()
        self.phase = "preflight"
        return "ok"

    def ready(self, clean=True, tracers_clear=True):
        if self.phase != "preflight" or not clean or not tracers_clear:
            return self.reject()
        self.phase = "ready_no_attach"
        return "ok"

    def resume(self, same_process=True, esc_count=1, positive_delta=True):
        if (self.phase != "ready_no_attach" or not same_process or
                esc_count != 1 or not positive_delta):
            return self.reject()
        self.phase = "resumed"
        return "ok"

    def install(self, writes=1, exact=True):
        if self.phase != "resumed" or writes != 1 or not exact:
            return self.reject()
        self.mutated = True
        self.phase = "shadow"
        return "ok"

    def register(self, exact=True):
        if self.phase != "shadow" or not exact:
            return self.reject()
        self.phase = "registered"
        return "ok"

    def detach_and_arm(self, clean=True):
        if self.phase != "registered" or not clean:
            return self.reject()
        self.phase = "armed"
        return "ok"

    def select(self, calls=0):
        if self.phase != "armed" or calls != 0:
            return self.reject()
        self.pending = True
        self.phase = "frame"
        return "ok"

    def receipt(self, exact=True):
        if self.phase != "frame" or not self.pending or not exact:
            return self.reject()
        self.pending = False
        self.frames += 1
        self.phase = "armed"
        return "ok"

    def remove(self, exact=True):
        if self.phase != "armed" or self.pending or not exact:
            return self.reject()
        self.phase = "removing"
        return "ok"

    def finish(self, absent=True, stable=True, tracer_clear=True):
        if (self.phase != "removing" or not absent or not stable or
                not tracer_clear):
            return self.reject()
        self.phase = "clean"
        return "ok"


class LifecycleTransactionTests(unittest.TestCase):
    def test_complete_zero_call_transaction(self):
        tx = Transaction()
        self.assertEqual(tx.preflight(), "ok")
        self.assertEqual(tx.ready(), "ok")
        self.assertEqual(tx.resume(), "ok")
        self.assertEqual(tx.install(), "ok")
        self.assertEqual(tx.register(), "ok")
        self.assertEqual(tx.detach_and_arm(), "ok")
        for _ in range(5):
            self.assertEqual(tx.select(0), "ok")
            self.assertEqual(tx.receipt(), "ok")
        self.assertEqual(tx.remove(), "ok")
        self.assertEqual(tx.finish(), "ok")
        self.assertEqual(tx.frames, 5)

    def test_pre_mutation_failure_is_local_fault(self):
        tx = Transaction()
        self.assertEqual(tx.preflight(identity=False), "reject")
        self.assertEqual(tx.phase, "fault")

    def test_post_mutation_failure_requires_force_stop(self):
        tx = Transaction()
        tx.preflight()
        tx.ready()
        tx.resume()
        tx.install()
        tx.register()
        tx.detach_and_arm()
        self.assertEqual(tx.select(calls=1), "reject")
        self.assertEqual(tx.phase, "force_stop")

    def test_startline_requires_one_resume_and_positive_delta(self):
        for esc_count, positive in ((0, True), (2, True), (1, False)):
            tx = Transaction()
            tx.preflight()
            tx.ready()
            self.assertEqual(tx.resume(esc_count=esc_count,
                                       positive_delta=positive), "reject")
            self.assertEqual(tx.phase, "fault")

    def test_pending_frame_blocks_removal(self):
        tx = Transaction()
        tx.preflight()
        tx.ready()
        tx.resume()
        tx.install()
        tx.register()
        tx.detach_and_arm()
        tx.select()
        self.assertEqual(tx.remove(), "reject")
        self.assertEqual(tx.phase, "force_stop")


if __name__ == "__main__":
    unittest.main()
