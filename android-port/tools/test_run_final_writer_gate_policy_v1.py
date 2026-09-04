#!/usr/bin/env python3
"""Offline safety/semantics policy for run-final-writer-gate-v1.ps1."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-final-writer-gate-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    text = RUNNER.read_text(encoding="utf-8")
    for needle in (
        '[string]$Mode = "OfflineValidate"',
        '"OfflineValidate", "PrepareFreshProcess", "ReadOnlyGate", "ExecuteGate"',
        '[ValidateSet(30, 360, 900)]',
        "$AcknowledgeFreshProcessPreload",
        "$AcknowledgeAncientRuinsZl1Countdown3Paused",
        "$AcknowledgeSingleEscapeAndNoManualInput",
        "$AcknowledgeExactFixedDeltaAndControlWrites",
        "$AcknowledgeFinalWriterConditional64Plus12",
        "$AcknowledgePtraceStallRollbackAndCrashRisk",
        "$ExecuteExactlyOneAttempt",
        "Get-StartTicks", "Assert-CleanTracer", "Assert-ArmedTracer",
        "$baseCandidates = @(foreach ($line in $maps)",
        "$bases = @($baseCandidates | Sort-Object -Unique)",
        "return [string]$bases[0]",
        "prepared.start_ticks", "payload_mapped=1",
        "-EmitOneShotLiveCandidate",
        "input keyevent 111",
        "FINAL_WRITER_FW0_PASSED",
        "A9TAS_FINAL_WRITER_READ_ONLY_GATE_V1=1",
        "FINAL_WRITER_READ_ONLY_COMPLETE",
        "READY_ARMED_FINAL_WRITER_V1",
        "all_target_threads_frozen=1",
        "Assert-ArmedTracer",
        "attached=0 gameplay_writes=0 reports=0",
        "A9UER8/A9FWR1 cross-validation failed",
        "$pins[$helper]",
        '$parser = "cd929bf2f30c3b82a886a27645d19d6068a53b1e42c9c77c9c8f96e663b71622"',
        '$pairValidator = "c073a373833451c93effba166de57df82b867a77c3342adcebf109e0fdc77193"',
        "$gateArtifactPins",
        '30 = @{ recording = "8659dceddf0edfb60d3c990ad2739f6e69e776d66eb86450754b7c6d0ba6e528"',
        '360 = @{ recording = "e7dc4bf453012627fe94e007e3db88214e5714e696c335df19b726305fe67ba6"',
        'target = "17ff6a87d5b99f78ac2bdd7afb6148bc8b013a8de1063d80624a76eca0d18b21"',
        "$process.StandardOutput.ReadToEndAsync()",
        "$process.StandardError.ReadToEndAsync()",
        "$rollbackWaitMs = if ($releaseCommitted) { 5000 } else { 25000 }",
        "forced tracer termination used; restart game process",
        '900 = @{ recording = "850e8c64fc55e291ae362cb6cf483c13807bf946de0679293419f122f33137ec"',
        "Gate artifacts differ from the reviewed Ancient Ruins + ZL1 prefix pair",
        "chmod runtime reports",
        "Remove-Item -LiteralPath $candidate -Force",
        "Final-writer candidate failed closed",
        "no retry",
    ):
        require(needle in text, f"runner policy missing: {needle}")
    require(text.count("input keyevent 111") == 1,
            "runner must contain exactly one resume input")
    require("$process.WaitForExit()" not in text,
            "runner contains an unbounded candidate wait")
    require("input tap" not in text and "keyevent 62" not in text,
            "runner contains an unauthorized gameplay input")
    require("while ($true)" not in text and "for (;;)" not in text,
            "runner contains an unbounded retry loop")
    marker_release = text.index('"remove READY marker"')
    resume_input = text.index('"single ESC"')
    require(marker_release < resume_input,
            "final-writer must release frozen target before synchronous ADB input")
    stdout_drain = text.index("$process.StandardOutput.ReadToEndAsync()")
    bounded_wait = text.index("$process.WaitForExit($TimeoutMs + 15000)")
    require(stdout_drain < bounded_wait,
            "final-writer output must drain before bounded wait")
    failure_check = text.index("Final-writer candidate failed closed")
    final_health = text.rfind("Assert-CleanTracer $gamePid", 0, failure_check)
    require(final_health >= 0 and final_health < failure_check,
            "failed live gate can bypass final process/tracer health proof")
    offline = text.index('if ($Mode -eq "OfflineValidate")')
    adb_gate = text.index('if (-not (Test-Path -LiteralPath $AdbPath')
    require(offline < adb_gate,
            "offline mode does not return before device discovery")
    print("FINAL_WRITER_GATE_RUNNER_POLICY passed=1 offline_default=1 "
          "fresh_process_bound=1 single_esc=1 retry=0 report_pair=1 "
          "one_shot_cleanup=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
