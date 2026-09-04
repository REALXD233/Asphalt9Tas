#!/usr/bin/env python3
"""End-to-end source-bound tests for the authoritative replay session."""

from __future__ import annotations

import pathlib
import unittest

from authoritative_tick_state_machine_v1 import (
    ReplayMode,
    ReplayPacketV1,
    SelectionOutcome,
    TickInputStateV1,
    end_tick,
    select_on_new_tick,
)
from upstream_replay_control_plane_v1 import ControlPlaneV1, ReplayV1


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SESSION_SOURCE = (
    WORKSPACE / "android-port" / "src" / "authoritative_replay_session_v1.cpp"
).read_text(encoding="utf-8")
SESSION_HEADER = (
    WORKSPACE / "android-port" / "src" / "authoritative_replay_session_v1.h"
).read_text(encoding="utf-8")
MANAGER_SOURCE = (
    WORKSPACE
    / "source"
    / "AluTasV2-main"
    / "AsphaltTool"
    / "tool"
    / "src"
    / "globalstate"
    / "ReplayStateManager.cpp"
).read_text(encoding="utf-8")
DETOUR_SOURCE = (
    WORKSPACE
    / "source"
    / "AluTasV2-main"
    / "AsphaltTool"
    / "dll"
    / "src"
    / "DetourFunctions.cpp"
).read_text(encoding="utf-8")


def refill_and_select(
    state: ControlPlaneV1, current_tick: int
) -> tuple[ControlPlaneV1, SelectionOutcome, int | None, int]:
    state = state.on_update(current_race_tick=current_tick)
    result = select_on_new_tick(
        current_tick=current_tick,
        mode=(
            ReplayMode.ACTIVE_BLOCK_THREAD
            if state.block_mode_active
            else ReplayMode.INACTIVE
        ),
        queue=tuple(ReplayPacketV1(tick) for tick in state.queue),
    )
    selected = None if result.selected is None else result.selected.tick
    state = ControlPlaneV1(
        queue_capacity=state.queue_capacity,
        queue=tuple(packet.tick for packet in result.remaining),
        session=state.session,
        block_mode_active=state.block_mode_active,
        fixed_interval_us=state.fixed_interval_us,
    )
    return state, result.outcome, selected, result.stale_dropped


