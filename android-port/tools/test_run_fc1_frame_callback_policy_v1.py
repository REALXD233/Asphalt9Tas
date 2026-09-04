#!/usr/bin/env python3

from __future__ import annotations

import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-fc1-frame-callback-v1.ps1").read_text(encoding="utf-8")
HELPER = (ROOT / "tools" / "run_fc1_frame_callback_preload_v1.sh").read_text(
    encoding="utf-8"
)
BOOTSTRAP = (ROOT / "src" / "bootstrap_frame_callback_v1_build.cpp").read_text(
    encoding="utf-8"
)


class Fc1RunnerPolicyTests(unittest.TestCase):
    def test_default_is_offline_and_returns_before_adb_check(self) -> None:
        self.assertIn('[string]$Mode = "OfflineValidateOnly"', RUNNER)
        offline = RUNNER.index('if ($Mode -eq "OfflineValidateOnly")')
        adb_check = RUNNER.index('if (-not (Test-Path -LiteralPath $AdbPath')
        self.assertLess(offline, adb_check)
        self.assertIn("device_access=0 restart=0 attached=0 game_writes=0", RUNNER)

    def test_all_hash_placeholders_are_closed(self) -> None:
        self.assertNotRegex(RUNNER, r"__[A-Z0-9_]+__")
        self.assertGreaterEqual(RUNNER.count('sha256'), 6)

    def test_prepare_and_probe_are_separate(self) -> None:
        self.assertIn('"PrepareFreshProcess", "ProbePreparedProcess"', RUNNER)
        self.assertIn("AcknowledgeFreshGameProcessRestart", RUNNER)
        self.assertIn("AcknowledgeNaturallyRunningRace", RUNNER)
        self.assertIn("AcknowledgeOneFramePassthroughVptrSwap", RUNNER)

    def test_old_process_stops_before_remote_payload_replacement(self) -> None:
        prepare = RUNNER.index('if ($Mode -eq "PrepareFreshProcess")')
        force_stop = RUNNER.index('"am force-stop $package"', prepare)
        first_push = RUNNER.index("Invoke-AdbChecked @('-s', $Device, 'push'", prepare)
        self.assertLess(force_stop, first_push)

    def test_stale_preload_waiters_are_rejected(self) -> None:
        self.assertIn("function Assert-NoStaleFc1Waiter", RUNNER)
        self.assertIn("Stale FC-1 preload waiter detected", RUNNER)
        prepare = RUNNER.index('if ($Mode -eq "PrepareFreshProcess")')
        first_audit = RUNNER.index("Assert-NoStaleFc1Waiter", prepare)
        first_push = RUNNER.index("Invoke-AdbChecked @('-s', $Device, 'push'", prepare)
        self.assertLess(first_audit, first_push)
        preload_check = RUNNER.index("FC-1 preload bootstrap did not reach armed stage")
        second_audit = RUNNER.index("Assert-NoStaleFc1Waiter", preload_check)
        self.assertLess(preload_check, second_audit)

    def test_acknowledgements_precede_live_commands(self) -> None:
        prepare = RUNNER.index('if ($Mode -eq "PrepareFreshProcess")')
        prepare_gate = RUNNER.index("AcknowledgeFreshGameProcessRestart", prepare)
        first_push = RUNNER.index("Invoke-AdbChecked @('-s', $Device, 'push'", prepare)
        self.assertLess(prepare_gate, first_push)
        probe_gate_block = RUNNER.index(
            '@($AcknowledgeNaturallyRunningRace, "a naturally running race")',
            first_push,
        )
        probe_pid = RUNNER.index("$gamePid = Get-GamePid", probe_gate_block)
        self.assertLess(probe_gate_block, probe_pid)

    def test_fresh_process_receipt_pins_start_time(self) -> None:
        self.assertIn("Get-ProcessStartTime", RUNNER)
        self.assertIn("fc1-prepared-process-v1.json", RUNNER)
        self.assertIn("start_time", RUNNER)
        self.assertIn("Prepared process receipt mismatch", RUNNER)

    def test_every_device_artifact_is_rehashed(self) -> None:
        self.assertIn("Assert-RemoteHash", RUNNER)
        self.assertIn("remotePayload", RUNNER)
        self.assertIn("remoteBootstrap", RUNNER)
        self.assertIn("remoteController", RUNNER)
        self.assertIn("remoteInjector", RUNNER)
        self.assertIn("remoteHelper", RUNNER)

    def test_probe_is_observe_only_and_exactly_acknowledged(self) -> None:
        self.assertIn("I_ACCEPT_FC1_ONE_FRAME_PASSTHROUGH_NO_ACTION_V1", RUNNER)
        self.assertIn("AcknowledgeNoActionInputNitroOrPhysicsCorrection", RUNNER)
        self.assertIn("FC1_DONE success=1", RUNNER)
        self.assertNotIn("input keyevent", RUNNER)
        self.assertNotIn("dispatch_action", RUNNER)

    def test_report_is_stale_safe_and_strictly_validated(self) -> None:
        self.assertIn("Remote report already exists", RUNNER)
        self.assertIn("Local report already exists", RUNNER)
        self.assertIn("validate_fc1_report_v1.py", RUNNER)
        self.assertIn("FC1_REPORT_VALID passed=1", RUNNER)

    def test_probe_requires_delayed_survival_observation(self) -> None:
        self.assertIn("PostProbeObservationMs = 3000", RUNNER)
        delay = RUNNER.index("Start-Sleep -Milliseconds $PostProbeObservationMs")
        delayed_check = RUNNER.index(
            "Game process changed during delayed FC-1 survival observation", delay
        )
        passed = RUNNER.index('Write-Output "FC1_PROBE_PASSED', delayed_check)
        self.assertLess(delay, delayed_check)
        self.assertLess(delayed_check, passed)

    def test_bootstrap_only_loads_the_fc0_payload(self) -> None:
        self.assertIn("liba9tas_frame_callback_bootstrap_v1_build_only.so", BOOTSTRAP)
        self.assertNotIn("A9TAS_ENABLE_SAME_THREAD_PROBE", BOOTSTRAP)
        self.assertNotIn("producer_thread", BOOTSTRAP)
        self.assertIn("liba9tas_bootstrap_frame_callback_v1.so", HELPER)
        self.assertNotIn("producer_thread", HELPER)

    def test_no_retired_transport_or_action_artifact(self) -> None:
        for forbidden in (
            "producer-thread", "same-thread", "game-action", "NativeBridge_gettid",
            "liba9tas_game_action_rpc", "A9TAS_GAME_ACTION_RPC_ENABLE_EXECUTE",
        ):
            self.assertNotIn(forbidden, RUNNER)


if __name__ == "__main__":
    unittest.main()
