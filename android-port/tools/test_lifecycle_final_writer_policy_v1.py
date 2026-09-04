#!/usr/bin/env python3
"""Offline policy for lifecycle-bound AluTasV2 final-writer replay."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
WRAPPER = ROOT / "src" / "hwbp_lifecycle_final_writer_replay_v1.cpp"
FINAL_WRITER = ROOT / "src" / "hwbp_final_writer_unified_replay_v1.cpp"
EXECUTOR = ROOT / "src" / "hwbp_unified_tick_executor_v1.cpp"
DEFAULT_OBJECT = (
    ROOT / "build" / "lifecycle-final-writer-v1" /
    "lifecycle_final_writer_v1_review_only.o"
)
READELF = (
    ROOT.parent / "toolchains" / "android-ndk-r27d" / "toolchains" /
    "llvm" / "prebuilt" / "windows-x86_64" / "bin" / "llvm-readelf.exe"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    obj = pathlib.Path(sys.argv[1]) if len(sys.argv) == 2 else DEFAULT_OBJECT
    require(len(sys.argv) <= 2, f"usage: {sys.argv[0]} [review-object]")
    wrapper = WRAPPER.read_text(encoding="utf-8")
    final_writer = FINAL_WRITER.read_text(encoding="utf-8")
    executor = EXECUTOR.read_text(encoding="utf-8")
    for needle in (
        "A9TAS_FINAL_WRITER_LIVE_CANDIDATE 1",
        "A9TAS_RACE_LIFECYCLE_START_V1 1",
        'hwbp_final_writer_unified_replay_v1.cpp',
    ):
        require(needle in wrapper, f"missing lifecycle wrapper policy {needle}")
    for needle in (
        "LIFECYCLE_OBJECT_HEX LIFECYCLE_STATE_HEX",
        "kCountdownState",
        "lifecycle_candidate.state_address != lifecycle_state",
        "g_a9tas_race_lifecycle_state_address_v1",
        "READY_ARMED_RACE_LIFECYCLE_FINAL_WRITER_V1",
        '" state_address=0x%" PRIxPTR " state=2"',
    ):
        require(needle in final_writer, f"missing lifecycle binding {needle}")
    for needle in (
        "WaitForAuthoritativeRaceStart",
        "RaceLifecycleDr7",
        "FreezeStableLifecycleThreadSet",
        "ProgramAllStoppedThreads",
        "kRacingState",
        "tick_watchpoints_armed_before_continue=1",
        "FinalWriterStartAnchorStage::kReady",
        "authoritative race-start handoff failed closed",
        "thread_abnormal_exit",
        "++*retired_threads",
        "reason=%s lifecycle_events=%",
    ):
        require(needle in executor, f"missing lifecycle handoff {needle}")
    lifecycle = executor.index("WaitForAuthoritativeRaceStart(")
    machine = executor.index("Machine machine{")
    require(lifecycle < machine, "lifecycle handoff must precede replay machine")
    require(obj.is_file(), "lifecycle final-writer review object missing")
    symbols = subprocess.run(
        [str(READELF), "-s", "--wide", str(obj)],
        check=True, capture_output=True, text=True,
    ).stdout
    for forbidden in (" process_vm_writev", " PTRACE_POKEDATA", " PTRACE_POKETEXT"):
        require(forbidden not in symbols, f"forbidden primitive {forbidden}")
    print(
        "LIFECYCLE_FINAL_WRITER_POLICY passed=1 race_edge=2_to_3 "
        "tick0=next_positive_delta same_tracer_handoff=1 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
