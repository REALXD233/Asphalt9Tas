#!/usr/bin/env python3
"""Keep FC-3 from being mislabeled as current-tick OnNewTick."""

from __future__ import annotations

import pathlib
import unittest


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
PORT = WORKSPACE / "android-port"
CORRECTION = (PORT / "FC3_PHASE_ROLE_CORRECTION_20260820.md").read_text(encoding="utf-8")
PIPELINE = (PORT / "evidence" / "UNIFIED_TICK_EXECUTOR_DESIGN_20260817.md").read_text(encoding="utf-8")
PROTOCOL = (PORT / "evidence" / "FC3_REPLAY_OBSERVER_PROTOCOL_BUILD_ONLY_20260820.md").read_text(encoding="utf-8")
ROUTE = (PORT / "evidence" / "PHYSICS_FRAME_CALLBACK_ROUTE_BUILD_ONLY_20260818.md").read_text(encoding="utf-8")
DIRECTION = (PORT / "evidence" / "FC3_DIRECTION_AUDIT_AND_IDENTITY_RESOLVER_BUILD_ONLY_20260820.md").read_text(encoding="utf-8")
EXTERNAL_REVIEW = (PORT / "FC3_SUCCESSOR_EXTERNAL_REVIEW_REQUEST_20260820.md").read_text(encoding="utf-8")
PREFLIGHT = (PORT / "FC3_SUCCESSOR_MAIN_AGENT_PREFLIGHT_REVIEW_20260820.md").read_text(encoding="utf-8")
DETOURS = (WORKSPACE / "source" / "AluTasV2-main" / "AsphaltTool" / "dll" / "src" / "DetourFunctions.cpp").read_text(encoding="utf-8")


class Fc3PhaseRolePolicyTests(unittest.TestCase):
    def test_correction_has_fail_closed_policy_marker(self) -> None:
        self.assertIn("MUST_NOT_CURRENT_TICK_SELECTOR", CORRECTION)
        self.assertIn("Gate 12 remains closed", CORRECTION)
        self.assertIn("off-by-one", CORRECTION)

    def test_live_pipeline_places_early_consumers_before_callback(self) -> None:
        order = "DT -> C98 -> completion / callback open -> C9C -> F64 -> callback close"
        self.assertIn(order, PIPELINE)
        self.assertIn("fixed-delta event has already occurred", CORRECTION)
        self.assertIn("C98 control-consumption event has already occurred", CORRECTION)

    def test_upstream_on_new_tick_precedes_brake_consumption(self) -> None:
        start = DETOURS.index("void REROUTE_FUNCTION Detour_BrakeValue")
        end = DETOURS.index("RealBrakeValueCall(p_this, p_value_ptr);", start)
        body = DETOURS[start:end]
        self.assertLess(body.index("StateManager::OnNewTick();"), body.index("m_brake_value"))

    def test_fc3_protocol_does_not_carry_inputs(self) -> None:
        self.assertIn("报告中没有 steering、brake、accelerator", PROTOCOL)
        self.assertIn("not a proof that the FC-3 dedicated callback is current-tick", DIRECTION)

    def test_older_route_has_visible_clarification(self) -> None:
        first = "\n".join(ROUTE.splitlines()[:14])
        self.assertIn("phase-role clarification", first)
        self.assertIn("not established as the", first)

    def test_cross_proof_and_startup_are_mandatory(self) -> None:
        for token in (
            "Required tick-index cross-proof",
            "DT[k+1]",
            "Startup problem",
            "preload and validate packet zero",
            "must not infer tick identity from wall-clock proximity",
        ):
            self.assertIn(token, CORRECTION)

    def test_review_documents_cannot_waive_phase_correction(self) -> None:
        for text in (EXTERNAL_REVIEW, PREFLIGHT):
            self.assertIn("FC3_PHASE_ROLE_CORRECTION_20260820.md", text)
            self.assertIn("current-tick `OnNewTick`", text)
            self.assertIn("Gate 12", text)


if __name__ == "__main__":
    unittest.main()
