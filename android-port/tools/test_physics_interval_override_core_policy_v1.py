#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import re
import sys


def fail(message: str) -> None:
    raise SystemExit(message)


def main() -> int:
    if len(sys.argv) != 3:
        fail("usage: policy HEADER SOURCE")
    header = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    source = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
    combined = header + "\n" + source

    required = (
        "kPhysicsContextVtableRva = 0x8103830",
        "kPhysicsContextExecuteTokenRva = 0x38B74DC",
        "kPhysicsContextGetIntervalRva = 0x38B77C0",
        "kDefaultStepOptionsVtableRva = 0x81039A0",
        "kDefaultStepOptionsGetIntervalRva = 0x38B7C5C",
        "kBackendFixedSubstepRva = 0x4CC75F0",
        "kStepOptionsOffset = 0x170",
        "kInlineIntervalOffset = 0x178",
        "kAlternateStepOptionsActive",
        "static_assert(sizeof(RecordingMetadataV1) == 64",
        "device_access=0 game_writes=0",
    )
    for marker in required:
        if marker not in combined:
            fail(f"missing required marker: {marker}")

    forbidden = (
        r"\bptrace\b",
        r"process_vm_(readv|writev)",
        r"/proc/",
        r"\bpwrite\b",
        r"\bPTRACE_",
        r"adb",
    )
    for pattern in forbidden:
        if re.search(pattern, combined, re.IGNORECASE):
            fail(f"forbidden runtime capability: {pattern}")

    print("PHYSICS_INTERVAL_OVERRIDE_CORE_POLICY passed=1 "
          "runtime=disabled device_access=0 game_writes=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
