#!/usr/bin/env python3
"""Static safety policy for the guarded lifecycle live runner."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-natural-action-lifecycle-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    text = RUNNER.read_text(encoding="utf-8")
    require("$mutationRisk = $false" in text, "risk phase must start false")
    esc = text.index("input keyevent 111 && echo NAL_ARM")
    risk = text.index("$mutationRisk = $true")
    require(risk < esc, "risk must begin immediately before the composite ESC/ARM command")
    composite = text[esc:esc + 180]
    require(composite.index("input keyevent 111") < composite.index("echo NAL_ARM") and
            "> $armPath" in composite,
            "composite command must resume before publishing the ARM marker")
    require("$readyPath $armPath $ack" in text,
            "candidate command must receive the unique ARM path")
    require("rm -f $armPath" in text,
            "ARM marker must be cleaned in finally")
    require("$null -ne $startInfo.ArgumentList" in text and
            "$startInfo.Arguments =" in text and
            "Windows PowerShell 5.1" in text,
            "candidate launch must support both modern and Windows PowerShell")
    require("-not $passed -and $mutationRisk" in text,
            "force-stop must require mutation risk")
    require("Read-only preflight failed before ESC" in text,
            "pre-ESC failure must preserve the prepared process")
    require("Assert-StableCleanTracer $gamePid" in text and
            "$consecutive -ge 8" in text and
            "AddMilliseconds(2000)" in text,
            "execution preflight requires bounded stable tracer-clear proof")
    require("if ($confirmed -and -not $passed) { Force-StopGame" not in text,
            "blanket failure force-stop is forbidden")
    for token in (
        "paused_zero_samples=8 target_threads_attached=0 game_writes=0",
        "sending_single_ESC_then_ARM=1",
        "AcknowledgeExactlyOneEscapeResume",
        "AcknowledgeFailureForceStopsFreshProcess",
        "NAL_DONE success=1",
        "validate_natural_action_lifecycle_report_v1.py",
        "Game process changed after bootstrap proof",
        "Mapped process does not match bootstrap proof",
    ):
        require(token in text, f"runner gate missing: {token}")
    print("NATURAL_ACTION_LIFECYCLE_RUNNER_POLICY passed=1 "
          "pre_esc_preserve=1 post_esc_force_stop=1 blanket_stop=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
