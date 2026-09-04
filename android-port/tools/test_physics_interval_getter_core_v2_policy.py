#!/usr/bin/env python3

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("header", type=Path)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    header = args.header.read_text(encoding="utf-8")
    source = args.source.read_text(encoding="utf-8")
    required = (
        "A9PHY2 metadata ABI",
        "kStepOptionsVtableRva = 0x7EED420",
        "kGetterAdjustorThunkRva = 0x3695740",
        "kOwnerAdjustment = -0x2A78",
        "call_original_first",
        "override_after_original",
        "capture_final_output",
        "inline_field_write_allowed",
        "ApplyOriginalSemantics",
        "override_enabled ? metadata.physics_interval_bits : original_output_bits",
    )
    forbidden = (
        "ptrace(", "pwrite(", "process_vm_writev", "O_RDWR",
        "adb", "InstallHook(",
    )
    combined = header + "\n" + source
    failures = [f"missing marker: {item}" for item in required if item not in combined]
    failures += [f"forbidden marker: {item}" for item in forbidden if item in combined]
    if failures:
        print("\n".join(failures))
        return 1
    print("PHYSICS_INTERVAL_GETTER_CORE_V2_POLICY passed=1 runtime=disabled game_writes=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
