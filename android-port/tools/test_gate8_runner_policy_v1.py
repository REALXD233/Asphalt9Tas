#!/usr/bin/env python3
"""Static safety policy for the guarded Gate 8 final-correction runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-gate8-unified-final-correction-v1.ps1").read_text(
    encoding="utf-8"
)


class Gate8RunnerPolicyTests(unittest.TestCase):
    def test_pins_reviewed_executor_hash(self) -> None:
        self.assertIn(
            "e61aab11346c38f26fdecbafb2cf30394f69cda6462964f9c4fce3165534760f",
            RUNNER,
        )

    def test_pins_all_reviewed_gate_artifacts(self) -> None:
        for digest in (
            "ab1c7c9d38ca99f2f81c5e520792b883d28f74ed2476766196bacfdd7fa6e380",
            "af2db7cf887527f60df50db3a58fc164a8dbcaedb54fe939ba3d50d690eda160",
            "cec719af384749cf6e9bdcec7f31d01334917a59756ba93c19a04d9ef20af8b8",
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
            "$Gate5Validated",
            "$Gate7Validated",
            "$AcknowledgeRaceRunningBeforeAttach",
            "$AcknowledgeOneFixedDeltaWrite",
            "$AcknowledgeOneTransformLinearCorrection",
            "$AcknowledgeAbsoluteRecordedPoseMayMoveVehicle",
            "$AcknowledgeSingleFrameNoTrajectoryClaim",
        ):
            self.assertIn(f"if (-not {gate})", RUNNER)

    def test_uses_final_input_and_result_verifiers(self) -> None:
        self.assertIn("verify_unified_final_correction_gate_v1.py", RUNNER)
        self.assertIn("verify_gate8_final_correction_result_v1.py", RUNNER)
        self.assertIn("skip_flags=0x7f control_writes=0 final_transactions=1", RUNNER)

    def test_checks_tracer_before_and_after(self) -> None:
        self.assertIn("Game already has a tracer", RUNNER)
        self.assertIn("Gate 8 left a tracer attached", RUNNER)

    def test_does_not_send_keyboard_or_touch_input(self) -> None:
        lowered = RUNNER.lower()
        self.assertNotIn("shell input", lowered)
        self.assertNotIn("keyevent", lowered)
        self.assertNotIn("motionevent", lowered)


if __name__ == "__main__":
    unittest.main()
