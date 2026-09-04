#!/usr/bin/env python3
"""Source-bound tests for the authoritative tick state-machine model."""

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


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
DETOURS = (WORKSPACE / "source" / "AluTasV2-main" / "AsphaltTool" / "dll" / "src" / "DetourFunctions.cpp").read_text(encoding="utf-8")
COMMUNICATION = (WORKSPACE / "source" / "AluTasV2-main" / "AsphaltTool" / "shared" / "src" / "Communication.h").read_text(encoding="utf-8")


class AuthoritativeTickStateMachineTests(unittest.TestCase):
    def test_block_mode_empty_queue_remains_blocked(self) -> None:
        result = select_on_new_tick(
            current_tick=7, mode=ReplayMode.ACTIVE_BLOCK_THREAD, queue=[]
        )
        self.assertEqual(result.outcome, SelectionOutcome.BLOCKED_EMPTY)

    def test_block_mode_drains_all_stale_then_consumes_exact(self) -> None:
        packets = [ReplayPacketV1(3), ReplayPacketV1(6), ReplayPacketV1(7, "frame"), ReplayPacketV1(8)]
        result = select_on_new_tick(
            current_tick=7, mode=ReplayMode.ACTIVE_BLOCK_THREAD, queue=packets
        )
        self.assertEqual(result.outcome, SelectionOutcome.EXACT_PACKET)
        self.assertEqual(result.selected.payload, "frame")
        self.assertEqual(result.stale_dropped, 2)
        self.assertEqual([packet.tick for packet in result.remaining], [8])

    def test_block_mode_future_head_is_gap_not_wait(self) -> None:
        future = ReplayPacketV1(8)
        result = select_on_new_tick(
            current_tick=7,
            mode=ReplayMode.ACTIVE_BLOCK_THREAD,
            queue=[future],
        )
        self.assertEqual(result.outcome, SelectionOutcome.FUTURE_HEAD_GAP)
        self.assertIsNone(result.selected)
        self.assertEqual(result.remaining, (future,))

    def test_block_mode_can_be_cancelled_by_general_buffer(self) -> None:
        result = select_on_new_tick(
            current_tick=7,
            mode=ReplayMode.ACTIVE_BLOCK_THREAD,
            queue=[],
            block_mode_still_active=False,
        )
        self.assertEqual(result.outcome, SelectionOutcome.MODE_CANCELLED)

    def test_no_block_drops_at_most_one_stale_packet(self) -> None:
        packets = [ReplayPacketV1(4), ReplayPacketV1(5), ReplayPacketV1(7)]
        result = select_on_new_tick(
            current_tick=7, mode=ReplayMode.ACTIVE_NO_BLOCK, queue=packets
        )
        self.assertEqual(result.outcome, SelectionOutcome.STALE_DROPPED_NO_BLOCK)
        self.assertEqual([packet.tick for packet in result.remaining], [5, 7])

    def test_outside_race_and_version_mismatch_do_not_touch_queue(self) -> None:
        packet = ReplayPacketV1(7)
        outside = select_on_new_tick(
            current_tick=7,
            mode=ReplayMode.ACTIVE_BLOCK_THREAD,
            queue=[packet],
            in_race=False,
        )
        mismatch = select_on_new_tick(
            current_tick=7,
            mode=ReplayMode.ACTIVE_BLOCK_THREAD,
            queue=[packet],
            communication_version_matches=False,
        )
        self.assertEqual(outside.outcome, SelectionOutcome.OUTSIDE_RACE)
        self.assertEqual(mismatch.outcome, SelectionOutcome.VERSION_MISMATCH)
        self.assertEqual(outside.remaining, (packet,))
        self.assertEqual(mismatch.remaining, (packet,))
        self.assertTrue(outside.clear_previous_packet)
        self.assertTrue(mismatch.clear_previous_packet)

    def test_every_normal_selection_clears_previous_packet(self) -> None:
        cases = (
            select_on_new_tick(current_tick=7, mode=ReplayMode.INACTIVE, queue=[]),
            select_on_new_tick(current_tick=7, mode=ReplayMode.ACTIVE_BLOCK_THREAD, queue=[]),
            select_on_new_tick(current_tick=7, mode=ReplayMode.ACTIVE_BLOCK_THREAD, queue=[ReplayPacketV1(8)]),
            select_on_new_tick(current_tick=7, mode=ReplayMode.ACTIVE_NO_BLOCK, queue=[]),
        )
        self.assertTrue(all(result.clear_previous_packet for result in cases))

    def test_end_tick_publishes_before_clearing_only_transients(self) -> None:
        state = TickInputStateV1(
            race_tick=9,
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
        self.assertEqual(result.next_state.race_tick, 10)
        self.assertEqual(
            (result.next_state.steer, result.next_state.brake, result.next_state.accelerator),
            (0.25, -1.0, 0.75),
        )
        self.assertEqual(result.next_state.nitro_activation_count, 0)
        self.assertFalse(result.next_state.respawn)
        self.assertEqual(result.next_state.barrel_angular, (0.0, 0.0, 0.0))
        self.assertEqual(result.next_state.barrel_rbx, (0.0, 0.0))

    def test_end_tick_outside_race_does_not_increment(self) -> None:
        result = end_tick(TickInputStateV1(race_tick=12, respawn=True), in_race=False)
        self.assertEqual(result.next_state.race_tick, 12)
        self.assertFalse(result.next_state.respawn)

    def test_model_is_bound_to_literal_upstream_branches(self) -> None:
        start = DETOURS.index("void OnNewTick()")
        end = DETOURS.index("void OnEndTick()", start)
        body = DETOURS[start:end]
        self.assertIn("pkt.m_race_frame_tick == current_tick", body)
        self.assertIn("pkt.m_race_frame_tick < current_tick", body)
        self.assertIn("else break;", body)
        self.assertIn("g_replay_current_frame_inputs = std::nullopt", body)
        self.assertIn("ActiveBlockThread", COMMUNICATION)
        self.assertIn("ActiveNoBlock", COMMUNICATION)


if __name__ == "__main__":
    unittest.main()
