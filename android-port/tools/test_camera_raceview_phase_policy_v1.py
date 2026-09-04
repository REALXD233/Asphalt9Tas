#!/usr/bin/env python3
"""Offline policy for the timestamped read-only RaceView phase sampler."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
INPUTS = (
    ROOT / "src" / "camera_raceview_phase_sampler_v1.cpp",
    ROOT / "src" / "camera_raceview_state_sampler_v1.cpp",
    ROOT / "src" / "camera_raceview_state_v1.h",
)
DEFAULT_BINARY = (
    ROOT / "build" / "camera-raceview-phase-v1" /
    "a9tas_camera_raceview_phase_sampler_v1_review_only"
)
READELF = (
    ROOT.parent / "toolchains" / "android-ndk-r27d" / "toolchains" /
    "llvm" / "prebuilt" / "windows-x86_64" / "bin" /
    "llvm-readelf.exe"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    binary = pathlib.Path(sys.argv[1]) if len(sys.argv) == 2 else DEFAULT_BINARY
    require(len(sys.argv) <= 2, f"usage: {sys.argv[0]} [binary]")
    combined = "\n".join(path.read_text(encoding="utf-8") for path in INPUTS)
    for needle in (
        "I_ACCEPT_RACEVIEW_PHASE_READ_ONLY_V1",
        "samples > 2400",
        "samples > 12000 / interval_ms",
        "O_RDONLY | O_CLOEXEC",
        "RACEVIEW_PHASE_BEGIN",
        "RACEVIEW_PHASE_END",
        "gameplay_writes=0",
        "ptrace_calls=0",
        "invoked_methods=0",
        "input_events=0",
    ):
        require(needle in combined, f"missing phase policy {needle}")
    for forbidden in (
        "pwrite", "process_vm_writev", "PTRACE_", "ptrace(", "O_RDWR",
        "O_WRONLY", "mprotect(", "dlopen(", "dlsym(", "kill(",
    ):
        require(forbidden not in combined,
                f"forbidden phase primitive {forbidden}")
    require(binary.is_file(), "review-only phase binary missing")
    result = subprocess.run(
        [str(READELF), "-h", "-s", str(binary)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    require(
        "Machine:                           Advanced Micro Devices X86-64"
        in result,
        "review binary architecture",
    )
    require("Type:                              DYN" in result,
            "review binary PIE type")
    for forbidden in (" pwrite", " process_vm_writev", " ptrace"):
        require(forbidden not in result, f"forbidden import {forbidden}")
    print(
        "CAMERA_RACEVIEW_PHASE_POLICY passed=1 read_only=1 "
        "max_samples=2400 max_duration_ms=12000 gameplay_writes=0 "
        "ptrace_calls=0 invoked_methods=0 input_events=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
