#!/usr/bin/env python3
"""Static safety policy for the guarded Gate 9 steering+final runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-gate9-unified-steering-final-v1.ps1").read_text(
    encoding="utf-8"
)


class Gate9RunnerPolicyTests(unittest.TestCase):
    def test_pins_executor_and_all_reviewed_inputs(self) -> None:
        for digest in (
            "e61aab11346c38f26fdecbafb2cf30394f69cda6462964f9c4fce3165534760f",
            "3f910e22cd45da3d8ba01dd0d6df09c85aece0a7d60ab6958d8f5a88c635eed1",
            "a1d293e74eb1b633017f8c239ff39420f1f30eb3b04e29ddea6c05d07461d984",
            "cec719af384749cf6e9bdcec7f31d01334917a59756ba93c19a04d9ef20af8b8",
            "d5f1499ef61334aaae8dcedc66f3b8086e54db069625b0f09486cd8701d12dbf",
            "933c4d1dffd087edab21efcd1c35f5e13079aa4f969e41f9ec42980a2eed7681",
        ):
            self.assertIn(digest, RUNNER)

    def test_offline_validation_precedes_first_adb_call(self) -> None:
        self.assertLess(
            RUNNER.index("if ($OfflineValidateOnly)"),
            RUNNER.index("& $AdbPath devices"),
        )

    def test_requires_narrow_live_acknowledgements(self) -> None:
        for gate in (
            "$Gate8Validated",
            "$AcknowledgeRaceRunningBeforeAttach",
            "$AcknowledgeThreeFixedDeltaWrites",
            "$AcknowledgeSixSteeringWrites",
            "$AcknowledgeThreeTransformLinearCorrections",
            "$AcknowledgeAbsoluteRecordedPosesMayMoveVehicle",
            "$AcknowledgeThreeFrameIntegrationNoTrajectoryClaim",
        ):
            self.assertIn(f"if (-not {gate})", RUNNER)

    def test_uses_strict_input_and_result_verifiers(self) -> None:
        self.assertIn("verify_unified_steering_final_gate_v1.py", RUNNER)
        self.assertIn("verify_gate9_steering_final_result_v1.py", RUNNER)
        self.assertIn(
            "frames=3 skip_flags=0x7e delta_writes=3 control_writes=6 final_transactions=3 payload_writes=6",
            RUNNER,
        )

    def test_checks_tracer_before_and_after(self) -> None:
        self.assertIn("Game already has a tracer", RUNNER)
        self.assertIn("Gate 9 left a tracer attached", RUNNER)

    def test_does_not_send_keyboard_or_touch_input(self) -> None:
        lowered = RUNNER.lower()
        self.assertNotIn("shell input", lowered)
        self.assertNotIn("keyevent", lowered)
        self.assertNotIn("motionevent", lowered)


if __name__ == "__main__":
    unittest.main()
