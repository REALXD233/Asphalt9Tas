#!/usr/bin/env python3
"""Static safety policy for the guarded Gate 6 live runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-gate6-unified-phase-only-v1.ps1").read_text(encoding="utf-8")


class Gate6RunnerPolicyTests(unittest.TestCase):
    def test_pins_reviewed_executor_hash(self) -> None:
        self.assertIn("e61aab11346c38f26fdecbafb2cf30394f69cda6462964f9c4fce3165534760f", RUNNER)

    def test_offline_validation_returns_before_first_adb_call(self) -> None:
        self.assertLess(RUNNER.index("if ($OfflineValidateOnly)"), RUNNER.index("& $AdbPath devices"))

    def test_requires_all_live_acknowledgements(self) -> None:
        for gate in (
            "$Gate2Validated",
            "$Gate5Validated",
            "$AcknowledgeRaceRunningBeforeAttach",
            "$AcknowledgeOneFixedDeltaWrite",
            "$AcknowledgePhaseSwitchOnlyNoReplayClaim",
        ):
            self.assertIn(f"if (-not {gate})", RUNNER)

    def test_uses_strict_input_and_cross_binding_verifiers(self) -> None:
        self.assertIn("verify_unified_phase_only_gate_v1.py", RUNNER)
        self.assertIn("verify_gate6_phase_only_result_v1.py", RUNNER)
        self.assertIn("skip_flags=0xff gameplay_actions=0 final_writes=0", RUNNER)
        self.assertIn("fixed_delta_writes=1", RUNNER)

    def test_checks_tracer_before_and_after(self) -> None:
        self.assertIn("Game already has a tracer", RUNNER)
        self.assertIn("Gate 6 left a tracer attached", RUNNER)

    def test_does_not_capture_or_send_input(self) -> None:
        lowered = RUNNER.lower()
        self.assertNotIn("native_physics_recorder", lowered)
        self.assertNotIn("shell input", lowered)
        self.assertNotIn("keyevent", lowered)


if __name__ == "__main__":
    unittest.main()
