#!/usr/bin/env python3
"""Static fail-closed policy for the independent A9PHEX1 host runner."""

from __future__ import annotations

import pathlib
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"PHASE_PACED_RUNNER_POLICY_FAIL {message}")


def main() -> None:
    require(len(sys.argv) == 2, "usage")
    source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")

    for token in (
        '[string]$Mode = "OfflineValidate"',
        '"OfflineValidate", "PrepareFreshProcess", "ReadOnlyBasePreflight", "ExecuteFiveFrameGate"',
        "$AcknowledgeReadOnlyBasePreflight",
        "$AcknowledgeAncientRuinsZl1Countdown3Paused",
        "$AcknowledgeThreeAutomaticEscapeTransitions",
        "$AcknowledgeFiveFramePhasePacedWrites",
        "$AcknowledgeNoManualInputDuringGate",
        "$AcknowledgeFailureForceStopsFreshProcess",
        "$AcknowledgePtraceStallRollbackAndCrashRisk",
        "$ExecuteExactlyOneAttempt",
        "I_ACCEPT_PHASE_PACED_EXECUTOR_REVIEW_V1",
        "I_ACCEPT_EXTERNAL_FRAME_REPLAY_LIFECYCLE_REVIEW_V1",
        "parse_phase_paced_executor_report_v1.py",
        "verify_phase_paced_five_frame_candidate_v1.py",
        "Invoke-RemoteReadOnlyWithSnapshotRetry",
        "stage=gameplay_input_controller detail=result_-4 ",
        "$attempt -le 3",
    ):
        require(token in source, f"missing={token}")

    offline = source.find("if ($Mode -eq 'OfflineValidate')")
    adb_gate = source.find("if (-not (Test-Path -LiteralPath $AdbPath")
    require(0 <= offline < adb_gate, "offline_return_after_adb")
    offline_body = source[offline:adb_gate]
    require("PHASE_PACED_GATE_OFFLINE" in offline_body and
            "device_access=0" in offline_body and "return" in offline_body,
            "offline_device_access_contract")
    require("$candidateVerifier" in offline_body,
            "candidate_manifest_not_checked_offline")
    require(source.count("foreach ($script in $buildScripts)") == 1,
            "live_mode_rebuild_path")
    retirement = source.find(
        'throw "A9PHEX1 live modes are retired after the five-frame '
        'receipt-order failure"', offline)
    require(offline < retirement < adb_gate,
            "failed_live_path_not_retired")

    readonly_begin = source.find("if ($Mode -eq 'ReadOnlyBasePreflight') {",
                                 adb_gate)
    readonly_end = source.find("\n$mutationStarted = $false", readonly_begin)
    require(readonly_begin >= 0 and readonly_end > readonly_begin,
            "read_only_mode_bounds")
    readonly = source[readonly_begin:readonly_end]
    for forbidden in (
        "input keyevent 111",
        "$actionHandle = Start-Remote",
        "$executorHandle = Start-Remote",
        "am force-stop $package",
        "$remoteActionController",
        "$remoteExecutor",
    ):
        require(forbidden not in readonly,
                f"read_only_mode_mutation={forbidden}")
    require("process_writes=0 attach_attempts=0 automatic_esc=0" in readonly,
            "read_only_mode_proof_missing")

    natural_start = source.find("$actionHandle = Start-Remote")
    base_preflight = source.rfind(
        "$basePreflightText = Invoke-RemoteReadOnlyWithSnapshotRetry",
        0, natural_start)
    registration_resume = source.find("'resume natural callback registration'")
    registration_settle = source.find("Start-Sleep -Milliseconds 300",
                                      registration_resume)
    registration_arm = source.find("'arm natural callback after ESC settle'",
                                   registration_settle)
    natural_ready = source.find("$registrationState = 'ready'",
                                registration_arm)
    repause = source.find("'re-pause after callback registration'")
    preflight = source.find("$preflightText = Invoke-RemoteChecked")
    executor_start = source.find("$executorHandle = Start-Remote")
    phase_ready = source.find("Wait-RemoteFile $phaseReady")
    tracer_check = source.find("A9PHEX1 tracer identity mismatch")
    marker_release = source.find("'release A9PHEX1 frozen gate'")
    report_pull = source.find("'pull A9PHEX1 report'", marker_release)
    executor_failure = source.find('throw "A9PHEX1 executor failed closed.',
                                   marker_release)
    require(0 <= base_preflight < natural_start < registration_resume <
            registration_settle < registration_arm < natural_ready < repause <
            preflight < executor_start < phase_ready < tracer_check <
            marker_release, "runtime_order")
    require(marker_release < report_pull < executor_failure,
            "failure_report_not_preserved_before_throw")

    for token in (
        "PHASE_PACED_READY_ARMED ",
        "delta=0x[0-9a-f]+ c98=0x[0-9a-f]+ c9c=0x[0-9a-f]+ world=0x[0-9a-f]+",
        "all_target_threads_frozen=1 delta_writes=0",
        "host_resume_gate=marker_removal controller_pid=",
        "PHASE_ESC_DISPATCHED $gamePid $startTicks",
        "PHASE_PACED_EXECUTOR_DONE success=1 frames=5 ",
        "PHASE_PACED_GATE_PASSED",
        "delta_writes=5 ",
        "Preserved failed report:",
        "--frames 5",
        "am force-stop $package",
    ):
        require(token in source, f"proof_missing={token}")

    for digest in (
        "c60df0f52dc49abb1e2d413518ad5ddd4c6431c156434be121047a8e12ce373e",
        "3d29bebb3d6ef8f2398a905a08073f15aec72e46588139b16e2525744274ddab",
        "9e60c88c97e25902ba3de8c280b314f40dfc962ce095bec29d3965cf46672005",
        "78c0ec1f2a88c4d551f0a6e9e49d2aea21cec8c6d036407fe3ba0a933e306e17",
    ):
        require(digest in source, f"pin_missing={digest}")

    require(source.count("input keyevent 111") == 3,
            "automatic_escape_count")
    require("a9tas_m1_ready_" not in source,
            "old_single_address_ready_namespace")
    require("parse_m1_executor_report_v1.py" not in source,
            "old_report_parser_reused")
    print(
        "PHASE_PACED_RUNNER_POLICY passed=1 default_offline=1 "
        "fresh_process=1 read_only_preflight=1 live_rebuilds=0 "
        "four_address_ready=1 frozen_ready_gate=1 escapes=3 "
        "failure_report_preserved=1 force_stop_on_uncertain_failure=1 "
        "live_modes_retired=1 device_access=0"
    )


if __name__ == "__main__":
    main()
