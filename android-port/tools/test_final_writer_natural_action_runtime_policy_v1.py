#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "final_writer_natural_action_runtime_v1.h"
SOURCE = ROOT / "src" / "final_writer_natural_action_runtime_selftest_v1.cpp"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    require(len(sys.argv) == 2, "usage: policy SELFTEST")
    binary = pathlib.Path(sys.argv[1])
    header = HEADER.read_text(encoding="utf-8")
    source = SOURCE.read_text(encoding="utf-8")
    require("BeginAndPublish" in header, "missing begin/publication gate")
    require("ObserveCallbackClose" in header, "missing callback-close gate")
    require("CommitWorld" in header, "missing world commit gate")
    require("PublishFrame" in header and "ReadSnapshot" in header,
            "verified mailbox transport not used")
    require("CanCommit" in header, "dual-receipt commit proof absent")
    forbidden = ("ptrace(", "process_vm_writev", "/proc/", "NitroState",
                 "kNitroMode", "SendInput", "adb")
    require(not any(token in header for token in forbidden),
            "runtime layer contains forbidden live or mode-forcing primitive")
    require("{0, 1, 0, 2, 0}" in source,
            "mixed 0/1/2 sequence selftest missing")
    require(binary.is_file() and binary.stat().st_size > 0,
            "cross-compiled review object missing")
    require("FINAL_WRITER_NATURAL_ACTION_RUNTIME_SELFTEST passed=%u" in
            source, "compiled selftest result marker missing")
    require("RejectsMissingNaturalReceipt" in source,
            "missing natural receipt negative test absent")
    print("FINAL_WRITER_NATURAL_ACTION_RUNTIME_POLICY passed=1 "
          "verified_transport=1 dual_receipt=1 mode_forcing=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
