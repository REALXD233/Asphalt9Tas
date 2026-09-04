#!/usr/bin/env python3
"""Pin the Android action contract to the checked-in AluTasV2 source."""

from __future__ import annotations

import pathlib
import re
import unittest


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
UPSTREAM = WORKSPACE / "source" / "AluTasV2-main" / "AsphaltTool"
DETOURS = (UPSTREAM / "dll" / "src" / "DetourFunctions.cpp").read_text(
    encoding="utf-8"
)
COMMUNICATION = (UPSTREAM / "shared" / "src" / "Communication.h").read_text(
    encoding="utf-8"
)
REPLAY = (UPSTREAM / "tool" / "src" / "common" / "Replay.cpp").read_text(
    encoding="utf-8"
)


class OriginalSourceActionParityTests(unittest.TestCase):
    def test_packet_contains_raw_controls_call_count_and_edge(self) -> None:
        for field in (
            "m_steer_value",
            "m_brake_value",
            "m_nitro_activation_count_this_frame",
            "m_accelerator_value",
            "m_respawn_button_press",
            "m_barrel_angular_velocities_vec3",
            "m_value_rbx_2228",
            "m_value_rbx_222C",
        ):
            self.assertIn(field, COMMUNICATION)

    def test_skip_bits_match_a9utk1_exactly(self) -> None:
        expected = {
            "STEER": 0,
            "BRAKE": 1,
            "NITRO_ACTIVATION": 2,
            "ACCELERATOR": 3,
            "BARREL_ANGULAR": 4,
            "BARREL_RBX": 5,
            "RESPAWN_BUTTON": 6,
            "TRANSFORM_FORCED": 7,
        }
        for name, bit in expected.items():
            self.assertRegex(
                COMMUNICATION, rf"{name}\s*=\s*1\s*<<\s*{bit}\b"
            )

    def test_fixed_delta_then_nitro_then_respawn_precede_real_physics(self) -> None:
        start = DETOURS.index("uintptr_t* Detour_OnNewFrameWithPhysics(")
        end = DETOURS.index("void QueueSkipSubsequentTicks", start)
        body = DETOURS[start:end]
        delta = body.index(
            "*p_in_delta_micros = GameDLLState::g_current_state.m_meta_data.m_fixed_frame_interval_micros"
        )
        nitro = body.index("SpoofCallToEnableNitroFunction", delta)
        respawn = body.index("SpoofCallToRespawnInputFunc", nitro)
        physics = body.index("RealNewPhysicsFrameCall", respawn)
        self.assertLess(delta, nitro)
        self.assertLess(nitro, respawn)
        self.assertLess(respawn, physics)

    def test_nitro_replay_calls_real_function_exact_count(self) -> None:
        start = DETOURS.index("void SpoofCallToEnableNitroFunction")
        end = DETOURS.index("bool SetupHook()", start)
        body = DETOURS[start:end]
        self.assertIn("while (count-- > 0)", body)
        self.assertIn("RealNitroEnableCall", body)
        self.assertIn("m_nitro_activation_count_this_frame++", body)

    def test_end_tick_resets_only_transient_action_state(self) -> None:
        start = DETOURS.index("void OnEndTick()")
        end = DETOURS.index("// Does not lock, caller must lock", start)
        body = DETOURS[start:end]
        for field in (
            "m_barrel_angular_velocities_vec3",
            "m_value_rbx_2228",
            "m_value_rbx_222C",
            "m_respawn_button_press",
            "m_nitro_activation_count_this_frame",
        ):
            self.assertIn(field, body)
        for continuous in (
            "m_steer_value",
            "m_brake_value",
            "m_accelerator_value",
        ):
            self.assertNotIn(continuous, body)

    def test_replay_serializes_nitro_activations_not_colours(self) -> None:
        self.assertIn('NITRO_ACTIVATIONS[]         = "NitroActivations"', REPLAY)
        self.assertIn("m_nitro_activation_count_this_frame", REPLAY)
        self.assertIsNone(
            re.search(r"nitro_(yellow|blue|purple|red)|yellow_nitro|blue_nitro", REPLAY, re.I)
        )


if __name__ == "__main__":
    unittest.main()
