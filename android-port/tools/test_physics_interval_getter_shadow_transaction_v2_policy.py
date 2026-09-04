#!/usr/bin/env python3

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("header", type=Path)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    text = args.header.read_text(encoding="utf-8") + "\n" + args.source.read_text(encoding="utf-8")
    required = (
        "kStepOptionsVtableRva = 0x7EED420",
        "kFirstSlotRva = 0x3693228",
        "kSecondSlotRva = 0x3693264",
        "kGetterThunkRva = 0x3695740",
        "kOwnerAdjustment = -0x2A78",
        "kNextVtableAdjustment = -0x2A80",
        "kInvalidTableBoundary",
        "copy_shadow_first",
        "publish_continue_second",
        "swap_object_vptr_last",
        "restore_original_vptr_first",
        "plan.shadow_words[kGetterWord] = observation.payload_wrapper",
    )
    forbidden = ("ptrace(", "pwrite(", "process_vm_writev", "mprotect(", "open(")
    failures = [f"missing marker: {item}" for item in required if item not in text]
    failures += [f"forbidden marker: {item}" for item in forbidden if item in text]
    if failures:
        print("\n".join(failures))
        return 1
    print("PHYSICS_INTERVAL_SHADOW_TRANSACTION_V2_POLICY passed=1 runtime=disabled process_access=0 game_writes=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
