#!/usr/bin/env python3
"""Tests for the unified tick executor's two-phase transition contract."""

from __future__ import annotations

import unittest

from unified_tick_executor_semantics_v1 import Action, Event, Machine, State


class UnifiedTickExecutorSemanticsTests(unittest.TestCase):
    def test_one_corrected_tick_has_exact_original_order(self) -> None:
        machine = Machine(1)
        sequence = (
            (Event.DT_NONZERO, {}, Action.SELECT_FRAME_AND_APPLY_DT),
            (Event.C98, {}, Action.WRITE_CONTROL_PAIR),
            (Event.C9C, {}, Action.REARM_POST_PHASE),
            (Event.COMPLETION, {}, None),
            (Event.CALLBACK_OPEN, {}, None),
            (Event.F64, {}, None),
            (Event.CALLBACK_CLOSE, {"payload_equal": False}, Action.CORRECTION_COPY_BOTH),
            (Event.DEFERRED_CALLBACK_CLEAR, {}, None),
            (Event.WORLD_COMMIT, {}, Action.COMMIT_FINAL_FRAME),
        )
        for event, kwargs, required in sequence:
            machine, actions = machine.step(event, **kwargs)
            if required is not None:
                self.assertIn(required, actions)
        self.assertEqual(machine, Machine(1, 1, State.COMPLETE))

    def test_equal_tick_performs_zero_correction_write(self) -> None:
        machine = Machine(1)
        for event in (
            Event.DT_NONZERO,
            Event.C98,
            Event.C9C,
            Event.COMPLETION,
            Event.CALLBACK_OPEN,
            Event.F64,
        ):
            machine, _ = machine.step(event)
        machine, actions = machine.step(Event.CALLBACK_CLOSE, payload_equal=True)
        self.assertEqual(actions, (Action.CORRECTION_ZERO_WRITE,))

    def test_transform_skip_is_independent_of_comparator(self) -> None:
        machine = Machine(1)
        for event in (
            Event.DT_NONZERO,
            Event.C98,
            Event.C9C,
            Event.COMPLETION,
            Event.CALLBACK_OPEN,
            Event.F64,
        ):
            machine, _ = machine.step(event)
        machine, actions = machine.step(
            Event.CALLBACK_CLOSE, payload_equal=False, skip_transform=True
        )
        self.assertEqual(actions, (Action.CORRECTION_SKIPPED,))

    def test_paused_cycle_does_not_consume_frame(self) -> None:
        machine, _ = Machine(2).step(Event.DT_NONZERO)
        machine, actions = machine.step(Event.DT_ZERO)
        self.assertEqual(machine, Machine(2, 0, State.WAITING))
        self.assertEqual(actions, (Action.PAUSED_CYCLE_NO_COMMIT,))

    def test_input_pair_is_written_after_both_game_setters(self) -> None:
        machine, _ = Machine(1).step(Event.DT_NONZERO)
        machine, first = machine.step(Event.C98)
        machine, second = machine.step(Event.C9C)
        self.assertEqual(first, (Action.WRITE_CONTROL_PAIR,))
        self.assertEqual(
            second, (Action.WRITE_CONTROL_PAIR, Action.REARM_POST_PHASE)
        )

    def test_rejects_partial_input_cycle(self) -> None:
        machine, _ = Machine(1).step(Event.DT_NONZERO)
        machine, _ = machine.step(Event.C98)
        with self.assertRaisesRegex(ValueError, "invalid transition"):
            machine.step(Event.DT_ZERO)

    def test_rejects_incomplete_gate2_and_commit_before_close(self) -> None:
        machine = Machine(1)
        for event in (Event.DT_NONZERO, Event.C98, Event.C9C):
            machine, _ = machine.step(event)
        with self.assertRaisesRegex(ValueError, "invalid transition"):
            machine.step(Event.CALLBACK_CLOSE, payload_equal=False)
        with self.assertRaisesRegex(ValueError, "invalid transition"):
            machine.step(Event.F64)
        machine, _ = machine.step(Event.COMPLETION)
        with self.assertRaisesRegex(ValueError, "invalid transition"):
            machine.step(Event.F64)
        machine, _ = machine.step(Event.CALLBACK_OPEN)
        machine, _ = machine.step(Event.F64)
        with self.assertRaisesRegex(ValueError, "invalid transition"):
            machine.step(Event.WORLD_COMMIT)
        machine, _ = machine.step(Event.CALLBACK_CLOSE, payload_equal=False)
        with self.assertRaisesRegex(ValueError, "invalid transition"):
            machine.step(Event.WORLD_COMMIT)

    def test_two_ticks_commit_exactly_once_each(self) -> None:
        machine = Machine(2)
        for equal in (False, True):
            for event in (
                Event.DT_NONZERO,
                Event.C98,
                Event.C9C,
                Event.COMPLETION,
                Event.CALLBACK_OPEN,
                Event.F64,
            ):
                machine, _ = machine.step(event)
            machine, _ = machine.step(Event.CALLBACK_CLOSE, payload_equal=equal)
            machine, _ = machine.step(Event.DEFERRED_CALLBACK_CLEAR)
            machine, _ = machine.step(Event.WORLD_COMMIT)
        self.assertEqual(machine, Machine(2, 2, State.COMPLETE))

    def test_c9c_prefix_snapshot_can_certify_missed_completion_and_open(self) -> None:
        machine = Machine(1)
        for event in (Event.DT_NONZERO, Event.C98, Event.C9C):
            machine, _ = machine.step(event)
        machine, actions = machine.step(Event.PREFIX_CERTIFIED)
        self.assertEqual(machine.state, State.WAIT_F64)
        self.assertEqual(actions, ())


if __name__ == "__main__":
    unittest.main()
