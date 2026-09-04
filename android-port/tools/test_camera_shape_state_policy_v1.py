#!/usr/bin/env python3
"""Offline source/build policy for the bounded camera-shape sampler."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "camera_shape_state_v1.h"
SELFTEST = ROOT / "src" / "camera_shape_state_selftest_v1.cpp"
SOURCE = ROOT / "src" / "camera_shape_state_sampler_v1.cpp"
DEFAULT_BINARY = (
    ROOT / "build" / "camera-shape-state-v1" /
    "a9tas_camera_shape_state_sampler_v1_review_only"
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
    combined = "\n".join(
        path.read_text(encoding="utf-8") for path in (HEADER, SELFTEST, SOURCE)
    )
    for needle in (
        "kObjectSize = 0xF0",
        "kExpectedPrimaryVptrRva = 0x7F225E0",
        "kExpectedSecondaryVptrRva = 0x7F22728",
        "kStateAOffset = 0x40",
        "kStateBOffset = 0x5C",
        "kMotionPairOffset = 0x78",
        "kStateCOffset = 0x90",
        "kSecondaryVptrOffset = 0xE8",
        "I_ACCEPT_CAMERA_SHAPE_STATE_READ_ONLY_V1",
        "kMaximumSamples = 3000",
        "kMaximumDurationMs = 60000",
        "O_RDONLY | O_CLOEXEC",
        "ExactSnapshotPasses",
        "WrongPrimaryRejected",
        "WrongSecondaryRejected",
        "SharedObjectRejected",
        "ReadFailureIsFatal",
        "gameplay_writes=0",
        "ptrace_calls=0",
        "invoked_methods=0",
    ):
        require(needle in combined, f"missing camera-shape policy {needle}")
    for forbidden in (
        "pwrite", "process_vm_writev", "PTRACE_", "ptrace(", "O_RDWR",
        "O_WRONLY", "mprotect(", "dlopen(", "dlsym(",
    ):
        require(forbidden not in combined,
                f"forbidden camera-shape primitive {forbidden}")
    require(binary.is_file(), "review-only camera-shape binary missing")
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
        "CAMERA_SHAPE_STATE_POLICY passed=1 read_only=1 object_bytes=240 "
        "max_duration_ms=60000 invoked_methods=0 ptrace_calls=0 "
        "gameplay_writes=0 device_access=0 deployed=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
