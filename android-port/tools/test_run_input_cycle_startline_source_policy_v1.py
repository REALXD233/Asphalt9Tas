#!/usr/bin/env python3
"""Offline policy for the guarded input-cycle source runner."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-input-cycle-startline-source-v1.ps1"


def main() -> int:
    text = RUNNER.read_text(encoding="utf-8")
    required = (
        '[string]$Mode = "OfflineValidate"',
        '[ValidateSet(360)]',
        "$ExecuteExactlyOneAttempt",
        "$AcknowledgeAncientRuinsZl1Countdown3Paused",
        "$AcknowledgePreAttachFrozenAndSingleEscape",
        "$Acknowledge360FixedDeltaWrites",
        "$AcknowledgeReadOnlyControlAndPhysicsCapture",
        "$AcknowledgeNoManualInput",
        "$AcknowledgePtraceStallRollbackAndCrashRisk",
        "READY_ARMED_INPUT_CYCLE_SOURCE_V1",
        "all_target_threads_frozen=1",
        "thread_set_stable=1 freeze_passes=[2-4]",
        "$identityMatch = [regex]::Match(",
        "$controllerPid = [int]$identityMatch.Groups[1].Value",
        "$candidate.StandardOutput.ReadToEndAsync()",
        "$candidate.StandardError.ReadToEndAsync()",
        "$rollbackWaitMs = if ($releaseCommitted) { 5000 } else { 25000 }",
        "$forcedTracerTermination = $true",
        "forced tracer termination used; restart game process",
        "cleanup TracerPid=",
        "input keyevent 111",
        "Armed tracer identity mismatch",
        "no retry",
        "synchronized_brake_recording_v1.py",
        "--require-input-cycle-anchor",
        "INPUT_CYCLE_STARTLINE_SOURCE_LIVE_PASSED",
    )
    for needle in required:
        if needle not in text:
            raise AssertionError(f"input-cycle runner policy missing: {needle}")
    if text.count("input keyevent 111") != 1:
        raise AssertionError("runner must contain exactly one resume input")
    if "$controllerPid = [int]$matches[1]" in text:
        raise AssertionError("controller identity must not depend on mutable PowerShell $matches")
    if "$candidate.WaitForExit()" in text:
        raise AssertionError("runner contains an unbounded candidate wait")
    stdout_drain = text.index("$candidate.StandardOutput.ReadToEndAsync()")
    bounded_wait = text.index("$candidate.WaitForExit($TimeoutMs + 15000)")
    if stdout_drain >= bounded_wait:
        raise AssertionError("candidate output must drain before bounded wait")
    marker_release = text.index('"remove source READY marker"')
    resume_input = text.index('"single ESC"')
    if marker_release >= resume_input:
        raise AssertionError(
            "frozen target must be released before synchronous ADB input"
        )
    if "input tap" in text or "keyevent 62" in text or "while ($true)" in text:
        raise AssertionError("runner contains unauthorized input/retry logic")
    offline = text.index('if ($Mode -eq "OfflineValidate")')
    adb = text.index('if (-not (Test-Path -LiteralPath $AdbPath')
    if offline >= adb:
        raise AssertionError("offline mode reaches device discovery")
    print("INPUT_CYCLE_STARTLINE_SOURCE_RUNNER_POLICY passed=1 offline_default=1 "
          "single_esc=1 retries=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
