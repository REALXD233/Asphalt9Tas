#!/usr/bin/env python3
"""Static safety policy for the guarded Gate 7 steering runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-gate7-unified-steering-v1.ps1").read_text(encoding="utf-8")


class Gate7RunnerPolicyTests(unittest.TestCase):
    def test_pins_reviewed_executor_hash(self) -> None:
        self.assertIn(
            "e61aab11346c38f26fdecbafb2cf30394f69cda6462964f9c4fce3165534760f",
            RUNNER,
        )

    def test_offline_validation_precedes_first_adb_call(self) -> None:
        self.assertLess(
            RUNNER.index("if ($OfflineValidateOnly)"),
            RUNNER.index("& $AdbPath devices"),
        )

    def test_requires_narrow_live_acknowledgements(self) -> None:
        for gate in (
            "$Gate6Validated",
            "$AcknowledgeRaceRunningBeforeAttach",
            "$AcknowledgeOneFixedDeltaWrite",
            "$AcknowledgeTwoSteeringWrites",
            "$AcknowledgeSingleFrameNoTrajectoryClaim",
        ):
            self.assertIn(f"if (-not {gate})", RUNNER)

    def test_uses_steering_input_and_result_verifiers(self) -> None:
        self.assertIn("verify_unified_steering_gate_v1.py", RUNNER)
        self.assertIn("verify_gate7_steering_result_v1.py", RUNNER)
        self.assertIn("skip_flags=0xfe steering_writes=2 final_writes=0", RUNNER)

    def test_checks_tracer_before_and_after(self) -> None:
        self.assertIn("Game already has a tracer", RUNNER)
        self.assertIn("Gate 7 left a tracer attached", RUNNER)

    def test_does_not_send_keyboard_or_touch_input(self) -> None:
        lowered = RUNNER.lower()
        self.assertNotIn("shell input", lowered)
        self.assertNotIn("keyevent", lowered)
        self.assertNotIn("motionevent", lowered)


if __name__ == "__main__":
    unittest.main()