class AuthoritativeReplaySessionTests(unittest.TestCase):
    def test_queue_refill_exact_selection_and_stale_drain_compose(self) -> None:
        replay = ReplayV1((0, 1, 2, 3), 16667)
        state, accepted = ControlPlaneV1(8).queue_replay(
            replay, 3, in_race=False, current_race_tick=0
        )
        self.assertTrue(accepted)
        state, outcome, selected, stale = refill_and_select(state, 0)
        self.assertEqual((outcome, selected, stale), (SelectionOutcome.EXACT_PACKET, 0, 0))

        # Skipping the selector at tick 1 forces the literal block-mode stale
        # drain before tick 2 is selected.
        state, outcome, selected, stale = refill_and_select(state, 2)
        self.assertEqual((outcome, selected, stale), (SelectionOutcome.EXACT_PACKET, 2, 1))

    def test_future_packet_is_preserved(self) -> None:
        result = select_on_new_tick(
            current_tick=2,
            mode=ReplayMode.ACTIVE_BLOCK_THREAD,
            queue=(ReplayPacketV1(3),),
        )
        self.assertEqual(result.outcome, SelectionOutcome.FUTURE_HEAD_GAP)
        self.assertEqual(tuple(packet.tick for packet in result.remaining), (3,))

    def test_empty_blocking_queue_requires_retry(self) -> None:
        result = select_on_new_tick(
            current_tick=0,
            mode=ReplayMode.ACTIVE_BLOCK_THREAD,
            queue=(),
        )
        self.assertEqual(result.outcome, SelectionOutcome.BLOCKED_EMPTY)
        self.assertTrue(result.clear_previous_packet)

    def test_last_frame_saturation_survives_selector_composition(self) -> None:
        replay = ReplayV1((0, 1, 2), 16667)
        state, _ = ControlPlaneV1(7).queue_replay(
            replay, 2, in_race=False, current_race_tick=0
        )
        state = state.on_update(current_race_tick=0)
        self.assertEqual(state.queue, (0, 1, 2, 2, 2, 2, 2))
        result = select_on_new_tick(
            current_tick=2,
            mode=ReplayMode.ACTIVE_BLOCK_THREAD,
            queue=tuple(ReplayPacketV1(tick) for tick in state.queue),
        )
        self.assertEqual(result.outcome, SelectionOutcome.EXACT_PACKET)
        self.assertEqual(result.stale_dropped, 2)
        self.assertEqual(tuple(packet.tick for packet in result.remaining), (2, 2, 2, 2))

    def test_target_zero_literal_upstream_edge_is_not_silently_fixed(self) -> None:
        replay = ReplayV1((0, 1), 16667)
        state, _ = ControlPlaneV1(8).queue_replay(
            replay, 0, in_race=False, current_race_tick=0
        )
        state = state.on_update(current_race_tick=0)
        self.assertFalse(state.block_mode_active)
        self.assertEqual(state.queue, ())

    def test_race_end_rearms_then_clears_old_packets(self) -> None:
        replay = ReplayV1((0, 1, 2), 16667)
        state, _ = ControlPlaneV1(8).queue_replay(
            replay, 2, in_race=False, current_race_tick=0
        )
        state = state.on_update(current_race_tick=0)
        ended = state.on_race_ended()
        self.assertTrue(ended.block_mode_active)
        self.assertEqual(ended.session.frame_index, 0)
        self.assertEqual(ended.queue, ())

    def test_inclusive_final_packet_is_selected_before_completion_disable(self) -> None:
        replay = ReplayV1((0, 1, 2, 3), 16667)
        state, _ = ControlPlaneV1(8).queue_replay(
            replay, 3, in_race=False, current_race_tick=0
        )
        state = state.on_update(current_race_tick=0)
        input_state = TickInputStateV1(race_tick=0)
        selected_ticks: list[int] = []
        for tick in range(4):
            selected = select_on_new_tick(
                current_tick=tick,
                mode=(
                    ReplayMode.ACTIVE_BLOCK_THREAD
                    if state.block_mode_active
                    else ReplayMode.INACTIVE
                ),
                queue=tuple(ReplayPacketV1(item) for item in state.queue),
            )
            self.assertEqual(selected.outcome, SelectionOutcome.EXACT_PACKET)
            selected_ticks.append(selected.selected.tick)
            state = ControlPlaneV1(
                queue_capacity=state.queue_capacity,
                queue=tuple(packet.tick for packet in selected.remaining),
                session=state.session,
                block_mode_active=state.block_mode_active,
                fixed_interval_us=state.fixed_interval_us,
            )
            input_state = TickInputStateV1(race_tick=tick)
            ended = end_tick(input_state, in_race=True)
            # Tool-side OnUpdate observes the just-published tick, before the
            # DLL's next-state counter value is used by the next OnNewTick.
            state = state.on_update(current_race_tick=ended.published.race_tick)
            input_state = ended.next_state
        self.assertEqual(selected_ticks, [0, 1, 2, 3])
        self.assertFalse(state.block_mode_active)
        self.assertEqual(input_state.race_tick, 4)

    def test_end_tick_publish_reset_and_increment_remain_one_transaction(self) -> None:
        state = TickInputStateV1(
            race_tick=4,
            steer=0.25,
            brake=-1.0,
            accelerator=0.75,
            nitro_activation_count=2,
            respawn=True,
            barrel_angular=(1.0, 2.0, 3.0),
            barrel_rbx=(4.0, 5.0),
        )
        result = end_tick(state, in_race=True)
        self.assertEqual(result.published, state)
        self.assertEqual(result.next_state.race_tick, 5)
        self.assertEqual(
            (result.next_state.steer, result.next_state.brake, result.next_state.accelerator),
            (0.25, -1.0, 0.75),
        )
        self.assertEqual(result.next_state.nitro_activation_count, 0)
        self.assertFalse(result.next_state.respawn)

    def test_cpp_core_composes_existing_authoritative_functions(self) -> None:
        self.assertIn("authoritative_tick_v1::SelectOnNewTick", SESSION_SOURCE)
        self.assertIn("authoritative_tick_v1::EndTick", SESSION_SOURCE)
        self.assertIn("std::array<", SESSION_HEADER)
        self.assertIn("kReplayQueueCapacity = 1000", SESSION_HEADER)
        self.assertIn("queued_fixed_interval_us_", SESSION_HEADER)
        self.assertIn("installed_fixed_interval_us_", SESSION_HEADER)
        self.assertIn(
            "installed_fixed_interval_us_ = queued_fixed_interval_us_",
            SESSION_SOURCE,
        )

    def test_cpp_order_is_bound_to_upstream_order(self) -> None:
        completion = SESSION_SOURCE.index("current_race_tick >= final_tick_")
        refill = SESSION_SOURCE.index("while (queue_size_ < queue_.size())", completion)
        self.assertLess(completion, refill)
        rearm = SESSION_SOURCE.index("if (has_replay_) InitializeQueuedReplay()")
        clear = SESSION_SOURCE.index("ClearInputQueue();", rearm)
        self.assertLess(rearm, clear)
        self.assertIn("PushPlaybackFramesToInputInBuffer", MANAGER_SOURCE)
        self.assertIn("g_replay_current_frame_inputs = std::nullopt", DETOUR_SOURCE)

    def test_cpp_core_has_no_live_transport(self) -> None:
        combined = SESSION_SOURCE + SESSION_HEADER
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
            self.assertNotIn(token, combined)


if __name__ == "__main__":
    unittest.main()
