#!/usr/bin/env python3
"""Source-bound tests for the upstream ReplayStateManager model."""

from __future__ import annotations

import pathlib
import unittest

from upstream_replay_control_plane_v1 import ControlPlaneV1, ReplayV1


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
MANAGER = (WORKSPACE / "source" / "AluTasV2-main" / "AsphaltTool" / "tool" / "src" / "globalstate" / "ReplayStateManager.cpp").read_text(encoding="utf-8")
REPLAY = (WORKSPACE / "source" / "AluTasV2-main" / "AsphaltTool" / "tool" / "src" / "common" / "Replay.cpp").read_text(encoding="utf-8")


class UpstreamReplayControlPlaneTests(unittest.TestCase):
    def test_replay_requires_tick_equal_index(self) -> None:
        with self.assertRaisesRegex(ValueError, "indices"):
            ReplayV1((1, 2, 3))

    def test_queue_outside_race_arms_mode_and_fixed_interval(self) -> None:
        replay = ReplayV1((0, 1, 2), 16667)
        state, accepted = ControlPlaneV1(8).queue_replay(
            replay, 99, in_race=False, current_race_tick=0
        )
        self.assertTrue(accepted)
        self.assertTrue(state.block_mode_active)
        self.assertEqual(state.fixed_interval_us, 16667)
        self.assertEqual(state.session.final_tick, 2)
        self.assertEqual(state.queue, ())

    def test_unfinished_in_race_session_rejects_replacement(self) -> None:
        replay = ReplayV1((0, 1, 2, 3))
        state, _ = ControlPlaneV1(8).queue_replay(
            replay, 3, in_race=False, current_race_tick=0
        )
        unchanged, accepted = state.queue_replay(
            replay, 1, in_race=True, current_race_tick=2
        )
        self.assertFalse(accepted)
        self.assertEqual(unchanged, state)

    def test_partial_target_prefills_once_without_duplicate(self) -> None:
        replay = ReplayV1((0, 1, 2, 3, 4))
        state, _ = ControlPlaneV1(8).queue_replay(
            replay, 2, in_race=False, current_race_tick=0
        )
        state = state.on_update(current_race_tick=0)
        self.assertEqual(state.queue, (0, 1, 2))
        self.assertEqual(state.session.frame_index, 3)

    def test_last_target_repeats_last_packet_until_capacity(self) -> None:
        replay = ReplayV1((0, 1, 2))
        state, _ = ControlPlaneV1(7).queue_replay(
            replay, 2, in_race=False, current_race_tick=0
        )
        state = state.on_update(current_race_tick=0)
        self.assertEqual(state.queue, (0, 1, 2, 2, 2, 2, 2))
        self.assertEqual(state.session.frame_index, 2)

    def test_completion_check_precedes_refill(self) -> None:
        replay = ReplayV1((0, 1, 2))
        state, _ = ControlPlaneV1(8).queue_replay(
            replay, 2, in_race=False, current_race_tick=0
        )
        state = state.on_update(current_race_tick=2)
        self.assertFalse(state.block_mode_active)
        self.assertEqual(state.queue, ())

    def test_target_zero_disables_before_first_update_refill(self) -> None:
        replay = ReplayV1((0, 1))
        state, _ = ControlPlaneV1(8).queue_replay(
            replay, 0, in_race=False, current_race_tick=0
        )
        state = state.on_update(current_race_tick=0)
        self.assertFalse(state.block_mode_active)
        self.assertEqual(state.queue, ())

    def test_race_end_rearms_session_then_clears_queue(self) -> None:
        replay = ReplayV1((0, 1, 2))
        state, _ = ControlPlaneV1(8).queue_replay(
            replay, 2, in_race=False, current_race_tick=0
        )
        state = state.on_update(current_race_tick=0)
        self.assertTrue(state.queue)
        ended = state.on_race_ended()
        self.assertTrue(ended.block_mode_active)
        self.assertEqual(ended.session.frame_index, 0)
        self.assertEqual(ended.queue, ())

    def test_model_is_bound_to_literal_upstream_order_and_saturation(self) -> None:
        self.assertIn("PushPlaybackFramesToInputInBuffer", MANAGER)
        disable = MANAGER.index("dll_out_state->m_replay_inputs.m_race_frame_tick >=")
        push = MANAGER.index("PushPlaybackFramesToInputInBuffer();", disable)
        self.assertLess(disable, push)
        self.assertIn("std::min(m_current_frame_index + count, m_frames.size() - 1)", REPLAY)
        self.assertIn("m_frames[i].m_replay_input.m_race_frame_tick != i", REPLAY)

    def test_v3_fields_are_parsed_but_not_copied_to_output_frame(self) -> None:
        copy_start = REPLAY.index("const Communication::DllOut::RecordedRacerState& raw")
        copy_end = REPLAY.index("replay.EmplaceBackFrame", copy_start)
        copy_block = REPLAY[copy_start:copy_end]
        for present in ("m_racer_transform_mat4x4", "m_racer_velocity_vec3", "m_nitro_bar_value"):
            self.assertIn(present, copy_block)
        for missing in ("m_race_progress_percentage", "m_rpm", "m_checkpoint", "m_gear"):
            self.assertNotIn(missing, copy_block)


if __name__ == "__main__":
    unittest.main()
