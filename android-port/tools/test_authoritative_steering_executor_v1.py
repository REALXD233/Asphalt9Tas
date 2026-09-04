#!/usr/bin/env python3
"""Offline source and bound-artifact tests for A9AST1."""

from __future__ import annotations

import hashlib
import pathlib
import struct
import unittest

from unified_tick_recording_v1 import decode_recording

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE_PATH = ROOT / "android-port/src/hwbp_authoritative_steering_v1.cpp"
SOURCE = SOURCE_PATH.read_text(encoding="utf-8")
RECORDING = ROOT / "android-port/evidence/a9tas_authoritative_steering_projection_438f_20260821.a9utk1"
ANCHOR = ROOT / "android-port/evidence/a9tas_authoritative_steering_projection_438f_20260821.a9npa1"


class AuthoritativeSteeringExecutorTests(unittest.TestCase):
    def test_only_two_narrow_gameplay_write_primitives_exist(self) -> None:
        self.assertEqual(SOURCE.count("pwrite("), 2)
        self.assertEqual(SOURCE.count("WriteFixedDeltaVerified("), 2)
        self.assertEqual(SOURCE.count("WritePairVerified("), 2)
        self.assertIn("pwrite(mem, &value, sizeof(value)", SOURCE)
        self.assertIn("pwrite(mem, &plan.pair_intended", SOURCE)
        for forbidden in ("process_vm_writev", "PTRACE_POKEDATA", "PTRACE_POKETEXT", "RemoteCall", "NitroEnable", "ApplySteering"):
            self.assertNotIn(forbidden, SOURCE)

    def test_recording_declared_runtime_ceiling_is_fail_closed(self) -> None:
        for token in (
            "kMinimumFrames = 1",
            "kMaximumFrames = 36000",
            "kRequiredFixedUs = 16667",
            "I_ACCEPT_RECORDING_DECLARED_DELTA_AND_2X_STEERING_WRITES_V2",
            "report.delta_write_attempts == required_frames",
            "report.pair_write_attempts == required_frames * 2",
            "report.nonzero_steering_frames == expected_nonzero",
        ):
            self.assertIn(token, SOURCE)

    def test_incomplete_search_cycle_recovers_without_enabling_replay_recovery(self) -> None:
        recovery = SOURCE.index("search_world_gap_recovery_failed")
        search_guard = SOURCE.rfind("searching && state.phase_stage == 7u", 0, recovery)
        replay_write = SOURCE.index("WriteFixedDeltaVerified(", SOURCE.index("int main"))
        self.assertGreaterEqual(search_guard, 0)
        self.assertLess(recovery, replay_write)
        self.assertIn("++report.rejected_search_prefixes", SOURCE[search_guard:recovery])
        self.assertIn("RejectSearchPrefix(controller)", SOURCE[search_guard:recovery])

    def test_failure_report_is_preserved_after_detach(self) -> None:
        detach = SOURCE.index("for (auto& thread : threads)")
        report = SOURCE.index("const bool report_ok = WriteReport", detach)
        self.assertLess(detach, report)
        self.assertNotIn("success && WriteReport", SOURCE)

    def test_search_path_reaches_writes_only_after_replay_mode(self) -> None:
        delta_branch = SOURCE.index("state.mode == ModeV1::kReplay")
        delta_write = SOURCE.index("WriteFixedDeltaVerified(", SOURCE.index("int main"))
        pair_guard = SOURCE.index("if (replay && !selected_valid)")
        pair_write = SOURCE.index("WritePairVerified(mem", pair_guard)
        self.assertLess(delta_branch, delta_write)
        self.assertLess(pair_guard, pair_write)

    def test_every_other_capability_is_absent(self) -> None:
        self.assertIn("report.gameplay_action_calls == 0", SOURCE)
        self.assertIn("report.physics_correction_writes == 0", SOURCE)
        for token in ("nitro_activation_count)", "respawn_button_press)", "native_pose_address, frame", "native_linear_address, frame"):
            self.assertNotIn(token, SOURCE)

    def test_bound_recording_is_fixed_environment_438_frame_projection(self) -> None:
        fixed, frames = decode_recording(RECORDING.read_bytes())
        nonzero = [index for index, frame in enumerate(frames) if struct.pack("<f", frame.steering) != bytes(4)]
        self.assertEqual((fixed, len(frames), len(nonzero)), (16667, 438, 33))
        self.assertEqual((nonzero[0], nonzero[-1]), (405, 437))
        self.assertTrue(all(frame.skip_flags == 0xFE for frame in frames))

    def test_prior_write_neutral_live_source_remains_frozen(self) -> None:
        neutral = ROOT / "android-port/src/hwbp_authoritative_neutral_observer_v1.cpp"
        self.assertEqual(hashlib.sha256(neutral.read_bytes()).hexdigest(),
                         "3ad4aac949fa9fdc74ec517e7fb5bdd3f843d5a0d460e6a9a5fcf0fa9242058d")
        self.assertEqual(hashlib.sha256(ANCHOR.read_bytes()).hexdigest(),
                         "9ac31fa042f56e1fb4dc5af45df8ddb61e9ea229c850c2fbe8b974778ef4a986")


if __name__ == "__main__":
    unittest.main()
