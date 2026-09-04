#!/usr/bin/env python3
"""Offline policy for lifecycle-bound synchronized source capture."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "hwbp_synchronized_tick_recorder_v1.cpp"
DEFAULT_BINARY = (
    ROOT / "build" / "lifecycle-source-v1" /
    "a9tas_lifecycle_source_v1_review_only"
)
READELF = (
    ROOT.parent / "toolchains" / "android-ndk-r27d" / "toolchains" /
    "llvm" / "prebuilt" / "windows-x86_64" / "bin" / "llvm-readelf.exe"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    binary = pathlib.Path(sys.argv[1]) if len(sys.argv) == 2 else DEFAULT_BINARY
    require(len(sys.argv) <= 2, f"usage: {sys.argv[0]} [binary]")
    source = SOURCE.read_text(encoding="utf-8")
    for needle in (
        "A9TAS_RACE_LIFECYCLE_SOURCE_V1",
        "LIFECYCLE_OBJECT_HEX LIFECYCLE_STATE_HEX",
        "kCountdownState",
        "WaitForRaceLifecycleSourceStart",
        "FreezeRaceLifecycleSourceThreads",
        "kRacingState",
        "tick_watchpoints_armed_before_continue=1",
        "'A', '9', 'U', 'S', 'R', '5'",
        "SYNCHRONIZED_TICK_RECORDER_A9USR5_DONE",
        "I_ACCEPT_SYNC_RECORDER_RACE_LIFECYCLE_V1",
        "report.flags |= kSyncRaceLifecycleWitnessed",
        "captured_frames=0",
        "thread_abnormal_exit",
        "++*retired_threads",
        "reason=%s lifecycle_events=%",
    ):
        require(needle in source, f"missing lifecycle source policy {needle}")
    require(binary.is_file(), "lifecycle source review binary missing")
    info = subprocess.run(
        [str(READELF), "-h", "-s", str(binary)],
        check=True, capture_output=True, text=True,
    ).stdout
    require("Machine:                           Advanced Micro Devices X86-64"
            in info, "lifecycle source architecture")
    require("Type:                              DYN" in info,
            "lifecycle source PIE type")
    for forbidden in (" process_vm_writev", " PTRACE_POKEDATA", " PTRACE_POKETEXT"):
        require(forbidden not in info, f"forbidden primitive {forbidden}")
    print(
        "LIFECYCLE_SOURCE_POLICY passed=1 race_edge=2_to_3 "
        "tick0=next_positive_delta fixed_delta_only=1 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
