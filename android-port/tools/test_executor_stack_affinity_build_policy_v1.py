#!/usr/bin/env python3

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src" / "hwbp_worker_stack_scope_observer_v1.cpp").read_text(
    encoding="utf-8"
)
WRAPPER = (ROOT / "src" / "hwbp_executor_stack_affinity_v1.cpp").read_text(
    encoding="utf-8"
)
LEGACY_P1_RUNNER = (
    ROOT / "run-physics-executor-affinity-p1-v1.ps1"
).read_text(encoding="utf-8")
AFFINITY_RUNNER = (
    ROOT / "run-hwbp-executor-stack-affinity-v1.ps1"
).read_text(encoding="utf-8")


class ExecutorStackAffinityBuildPolicyTests(unittest.TestCase):
    def test_mode_is_explicit_and_stack_is_bounded(self) -> None:
        self.assertIn("#define A9TAS_EXECUTOR_AFFINITY_STACK 1", WRAPPER)
        self.assertIn("kCapturedStackWords = 256", SOURCE)
        self.assertIn("capture=stack256", SOURCE)

    def test_exact_guest_targets_are_signature_gated(self) -> None:
        self.assertIn("kPhysicsExecutorRva = 0x38B74DC", SOURCE)
        self.assertIn("kPhysicsExecutorSignature[16]", SOURCE)
        self.assertIn("physics executor signature mismatch", SOURCE)
        self.assertIn("kWorkerUpdateSignature[16]", SOURCE)

    def test_affinity_mode_watches_only_pose_data(self) -> None:
        self.assertIn(
            "AttachOne(worker_tid, pose, 0, 0, 0, OneWrite4Dr7())", SOURCE
        )
        self.assertIn("DR1..DR3 are disabled", SOURCE)

    def test_no_guest_patch_or_game_value_write_api(self) -> None:
        combined = SOURCE + WRAPPER
        for forbidden in (
            "mprotect(",
            "pwrite(",
            "PTRACE_POKEDATA",
            "PTRACE_SETREGS",
            "__builtin___clear_cache",
        ):
            self.assertNotIn(forbidden, combined)

    def test_superseded_guest_patch_route_fails_closed_before_device_access(self) -> None:
        lock = 'if ($Mode -ne "OfflineValidateOnly") {'
        message = "P1 live modes are frozen by the 2026-08-18 route audit"
        self.assertIn(lock, LEGACY_P1_RUNNER)
        self.assertIn(message, LEGACY_P1_RUNNER)
        self.assertLess(
            LEGACY_P1_RUNNER.index(lock),
            LEGACY_P1_RUNNER.index("Assert-LocalArtifacts"),
        )

    def test_host_only_runner_defaults_to_offline_and_requires_live_ack(self) -> None:
        self.assertIn('[string]$Mode = "OfflineValidateOnly"', AFFINITY_RUNNER)
        self.assertIn("device_access=0 attached=0", AFFINITY_RUNNER)
        self.assertIn("-not $AcknowledgeNaturallyRunningRace", AFFINITY_RUNNER)
        self.assertLess(
            AFFINITY_RUNNER.index('if ($Mode -eq "OfflineValidateOnly")'),
            AFFINITY_RUNNER.index("& $runner"),
        )


if __name__ == "__main__":
    unittest.main()
