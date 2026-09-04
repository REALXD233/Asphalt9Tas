#!/usr/bin/env python3
"""Offline policy audit for the one-shot Android 2 -> 3 HWBP observer."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "hwbp_race_lifecycle_transition_v1.cpp"
SCHEDULER = ROOT / "src" / "hwbp_scheduler_observer_v1.cpp"
DEFAULT_BINARY = (
    ROOT / "build" / "race-lifecycle-transition-v1" /
    "a9tas_race_lifecycle_transition_v1_review_only"
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
    scheduler = SCHEDULER.read_text(encoding="utf-8")
    for needle in (
        "I_ACCEPT_ONE_RACE_LIFECYCLE_2_TO_3_WATCH_V1",
        "candidate.state != a9tas::race_lifecycle_v1::kCountdownState",
        "observed_after !=",
        "kRacingState",
        "event_count != 1",
        "AttachNewThreadsStopped",
        "FreezeStableThreadSet",
        "all_target_threads_frozen=1",
        "thread_set_stable=1",
        "freeze_passes=%u",
        "RACE_LIFECYCLE_TRANSITION_PREARM_FAILURE",
        "state_read=%u armed_state=%u ready_created=%u",
        "FreezeAllExcept",
        "ClearAndDetach",
        "target_memory_write_attempts=0",
        "gameplay_writes=0",
        "O_RDONLY | O_CLOEXEC",
        "expected_start_ticks",
    ):
        require(needle in source, f"missing transition policy {needle}")
    for forbidden in (
        "pwrite", "process_vm_writev", "PTRACE_POKEDATA",
        "PTRACE_POKETEXT", "mprotect(", "dlopen(", "dlsym(",
    ):
        require(forbidden not in source,
                f"forbidden transition primitive {forbidden}")
    for needle in (
        "PTRACE_POKEUSER", "offsetof(user, u_debugreg)",
        "PTRACE_SEIZE", "PTRACE_DETACH",
    ):
        require(needle in scheduler, f"missing debug-register primitive {needle}")
    require(binary.is_file(), "transition review binary missing")
    result = subprocess.run(
        [str(READELF), "-h", "-s", str(binary)],
        check=True, capture_output=True, text=True,
    ).stdout
    require("Machine:                           Advanced Micro Devices X86-64"
            in result, "transition binary architecture")
    require("Type:                              DYN" in result,
            "transition binary PIE type")
    for forbidden in (" pwrite", " process_vm_writev"):
        require(forbidden not in result, f"forbidden import {forbidden}")
    print(
        "RACE_LIFECYCLE_TRANSITION_POLICY passed=1 one_shot=1 "
        "transition_2_to_3=1 prearm_frozen=1 gameplay_writes=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
