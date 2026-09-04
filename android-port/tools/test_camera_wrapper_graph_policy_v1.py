#!/usr/bin/env python3
"""Offline build/source policy for the bounded camera-wrapper graph."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "camera_wrapper_graph_v1.h"
SELFTEST = ROOT / "src" / "camera_wrapper_graph_selftest_v1.cpp"
SOURCE = ROOT / "src" / "camera_wrapper_graph_check_v1.cpp"
DEFAULT_BINARY = (
    ROOT / "build" / "camera-wrapper-graph-v1" /
    "a9tas_camera_wrapper_graph_check_v1_review_only"
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
        "kWrapperSize = 0x98",
        "kWordCount == 19",
        "kMaximumPointerDereferences = kWordCount",
        "kExpectedPrimaryVptrRva = 0x9DF3FE8",
        "0x9DF4088", "0x9DF40D8", "0x9DF4128", "0x9DF4178",
        "kStaticCaptureOffsets",
        "0x78, 0x80, 0x88, 0x90",
        "I_ACCEPT_CAMERA_WRAPPER_GRAPH_READ_ONLY_V1",
        "O_RDONLY | O_CLOEXEC",
        "active::ObjectMapping(value_mapping)",
        "value_is_known_interface_vptr",
        "invoked_methods=0",
        "gameplay_writes=0",
        "ptrace_calls=0",
        "ExactBoundedGraphPasses",
        "WrongPrimaryVptrRejected",
        "ChangedWrapperAfterResolutionRejected",
        "SharedTargetIsNotDereferenced",
        "ReadFailureIsFatal",
    ):
        require(needle in combined, f"missing wrapper-graph policy {needle}")
    for forbidden in (
        "pwrite", "process_vm_writev", "PTRACE_", "ptrace(", "O_RDWR",
        "O_WRONLY", "mprotect(", "dlopen(", "dlsym(",
    ):
        require(forbidden not in combined,
                f"forbidden wrapper-graph primitive {forbidden}")
    require(binary.is_file(), "review-only wrapper-graph binary missing")
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
        "CAMERA_WRAPPER_GRAPH_POLICY passed=1 read_only=1 wrapper_bytes=152 "
        "word_slots=19 max_pointer_depth=1 invoked_methods=0 ptrace_calls=0 "
        "gameplay_writes=0 device_access=0 deployed=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
