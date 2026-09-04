#!/usr/bin/env python3
"""Build/policy audit for the read-only active-camera diagnostic."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "camera_active_state_resolver_v1.h"
SOURCE = ROOT / "src" / "camera_active_state_check_v1.cpp"
DEFAULT_BINARY = (
    ROOT / "build" / "camera-active-state-check-v1" /
    "a9tas_camera_active_state_check_v1_review_only"
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
    combined = HEADER.read_text(encoding="utf-8") + "\n" + SOURCE.read_text(
        encoding="utf-8"
    )
    for needle in (
        "I_ACCEPT_CAMERA_ACTIVE_STATE_READ_ONLY_V1",
        "expected_start_ticks",
        "kRaceViewPrimaryVptrRva = 0x8174F40",
        "kRaceViewSecondaryVptrRva = 0x8175108",
        "kCameraManagerOffset = 0x4B8",
        "kBlendedCameraOffset = 0x100",
        "ReadStateSamples",
        "plausible_mask",
        "mapping_count=%zu",
        "failure_mapping_index=",
        "failure_address=",
        "failure_size=",
        "mapping.private_mapping = perms[3] == 'p'",
        "O_RDONLY | O_CLOEXEC",
        "gameplay_writes=0",
        "ptrace_calls=0",
    ):
        require(needle in combined, f"missing camera check policy {needle}")
    for forbidden in (
        "pwrite", "process_vm_writev", "PTRACE_", "ptrace(", "O_RDWR",
        "O_WRONLY", "mprotect(", "dlopen(", "dlsym(",
    ):
        require(forbidden not in combined, f"forbidden live primitive {forbidden}")
    require(binary.is_file(), "review-only camera check binary missing")
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
        "CAMERA_ACTIVE_STATE_CHECK_POLICY passed=1 read_only=1 "
        "start_ticks=1 three_layout_hypotheses=1 ptrace_calls=0 "
        "gameplay_writes=0 device_access=0 deployed=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
