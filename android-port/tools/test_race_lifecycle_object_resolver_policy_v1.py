#!/usr/bin/env python3
"""Build/policy audit for the read-only race-lifecycle object resolver."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "race_lifecycle_object_resolver_v1.h"
SOURCE = ROOT / "src" / "race_lifecycle_object_check_v1.cpp"
DEFAULT_BINARY = (
    ROOT / "build" / "race-lifecycle-object-v1" /
    "a9tas_race_lifecycle_object_check_v1_review_only"
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
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    combined = header + "\n" + source
    for needle in (
        "kPhaseGateRva = 0x3A5955C",
        "kRacingStoreRva = 0x3A596C4",
        "kPhaseGateSlot = 0x1D8",
        "kPhaseEnterSlot = 0x1E0",
        "kPhaseStateOffset = 0x2D8",
        "state != kCountdownState",
        "countdown_candidates == 1",
        "VerifyTargetBuild",
        "O_RDONLY | O_CLOEXEC",
        "I_ACCEPT_RACE_LIFECYCLE_READ_ONLY_V1",
        "expected_start_ticks",
        "gameplay_writes=0",
        "ptrace_calls=0",
    ):
        require(needle in combined, f"missing resolver policy {needle}")
    for forbidden in (
        "pwrite", "process_vm_writev", "PTRACE_", "ptrace(",
        "O_RDWR", "O_WRONLY", "mprotect(", "dlopen(", "dlsym(",
    ):
        require(forbidden not in combined,
                f"forbidden live primitive {forbidden}")
    require(binary.is_file(), "review-only binary missing")
    result = subprocess.run(
        [str(READELF), "-h", "-s", str(binary)],
        check=True, capture_output=True, text=True,
    ).stdout
    require("Machine:                           Advanced Micro Devices X86-64"
            in result, "review binary architecture")
    require("Type:                              DYN" in result,
            "review binary PIE type")
    for forbidden in (" pwrite", " process_vm_writev", " ptrace"):
        require(forbidden not in result, f"forbidden import {forbidden}")
    print(
        "RACE_LIFECYCLE_OBJECT_POLICY passed=1 read_only=1 "
        "unique_countdown=1 start_ticks=1 ptrace_calls=0 gameplay_writes=0 "
        "device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
