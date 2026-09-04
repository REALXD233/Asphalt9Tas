#!/usr/bin/env python3

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-producer-thread-nativebridge-probe-v1.ps1").read_text(
    encoding="utf-8"
)
HELPER = (ROOT / "tools" / "run_producer_thread_probe_preload_v1.sh").read_text(
    encoding="utf-8"
)


class ProducerThreadRunnerPolicyTests(unittest.TestCase):
    def test_live_modes_are_permanently_frozen_before_device_or_artifact_access(self) -> None:
        freeze = RUNNER.index('if ($Mode -ne "OfflineValidateOnly")')
        artifact_validation = RUNNER.index("Assert-LocalArtifacts")
        adb_check = RUNNER.index('if (-not (Test-Path -LiteralPath $AdbPath')
        self.assertLess(freeze, artifact_validation)
        self.assertLess(freeze, adb_check)
        self.assertIn("permanently retired", RUNNER)
        self.assertIn("guest-context crash", RUNNER)

    def test_default_mode_is_true_offline_validation(self) -> None:
        self.assertIn('[string]$Mode = "OfflineValidateOnly"', RUNNER)
        offline = RUNNER.index('if ($Mode -eq "OfflineValidateOnly")')
        adb_check = RUNNER.index('if (-not (Test-Path -LiteralPath $AdbPath')
        self.assertLess(offline, adb_check)
        self.assertIn("device_access=0 restart=0 attached=0 guest_calls=0", RUNNER)

    def test_prepare_and_probe_are_separate_modes(self) -> None:
        self.assertIn('"PrepareFreshProcess", "ProbePreparedProcess"', RUNNER)
        self.assertIn("AcknowledgeFreshGameProcessRestart", RUNNER)
        self.assertIn("AcknowledgeNaturallyRunningRace", RUNNER)
        self.assertIn("AcknowledgeNoPausedAttach", RUNNER)

    def test_probe_has_exact_zero_gameplay_contract(self) -> None:
        self.assertIn("AcknowledgeOneProducerGettidOnly", RUNNER)
        self.assertIn("AcknowledgeZeroGameCallsActionsAndGameplayWrites", RUNNER)
        self.assertIn("guest_calls=1 game_calls=0 gameplay_writes=0", RUNNER)
        self.assertNotIn("input keyevent", RUNNER)
        self.assertNotIn("dispatch_action", RUNNER)

    def test_probe_requires_st2_then_exact_result(self) -> None:
        st2 = RUNNER.index("$st2Lines =")
        probe = RUNNER.index("$probeLines =")
        self.assertLess(st2, probe)
        self.assertIn("producer_hits=2", RUNNER)
        self.assertIn("call_result=success rollback=1 detached=1", RUNNER)
        self.assertIn("host_calls=0 game_calls=0 gameplay_writes=0", RUNNER)

    def test_preload_helper_targets_only_isolated_bootstrap(self) -> None:
        self.assertIn("--wait-window", HELPER)
        self.assertIn("liba9tas_bootstrap_producer_thread_probe_v1.so", HELPER)
        self.assertNotIn("producer_action_observe", HELPER)

    def test_no_pt_nb1_or_action_execution_artifact_is_referenced(self) -> None:
        self.assertNotIn("producer-action-observe", RUNNER)
        self.assertNotIn("A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE", RUNNER)
        self.assertNotIn("liba9tas_game_action_rpc_v1.so", RUNNER)


if __name__ == "__main__":
    unittest.main()
