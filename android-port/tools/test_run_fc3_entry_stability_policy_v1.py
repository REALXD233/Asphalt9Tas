#!/usr/bin/env python3
"""Offline policy checks for the FC3 entry-only guarded runner."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-fc3-entry-stability-v1.ps1").read_text(encoding="utf-8")
CONTROLLER = (ROOT / "src" / "fc2_frame_callback_transaction_controller_v1.cpp").read_text(encoding="utf-8")


class EntryRunnerPolicyTests(unittest.TestCase):
    def test_offline_default_returns_before_adb(self) -> None:
        self.assertIn('[string]$Mode = "OfflineValidateOnly"', RUNNER)
        offline = RUNNER.index('if ($Mode -eq "OfflineValidateOnly")')
        adb = RUNNER.index('if (-not (Test-Path -LiteralPath $AdbPath', offline)
        self.assertLess(offline, adb)
        self.assertIn("device_access=0", RUNNER[offline:adb])

    def test_prepare_has_no_preload_or_injection_transport(self) -> None:
        lower = RUNNER.lower()
        for forbidden in ("remoteinjector", "remotehelper", "remotepayload",
                          "preload-window", "bootstrap_verified"):
            self.assertNotIn(forbidden, lower)
        self.assertIn("FC3_ENTRY_PREPARED", RUNNER)
        self.assertIn("preload=0", RUNNER)

    def test_probe_acknowledges_exact_reduced_scope(self) -> None:
        for token in (
            "$AcknowledgeAttachReadDetachOnly",
            "$AcknowledgeNoHardwareWatchpointsOrDebugWrites",
            "$AcknowledgeNoPayloadVptrOrGameWrites",
            "$AcknowledgePtraceStalls",
            "$AcknowledgeExitKillMayTerminateFreshProcess",
            "$AcknowledgeFailureForceStopsFreshProcess",
            "I_ACCEPT_FC3_ENTRY_STABILITY_ATTACH_READ_DETACH_V1",
        ):
            self.assertIn(token, RUNNER)

    def test_receipt_and_artifacts_are_hash_bound(self) -> None:
        for token in ("boot_id", "start_time", "nonce", "created_unix",
                      "$controllerHash", "$validatorHash", "Get-RemoteHash"):
            self.assertIn(token, RUNNER)

    def test_runner_does_not_shadow_powershell_pid_automatic_variable(self) -> None:
        self.assertNotIn("$pid", RUNNER.lower())
        self.assertIn("$gamePid", RUNNER)

    def test_failed_report_is_preserved_before_force_stop(self) -> None:
        pull = RUNNER.index("'pull', $remoteReport")
        preserved = RUNNER.index("FC3_ENTRY_FAILED_REPORT_PRESERVED", pull)
        failure = RUNNER.index("Entry controller failed closed", preserved)
        cleanup = RUNNER.index("Stop-FreshPackage", failure)
        self.assertLess(pull, preserved)
        self.assertLess(preserved, failure)
        self.assertLess(failure, cleanup)

    def test_controller_entry_path_returns_before_memory_open(self) -> None:
        dispatch = CONTROLLER.index("if (entry_stability_only)")
        call = CONTROLLER.index("return RunFc3EntryStability", dispatch)
        mem_open = CONTROLLER.index("open(mem_path, O_RDONLY", call)
        self.assertLess(call, mem_open)
        for token in ("AttachStableInitialThreadSet(pid, 0, false, &threads)",
                      "DetachStoppedWithoutDebugWrites",
                      "if (arm_flags) (void)RestoreDebug",
                      "debug_write_attempts == 0",
                      "game_write_attempts == 0"):
            self.assertIn(token, CONTROLLER)


if __name__ == "__main__":
    unittest.main()
