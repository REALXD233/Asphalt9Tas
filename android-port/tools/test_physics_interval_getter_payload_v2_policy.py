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
        "call original -> optionally overwrite *X8 -> capture final *X8",
        "blr x17",
        "bl PhysicsIntervalGetterAfterOriginalV2",
        "__atomic_store_n(output, requested_bits",
        "final_bits = __atomic_load_n(output",
        "a9tas_physics_interval_getter_arm_v2",
        "return -100",
        "installer=absent",
        "a9tas_physics_interval_getter_wrapper_data_v2",
        "a9tas_physics_interval_getter_shadow_data_v2",
        "a9tas_physics_interval_getter_continue_data_v2",
    )
    forbidden = (
        "mprotect(", "pwrite(", "process_vm_writev", "ptrace(",
        "InstallInternal", "AtomicSwapInstruction", "BuildNearBridge",
    )
    failures = [f"missing marker: {item}" for item in required if item not in text]
    failures += [f"forbidden marker: {item}" for item in forbidden if item in text]
    if failures:
        print("\n".join(failures))
        return 1
    print("PHYSICS_INTERVAL_GETTER_PAYLOAD_V2_POLICY passed=1 installer=absent runtime_arming=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
