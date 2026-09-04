#!/usr/bin/env python3
"""Offline source policy for the exact RaceView callback node resolver."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "camera_raceview_callback_node_v1.h"
SELFTEST = ROOT / "src" / "camera_raceview_callback_node_selftest_v1.cpp"


def main() -> int:
    combined = HEADER.read_text(encoding="utf-8") + SELFTEST.read_text(
        encoding="utf-8"
    )
    for needle in (
        "kManagerNodeOffset = 0xE0",
        "kNodeSize = 0x68",
        "kExpectedNodeVptrRva = 0x81766D8",
        "kEnabledOffset = 0x40",
        "kOwnerOffset = 0x48",
        "kSelfOffset = 0x50",
        "kCallbackOffset = 0x58",
        "kExpectedCallbackRva = 0x3A787D8",
        "kContextOffset = 0x60",
        "kCallbackMismatch",
    ):
        assert needle in combined, needle
    for forbidden in (
        "pwrite", "process_vm_writev", "ptrace(", "PTRACE_", "O_RDWR",
        "mprotect(", "dlopen(", "dlsym(",
    ):
        assert forbidden not in combined, forbidden
    print(
        "CAMERA_RACEVIEW_CALLBACK_NODE_POLICY passed=1 read_only=1 "
        "exact_node=1 callback_slot=0x58 writes=0 ptrace_calls=0 "
        "device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
