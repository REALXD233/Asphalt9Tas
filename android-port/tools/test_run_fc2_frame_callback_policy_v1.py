#!/usr/bin/env python3
"""Offline unit policy for the FC-2 guarded runner and preload pair."""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = (ROOT / "run-fc2-frame-callback-v1.ps1").read_text(encoding="utf-8")
HELPER = (ROOT / "tools" / "run_fc2_frame_callback_preload_v1.sh").read_text(
    encoding="utf-8"
)
BOOTSTRAP = (ROOT / "src" / "bootstrap_frame_callback_fc2_v1_build.cpp").read_text(
    encoding="utf-8"
)


class Fc2RunnerPolicyTests(unittest.TestCase):
    def test_default_returns_before_any_adb_path_check(self) -> None:
        self.assertIn('[string]$Mode = "OfflineValidateOnly"', RUNNER)
        offline = RUNNER.index('if ($Mode -eq "OfflineValidateOnly")')
        adb_check = RUNNER.index('if (-not (Test-Path -LiteralPath $AdbPath')
        self.assertLess(offline, adb_check)
        self.assertIn(
            "device_access=0 restart=0 attached=0 game_writes=0 registration=0 removal=0",
            RUNNER,
        )

    def test_runner_build_cannot_skip_missing_policy(self) -> None:
        build = (ROOT / "build-fc2-runner-v1.ps1").read_text(encoding="utf-8")
        self.assertIn('throw "FC-2 runner policy is missing"', build)
        self.assertNotIn("if (Test-Path -LiteralPath $runnerPolicy) {", build)

    def test_hash_pins_are_closed_and_stage_specific(self) -> None:
        self.assertNotRegex(RUNNER, r"__[A-Z0-9_]+__")
        hashes = re.findall(r'"([0-9a-f]{64})"', RUNNER)
        self.assertGreaterEqual(len(set(hashes)), 6)
        self.assertIn("frame-callback-deferred-registration-v1", RUNNER)
        self.assertIn("fc2-runner-v1", RUNNER)

    def test_prepare_and_probe_have_separate_acknowledgements(self) -> None:
        self.assertIn('"PrepareFreshProcess", "ProbePreparedProcess"', RUNNER)
        for token in (
            "AcknowledgeFreshGameProcessRestart",
            "AcknowledgePreloadWindowInjection",
            "AcknowledgePassiveFc2PayloadOnly",
            "AcknowledgeNaturallyRunningRace",
            "AcknowledgeNoPausedAttach",
            "AcknowledgeThreeFrameDeferredRegistration",
            "AcknowledgeOneVptrSwapAndGameOwnedAddRemove",
            "AcknowledgeProbeFailureForceStopsFreshProcess",
        ):
            self.assertIn(token, RUNNER)

    def test_old_process_is_stopped_before_any_push(self) -> None:
        prepare = RUNNER.index('if ($Mode -eq "PrepareFreshProcess")')
        stop = RUNNER.index('"am force-stop $package"', prepare)
        push = RUNNER.index("Invoke-AdbChecked @('-s', $Device, 'push'", prepare)
        self.assertLess(stop, push)

    def test_stale_waiter_audits_surround_preload(self) -> None:
        self.assertIn("function Assert-NoStaleFc2Waiter", RUNNER)
        self.assertIn("Stale FC-2 preload waiter detected", RUNNER)
        prepare = RUNNER.index('if ($Mode -eq "PrepareFreshProcess")')
        first = RUNNER.index("Assert-NoStaleFc2Waiter", prepare)
        preload_result = RUNNER.index("FC-2 preload bootstrap did not reach armed stage")
        second = RUNNER.index("Assert-NoStaleFc2Waiter", preload_result)
        self.assertLess(first, preload_result)
        self.assertLess(preload_result, second)

    def test_receipt_pins_pid_start_time_and_all_preload_inputs(self) -> None:
        self.assertIn("fc2-prepared-process-v1.json", RUNNER)
        for field in (
            "start_time", "payload_sha256", "bootstrap_sha256",
            "controller_sha256", "injector_sha256", "helper_sha256",
        ):
            self.assertIn(field, RUNNER)
        self.assertIn("Prepared process receipt mismatch", RUNNER)
        mismatch = RUNNER.index('throw "Prepared process receipt mismatch"')
        confirmed = RUNNER.index("$confirmedFresh = $true", mismatch)
        self.assertLess(mismatch, confirmed)

    def test_every_remote_execution_artifact_is_rehashed(self) -> None:
        self.assertIn("function Assert-RemoteHash", RUNNER)
        for token in (
            "remotePayload", "remoteBootstrap", "remoteController",
            "remoteInjector", "remoteHelper",
        ):
            self.assertIn(token, RUNNER)

    def test_probe_failure_force_stops_only_confirmed_fresh_process(self) -> None:
        self.assertIn("function Stop-ConfirmedFreshProcess", RUNNER)
        final = RUNNER.index("} finally {")
        condition = RUNNER.index("$confirmedFresh -and -not $probePassed", final)
        stop = RUNNER.index("Stop-ConfirmedFreshProcess $gamePid $startTime", condition)
        receipt_delete = RUNNER.index("Remove-Item -LiteralPath $receipt -Force", stop)
        self.assertLess(final, condition)
        self.assertLess(condition, stop)
        self.assertLess(stop, receipt_delete)
        self.assertIn("Failed FC-2 package remained alive after force-stop", RUNNER)
        function_start = RUNNER.index("function Stop-ConfirmedFreshProcess")
        function = RUNNER[function_start:RUNNER.index(
            "\nAssert-LocalArtifacts", function_start
        )]
        self.assertNotIn("if ($current -gt 0)", function)
        self.assertIn("Always put the package in the stopped state", function)

    def test_failed_preparation_force_stops_and_invalidates_receipt(self) -> None:
        prepare = RUNNER.index('if ($Mode -eq "PrepareFreshProcess")')
        guard = RUNNER.index("$preparePassed = $false", prepare)
        final = RUNNER.index("} finally {", guard)
        condition = RUNNER.index("if (-not $preparePassed)", final)
        stop = RUNNER.index('shell "am force-stop $package"', condition)
        receipt_delete = RUNNER.index(
            "Remove-Item -LiteralPath $receipt -Force", stop
        )
        self.assertLess(guard, final)
        self.assertLess(final, condition)
        self.assertLess(condition, stop)
        self.assertLess(stop, receipt_delete)

    def test_success_deletes_receipt_before_commit(self) -> None:
        delayed = RUNNER.index(
            "Game process changed during delayed FC-2 survival observation"
        )
        receipt_delete = RUNNER.index(
            "Remove-Item -LiteralPath $receipt -Force", delayed
        )
        committed = RUNNER.index("$probePassed = $true", receipt_delete)
        self.assertLess(receipt_delete, committed)

    def test_probe_requires_exact_controller_and_report_success(self) -> None:
        self.assertIn(
            "I_ACCEPT_FC2_THREE_FRAME_DEFERRED_REGISTRATION_OBSERVE_ONLY_V1",
            RUNNER,
        )
        self.assertIn("FC2_DONE success=1", RUNNER)
        self.assertIn("validate_fc2_report_v1.py", RUNNER)
        self.assertIn("FC2_REPORT_VALID passed=1", RUNNER)
        self.assertIn("membership=0,1,0", RUNNER)

    def test_report_paths_are_stale_safe(self) -> None:
        self.assertIn("Local report already exists", RUNNER)
        self.assertIn("Remote report already exists", RUNNER)
        report_check = RUNNER.index("Remote report already exists")
        controller = RUNNER.index("$remoteController $gamePid", report_check)
        self.assertLess(report_check, controller)

    def test_delayed_survival_precedes_success(self) -> None:
        delay = RUNNER.index("Start-Sleep -Milliseconds $PostProbeObservationMs")
        changed = RUNNER.index(
            "Game process changed during delayed FC-2 survival observation", delay
        )
        passed = RUNNER.index('Write-Output "FC2_PROBE_PASSED', changed)
        self.assertLess(delay, changed)
        self.assertLess(changed, passed)

    def test_payload_and_preload_are_fc2_only(self) -> None:
        self.assertIn(
            "liba9tas_frame_callback_deferred_registration_v1_build_only.so",
            BOOTSTRAP,
        )
        self.assertIn("liba9tas_bootstrap_frame_callback_fc2_v1.so", HELPER)
        self.assertIn("a9tas_injector_fc2_v1", HELPER)
        self.assertNotIn("frame_callback_bootstrap_v1_build_only", BOOTSTRAP)

    def test_no_action_or_retired_transport(self) -> None:
        for forbidden in (
            "input keyevent", "dispatch_action", "producer-thread",
            "same-thread", "game-action", "NativeBridge_gettid",
            "liba9tas_game_action_rpc",
        ):
            self.assertNotIn(forbidden, RUNNER)


if __name__ == "__main__":
    unittest.main()
