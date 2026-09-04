#!/usr/bin/env python3
"""Static fail-closed policy for the guarded M1 host runner."""

from __future__ import annotations

import pathlib
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"M1_RUNNER_POLICY_FAIL {message}")


def main() -> None:
    require(len(sys.argv) == 2, "usage")
    source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")

    for token in (
        '[string]$Mode = "OfflineValidate"',
        '"OfflineValidate", "PrepareFreshProcess", "ReadOnlyBasePreflight", "ExecuteFiveFrameGate"',
        "$AcknowledgeReadOnlyBasePreflight",
        "$AcknowledgeAncientRuinsZl1Countdown3Paused",
        "$AcknowledgeThreeAutomaticEscapeTransitions",
        "$AcknowledgeFiveFrameM1Writes",
        "$AcknowledgeNoManualInputDuringGate",
        "$AcknowledgeFailureForceStopsFreshProcess",
        "$AcknowledgePtraceStallRollbackAndCrashRisk",
        "$ExecuteExactlyOneAttempt",
        "I_ACCEPT_M1_READ_ONLY_PREFLIGHT_V1",
        "I_ACCEPT_M1_BASE_READ_ONLY_PREFLIGHT_V1",
        "I_ACCEPT_M1_EXECUTOR_REVIEW_V1",
        "I_ACCEPT_EXTERNAL_FRAME_REPLAY_LIFECYCLE_REVIEW_V1",
    ):
        require(token in source, f"missing={token}")

    offline = source.find("if ($Mode -eq 'OfflineValidate')")
    adb_gate = source.find("if (-not (Test-Path -LiteralPath $AdbPath")
    require(0 <= offline < adb_gate, "offline_return_after_adb")
    offline_body = source[offline:adb_gate]
    require("device_access=0" in offline_body and "return" in offline_body,
            "offline_device_access_contract")
    build_loop = source.find("foreach ($script in $buildScripts)")
    offline_return = source.find("M1_GATE_OFFLINE", offline)
    require(offline < build_loop < offline_return < adb_gate,
            "builds_not_confined_to_offline_mode")
    require(source.count("foreach ($script in $buildScripts)") == 1,
            "live_mode_rebuild_path")
    retirement = source.find(
        'throw "Single-address M1 live modes are retired;', offline_return)
    require(offline_return < retirement < adb_gate,
            "single_address_live_retirement_missing")

    readonly_begin = source.find("if ($Mode -eq 'ReadOnlyBasePreflight') {",
                                 offline_return)
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
        "$basePreflightText = Invoke-RemoteChecked", 0, natural_start)
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
    m1_ready = source.find("Wait-RemoteFile $m1Ready")
    tracer_check = source.find("M1 tracer identity mismatch")
    marker_release = source.find("'release M1 frozen gate'")
    report_pull = source.find("'pull M1 report'", marker_release)
    executor_failure = source.find('throw "M1 executor failed closed.',
                                   marker_release)
    require(0 <= base_preflight < natural_start < registration_resume <
            registration_settle < registration_arm < natural_ready < repause < preflight <
            executor_start < m1_ready < tracer_check < marker_release,
            "runtime_order")
    require(marker_release < report_pull < executor_failure,
            "failure_report_not_preserved_before_throw")

    for token in (
        "lifecycle_state=2 ",
        "M1_BASE_PREFLIGHT_PASSED",
        "attach_attempts=0 process_writes=0 device_mutations=0$",
        "all_target_threads_frozen=1 delta_writes=0",
        "host_resume_gate=marker_removal controller_pid=",
        "M1_ESC_DISPATCHED $gamePid $startTicks",
        "M1_EXECUTOR_DONE success=1 frames=5 ",
        "delta_writes=5 ",
        "Persistent natural callback registration failed state=",
        "Preserved failed report:",
        "--frames 5",
        "am force-stop $package",
    ):
        require(token in source, f"proof_missing={token}")

    for digest in (
        "a698ceb02bc68db892d54af84ada6a74f59f1513189376238a5b5b19cf15b763",
        "e610820bed802f19f7ae09e2f889dd826049ef50f0cb4673d6eab4590512478f",
        "636d0759cb8ccd4830c7de2b82b6a45b148f98ce4656ada53045e54df3c2f7de",
        "c2f1043cc280ed8397d65f6af80635d590434ddc88ab5078dfd78abc0fa497e2",
        "b1c913b28ff7e77c4245419ef1c540270cbff94113513ae5749e23950dfdc795",
        "3d29bebb3d6ef8f2398a905a08073f15aec72e46588139b16e2525744274ddab",
        "9e60c88c97e25902ba3de8c280b314f40dfc962ce095bec29d3965cf46672005",
        "f9c3257c6a8c3768e827408fb8f751ad4c918a9c306964e835e0e1abdee789f9",
    ):
        require(digest in source, f"pin_missing={digest}")

    require(source.count("input keyevent 111") == 3,
            "automatic_escape_count")
    print(
        "M1_RUNNER_POLICY passed=1 default_offline=1 fresh_process=1 "
        "base_preflight_before_mutation=1 live_rebuilds=0 "
        "preflight_before_executor=1 frozen_ready_gate=1 escapes=3 "
        "force_stop_on_uncertain_failure=1 live_modes_retired=1 device_access=0"
    )


if __name__ == "__main__":
    main()
