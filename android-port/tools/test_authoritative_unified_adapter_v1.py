#!/usr/bin/env python3
"""Transactional composition tests for authoritative selection + phase machine."""

from __future__ import annotations

import copy
import pathlib
import unittest
from dataclasses import dataclass

from authoritative_tick_state_machine_v1 import (
    ReplayMode,
    ReplayPacketV1,
    SelectionOutcome,
    TickInputStateV1,
    end_tick,
    select_on_new_tick,
)
from unified_tick_executor_semantics_v1 import Event, Machine, State
from upstream_replay_control_plane_v1 import ControlPlaneV1, ReplayV1


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (
    WORKSPACE / "android-port" / "src" / "authoritative_unified_adapter_v1.cpp"
).read_text(encoding="utf-8")
PROVEN_PHASE = (
    WORKSPACE / "android-port" / "src" / "unified_tick_executor_core_v1.cpp"
).read_text(encoding="utf-8")


@dataclass
class AdapterModel:
    control: ControlPlaneV1
    machine: Machine
    next_tick: int = 0
    selected_tick: int | None = None
    checkpoint: ControlPlaneV1 | None = None
    poisoned: bool = False

    @classmethod
    def create(cls, frame_count: int, target_tick: int) -> "AdapterModel":
        replay = ReplayV1(tuple(range(frame_count)), 16667)
        control, accepted = ControlPlaneV1(1000).queue_replay(
            replay, target_tick, in_race=False, current_race_tick=0
        )
        if not accepted:
            raise ValueError("queue rejected")
        control = control.on_update(current_race_tick=0)
        return cls(control, Machine(min(target_tick, frame_count - 1) + 1))

    def step(
        self,
        event: Event,
        *,
        payload_equal: bool | None = None,
        consumed_tick: int | None = None,
    ) -> SelectionOutcome | None:
        if self.poisoned:
            raise ValueError("adapter poisoned")
        selection_outcome = None
        if self.machine.state is State.WAITING and event is Event.DT_NONZERO:
            self.checkpoint = copy.deepcopy(self.control)
            selected = select_on_new_tick(
                current_tick=self.next_tick,
                mode=(
                    ReplayMode.ACTIVE_BLOCK_THREAD
                    if self.control.block_mode_active
                    else ReplayMode.INACTIVE
                ),
                queue=tuple(ReplayPacketV1(tick) for tick in self.control.queue),
            )
            selection_outcome = selected.outcome
            if selected.outcome is not SelectionOutcome.EXACT_PACKET:
                self.checkpoint = None
                return selection_outcome
            self.selected_tick = selected.selected.tick
            self.control = ControlPlaneV1(
                queue_capacity=self.control.queue_capacity,
                queue=tuple(packet.tick for packet in selected.remaining),
                session=self.control.session,
                block_mode_active=self.control.block_mode_active,
                fixed_interval_us=self.control.fixed_interval_us,
            )

        pause_rollback = self.machine.state is State.INPUT_OPEN and event is Event.DT_ZERO
        control_commit = self.machine.state is State.INPUT_OPEN and event is Event.C98
        world_commit = self.machine.state is State.WAIT_WORLD_COMMIT and event is Event.WORLD_COMMIT
        try:
            self.machine, _ = self.machine.step(event, payload_equal=payload_equal)
        except ValueError:
            if self.checkpoint is not None:
                self.control = self.checkpoint
                self.checkpoint = None
                self.selected_tick = None
            self.poisoned = True
            raise

        if pause_rollback:
            if self.checkpoint is None:
                self.poisoned = True
                raise ValueError("missing pause checkpoint")
            self.control = self.checkpoint
            self.checkpoint = None
            self.selected_tick = None
        elif control_commit:
            self.checkpoint = None

        if world_commit:
            if consumed_tick is None or consumed_tick != self.selected_tick:
                self.poisoned = True
                raise ValueError("commit tick mismatch")
            ended = end_tick(TickInputStateV1(race_tick=consumed_tick), in_race=True)
            self.control = self.control.on_update(
                current_race_tick=ended.published.race_tick
            )
            self.next_tick = ended.next_state.race_tick
            self.selected_tick = None
            if self.machine.frame_index != self.next_tick:
                self.poisoned = True
                raise ValueError("phase tick mismatch")
        return selection_outcome


