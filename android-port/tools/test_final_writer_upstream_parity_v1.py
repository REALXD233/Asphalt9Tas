#!/usr/bin/env python3
"""Cross-check Android final-writer ordering against AluTasV2 source."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
UPSTREAM = ROOT / "source" / "AluTasV2-main" / "AsphaltTool" / "dll" / "src" / "DetourFunctions.cpp"
PAYLOAD = ROOT / "android-port" / "src" / "payload_final_writer_replay_v1.cpp"
EXECUTOR = ROOT / "android-port" / "src" / "hwbp_unified_tick_executor_v1.cpp"


class UpstreamParityTests(unittest.TestCase):
    def test_final_writer_exact_semantic_order(self) -> None:
        upstream = UPSTREAM.read_text(encoding="utf-8")
        block = re.search(
            r"Detour_FinalRacerTransformWriter\(.*?\n\s*}\n\s*bool SetupHook",
            upstream,
            re.DOTALL,
        )
        self.assertIsNotNone(block)
        text = block.group(0)
        self.assertIn("g_replay_current_frame_inputs.has_value()", text)
        positions = [
            text.index("RealFinalRacerTransformWriterCall(a1, a2, a3)"),
            text.index("TRANSFORM_FORCED"),
            text.index("*curr_trans != inputs.m_racer_transform_mat4x4"),
            text.index("std::memcpy(curr_trans->Data()"),
            text.index("std::memcpy(curr_velo->Data()"),
        ]
        self.assertEqual(positions, sorted(positions))

        payload = PAYLOAD.read_text(encoding="utf-8")
        wrapper = re.search(
            r"a9tas_final_writer_replay_callback_v1\(.*?\n}\n\nextern \"C\"",
            payload,
            re.DOTALL,
        )
        self.assertIsNotNone(wrapper)
        body = wrapper.group(0)
        permit = body.index("__atomic_compare_exchange_n")
        active_original = body.index(
            "const std::int64_t result = original(object, frame_token);"
        )
        android_positions = [
            body.index("original(object, frame_token)"),
            body.index("ComponentsEqual(pose, linear, target)"),
            body.index("CopyBytes(pose, target.transform"),
            body.index("CopyBytes(linear, target.linear"),
        ]
        self.assertEqual(android_positions, sorted(android_positions))
        self.assertLess(permit, active_original)
        self.assertIn("return original(object, frame_token);", body[:active_original])
        self.assertEqual(body.count("const std::int64_t result = original(object, frame_token);"), 1)
        self.assertIn("return result;", body)

    def test_executor_does_not_repeat_late_correction(self) -> None:
        executor = EXECUTOR.read_text(encoding="utf-8")
        acknowledge = executor.index("AcknowledgeAtCallbackClose")
        legacy_pose_write = executor.index(
            "WriteExactVerified(\n                                                mem,\n                                                backend.native_pose_address",
            acknowledge,
        )
        guard_else = executor.rfind("#else", acknowledge, legacy_pose_write)
        self.assertGreater(guard_else, acknowledge)
        self.assertIn("InstallAtCertifiedPrefix", executor)
        self.assertIn("InstallBeforeResume", executor)
        self.assertIn("CommitAtWorldBoundary", executor)

    def test_fixed_delta_matches_upstream_after_physics_entry_anchor(self) -> None:
        upstream = UPSTREAM.read_text(encoding="utf-8")
        physics = re.search(
            r"Detour_OnNewFrameWithPhysics\(.*?\n\s*}\n\s*bool SetupHook",
            upstream,
            re.DOTALL,
        )
        self.assertIsNotNone(physics)
        body = physics.group(0)
        forced = body.index("*p_in_delta_micros =")
        optional_actions = body.index(
            "g_replay_current_frame_inputs.has_value()", forced
        )
        original = body.index("RealNewPhysicsFrameCall", optional_actions)
        self.assertLess(forced, optional_actions)
        self.assertLess(optional_actions, original)

        executor = EXECUTOR.read_text(encoding="utf-8")
        self.assertIn(
            "delta_us > 0\n#ifndef A9TAS_FINAL_WRITER_REPLAY_V1\n"
            "                               && delta_us <= 1000000",
            executor,
        )
        self.assertIn("FinalWriterStartAnchorStage::kAwaitC98", executor)
        self.assertIn("FinalWriterStartAnchorStage::kAwaitC9C", executor)
        self.assertIn("FinalWriterStartAnchorStage::kAwaitDeltaZero", executor)
        self.assertIn("final_writer_start_anchor_complete", executor)

        # Countdown/menu accumulator cycles are not the Android equivalent of
        # the upstream OnNewFrameWithPhysics entry.  A complete input cycle is
        # observed without writes, then the following positive delta is frame
        # zero.  Once synchronized, no one-second ceiling is applied: the
        # upstream hook overwrites p_in_delta_micros unconditionally.
        stage = "c98"
        owner = None
        selected = []
        events = (
            ("delta", 9709, 30_499_650),
            ("zero", 9709, 0),
            ("delta", 9709, 15_968),
            ("zero", 9709, 0),
            ("delta", 9709, 16_021),
            ("c98", 9709, 0),
            ("c9c", 9709, 0),
            ("zero", 9709, 0),
            ("delta", 9709, 2_000_000),
        )
        for kind, tid, value in events:
            if stage == "ready":
                if kind == "delta" and value > 0:
                    selected.append(value)
                continue
            if stage == "c98" and kind == "c98":
                stage, owner = "c9c", tid
            elif stage == "c9c" and kind == "c9c" and tid == owner:
                stage = "zero"
            elif stage == "zero" and kind == "zero" and tid == owner:
                stage, owner = "ready", None
        self.assertEqual(selected, [2_000_000])


if __name__ == "__main__":
    unittest.main()
