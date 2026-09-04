#!/usr/bin/env python3
"""Static policy gates for the build-only unified HWBP executor."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_unified_tick_executor_v1.cpp").read_text(encoding="utf-8")
CORE = (ROOT / "src" / "unified_tick_executor_core_v1.cpp").read_text(encoding="utf-8")


class UnifiedExecutorBuildPolicyTests(unittest.TestCase):
    def test_no_deployment_runner_exists(self) -> None:
        self.assertFalse((ROOT / "run-hwbp-unified-tick-executor-v1.ps1").exists())

    def test_capability_gate_precedes_target_open(self) -> None:
        gate = SOURCE.index("RuntimeCapabilitiesSupported(frames")
        process_open = SOURCE.index('open(mem_path, O_RDWR')
        self.assertLess(gate, process_open)
        for required in (
            "kSkipBrake",
            "kSkipNitroActivation",
            "kSkipAccelerator",
            "kSkipBarrelAngular",
            "kSkipBarrelRbx",
            "kSkipRespawnButton",
        ):
            self.assertIn(required, SOURCE)

    def test_full_gate2_order_is_modeled_and_c9c_prefix_is_certified(self) -> None:
        positions = [
            CORE.index("event == Event::kCompletion"),
            CORE.index("event == Event::kCallbackOpen"),
            CORE.index("event == Event::kF64"),
            CORE.index("event == Event::kCallbackClose"),
            CORE.index("event == Event::kWorldCommit"),
        ]
        self.assertEqual(positions, sorted(positions))
        for event in (
            "kPrefixCertified",
            "kF64",
            "kCallbackClose",
            "kDeferredCallbackClear",
            "kWorldCommit",
        ):
            self.assertIn(f"Event::{event}", SOURCE)
        for evidence in (
            "pending.completion_before",
            "pending.completion_after",
            "pending.callback_flags_at_c9c",
            '"completion_unchanged_at_c9c"',
            '"callback_not_open_at_c9c"',
            "pending.callback_deferred_clear_event",
            '"deferred_callback_clear_rejected"',
            "pending.commit_tid",
            "BoundaryDr7(true)",
            "pending.steering_bits",
            "pending.c98_pair_after",
            "pending.c9c_pair_after",
        ):
            self.assertIn(evidence, SOURCE)

    def test_phase_switch_disables_dr7_first(self) -> None:
        function = SOURCE[SOURCE.index("bool ProgramStoppedThread") : SOURCE.index("bool ComponentPayloadEqualUnified")]
        self.assertLess(function.index("PokeDebug(tid, 7, 0)"), function.index("PokeDebug(tid, 0, dr0)"))

    def test_unified_abi_constants_cannot_bind_to_included_observer_names(self) -> None:
        header_validator = CORE[CORE.index("bool ValidHeader") : CORE.index("bool ValidFrame")]
        frame_validator = CORE[CORE.index("bool ValidFrame") : CORE.index("bool LoadRecording")]
        self.assertNotIn("using namespace", header_validator + frame_validator)
        for constant in (
            "kMagic",
            "kVersion",
            "kMaximumFrames",
            "kRequiredHeaderFlags",
            "kSupportedBuildId",
            "kRequiredFrameFlags",
        ):
            self.assertIn(f"unified::{constant}", header_validator + frame_validator)

    def test_rejected_control_flow_and_setter_paths_absent(self) -> None:
        for forbidden in (
            "ptrace_singlestep",
            "process_vm_writev",
            "pose setter",
            "linear setter",
            "trampoline",
        ):
            self.assertNotIn(forbidden, SOURCE.lower())

    def test_runtime_fails_on_first_paused_candidate_instead_of_rewriting(self) -> None:
        marker = 'semantic_fault("paused_cycle_after_delta_write"'
        self.assertEqual(SOURCE.count(marker), 1)
        normal_branch = SOURCE[
            SOURCE.index("// V1 cannot distinguish a paused candidate") :
            SOURCE.index("semantic_fault(\"unexpected_delta\"")
        ]
        self.assertNotIn("Event::kDeltaZero", normal_branch)


if __name__ == "__main__":
    unittest.main()