def finish_tick(model: AdapterModel, tick: int, *, prefix: bool) -> None:
    model.step(Event.DT_NONZERO)
    model.step(Event.C98)
    model.step(Event.C9C)
    if prefix:
        model.step(Event.PREFIX_CERTIFIED)
    else:
        model.step(Event.COMPLETION)
        model.step(Event.CALLBACK_OPEN)
    model.step(Event.F64)
    model.step(Event.CALLBACK_CLOSE, payload_equal=bool(tick))
    model.step(Event.DEFERRED_CALLBACK_CLEAR)
    model.step(Event.WORLD_COMMIT, consumed_tick=tick)


class AuthoritativeUnifiedAdapterTests(unittest.TestCase):
    def test_pause_before_c98_rolls_back_selected_packet(self) -> None:
        model = AdapterModel.create(2, 1)
        before = model.control.queue
        self.assertEqual(model.step(Event.DT_NONZERO), SelectionOutcome.EXACT_PACKET)
        self.assertEqual(model.selected_tick, 0)
        model.step(Event.DT_ZERO)
        self.assertEqual(model.machine, Machine(2, 0, State.WAITING))
        self.assertEqual(model.control.queue, before)
        self.assertIsNone(model.selected_tick)
        self.assertEqual(model.step(Event.DT_NONZERO), SelectionOutcome.EXACT_PACKET)
        self.assertEqual(model.selected_tick, 0)

    def test_two_packets_bind_to_two_complete_phase_cycles(self) -> None:
        model = AdapterModel.create(2, 1)
        finish_tick(model, 0, prefix=True)
        self.assertEqual((model.machine.frame_index, model.next_tick), (1, 1))
        self.assertTrue(model.control.block_mode_active)
        finish_tick(model, 1, prefix=False)
        self.assertEqual(model.machine, Machine(2, 2, State.COMPLETE))
        self.assertEqual(model.next_tick, 2)
        self.assertFalse(model.control.block_mode_active)

    def test_final_packet_is_consumed_before_mode_disable(self) -> None:
        model = AdapterModel.create(3, 2)
        for tick in range(3):
            finish_tick(model, tick, prefix=True)
        self.assertEqual(model.next_tick, 3)
        self.assertEqual(model.machine.state, State.COMPLETE)
        self.assertFalse(model.control.block_mode_active)

    def test_target_zero_preserves_literal_upstream_edge(self) -> None:
        model = AdapterModel.create(2, 0)
        self.assertFalse(model.control.block_mode_active)
        self.assertEqual(model.step(Event.DT_NONZERO), SelectionOutcome.INACTIVE)
        self.assertEqual(model.machine.state, State.WAITING)

    def test_invalid_post_selection_order_rolls_back_and_poison_fails_closed(self) -> None:
        model = AdapterModel.create(2, 1)
        before = model.control.queue
        model.step(Event.DT_NONZERO)
        with self.assertRaisesRegex(ValueError, "invalid transition"):
            model.step(Event.C9C)
        self.assertTrue(model.poisoned)
        self.assertEqual(model.control.queue, before)
        with self.assertRaisesRegex(ValueError, "poisoned"):
            model.step(Event.DT_NONZERO)

    def test_commit_requires_matching_consumed_tick(self) -> None:
        model = AdapterModel.create(2, 1)
        for event in (
            Event.DT_NONZERO,
            Event.C98,
            Event.C9C,
            Event.PREFIX_CERTIFIED,
            Event.F64,
        ):
            model.step(event)
        model.step(Event.CALLBACK_CLOSE, payload_equal=False)
        model.step(Event.DEFERRED_CALLBACK_CLEAR)
        with self.assertRaisesRegex(ValueError, "commit tick mismatch"):
            model.step(Event.WORLD_COMMIT, consumed_tick=1)
        self.assertTrue(model.poisoned)

    def test_cpp_adapter_reuses_proven_phase_core_without_live_primitives(self) -> None:
        self.assertIn('#include "unified_tick_executor_core_v1.cpp"', SOURCE)
        self.assertIn("ReplaySessionV1 pre_c98_checkpoint", SOURCE)
        self.assertIn("state->session.OnUpdate(published.race_tick)", SOURCE)
        self.assertIn("state->poisoned = true", SOURCE)
        self.assertIn("bool Advance(Machine* machine", PROVEN_PHASE)
        for token in (
            "PTRACE_",
            "process_vm_",
            "/proc/",
            "socket(",
            "connect(",
            "pwrite(",
            "RemoteCall",
            "NitroEnable",
        ):
            self.assertNotIn(token, SOURCE)


if __name__ == "__main__":
    unittest.main()

