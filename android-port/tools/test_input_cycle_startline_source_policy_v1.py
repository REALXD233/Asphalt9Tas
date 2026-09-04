#!/usr/bin/env python3
"""Offline policy for the shared input-cycle-synchronized source build."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "hwbp_synchronized_tick_recorder_v1.cpp"
BUILD = ROOT / "build-input-cycle-startline-source-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    for needle in (
        "A9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1",
        "AttachNewThreadsStopped(",
        "READY_ARMED_INPUT_CYCLE_SOURCE_V1",
        "all_target_threads_frozen=1",
        "host_resume_gate=marker_removal",
        "WaitForInputCycleSourceResume",
        "FreezeInputCycleSourceThreads",
        "thread_set_stable=1",
        "freeze_passes",
        "ResumeInputCycleSourceThreads",
        "InputCycleSourceAnchorStage",
        "input_cycle_source_anchor_c98",
        "input_cycle_source_anchor_c9c",
        "input_cycle_source_anchor_complete",
        "input_cycle_anchor_skipped_deltas",
        "input_cycle_source_resume_failed",
        "waited == thread.tid && WIFEXITED(status)",
        "WEXITSTATUS(status) == 0",
        "pre_loop_retired_threads",
        "kSyncInputCycleAnchorWitnessed",
        "report.flags |= kSyncInputCycleAnchorWitnessed",
    ):
        require(needle in source, f"input-cycle source policy missing: {needle}")
    for needle in (
        '"-DA9TAS_SYNC_BRAKE_CAPTURE_V1"',
        '"-DA9TAS_INPUT_CYCLE_STARTLINE_SOURCE_V1"',
        "deployed=0 device_access=0",
    ):
        require(needle in build, f"input-cycle build policy missing: {needle}")
    require('"-DA9TAS_PAUSED_ANCHOR_WARMUP_V1"' not in build,
            "input-cycle build must not wait for the retired brake warmup")
    freeze = source.index("bool FreezeInputCycleSourceThreads(")
    resume = source.index("bool ResumeInputCycleSourceThreads(", freeze)
    require("for (std::uint32_t pass = 0; pass < 4; ++pass)" in
            source[freeze:resume],
            "input-cycle source must converge the frozen thread set")
    resume_end = source.index("#endif", resume)
    require("WIFSIGNALED" not in source[resume:resume_end],
            "signaled task must not be accepted as a clean pre-loop retirement")
    for transition in (
        "InputCycleSourceAnchorStage::kAwaitC98",
        "InputCycleSourceAnchorStage::kAwaitC9C",
        "InputCycleSourceAnchorStage::kAwaitDeltaZero",
        "InputCycleSourceAnchorStage::kReady",
    ):
        require(transition in source,
                f"input-cycle anchor transition missing: {transition}")
    require("adb" not in build.lower(), "offline build accesses a device")
    print("INPUT_CYCLE_STARTLINE_SOURCE_POLICY passed=1 preattach_stopped=1 "
          "anchor=c98-c9c-zero frame0=next-positive-delta "
          "stable_thread_set=1 clean_retirement_proof=1 "
          "anchor_report_flag=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
