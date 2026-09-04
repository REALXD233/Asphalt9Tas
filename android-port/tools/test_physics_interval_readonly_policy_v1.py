#!/usr/bin/env python3
"""Static policy checks for the strictly read-only physics interval observer."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("runner", type=Path)
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    runner = args.runner.read_text(encoding="utf-8")
    required_source = (
        'open(mem_path, O_RDONLY | O_CLOEXEC)',
        "kContextStepOptionsOffset = 0x170",
        "kContextFixedIntervalOffset",
        "kInlineDefaultForAllSamples",
        "alternate_options_samples",
        "LoadRuntimeBuildProfile",
        "g_runtime_build.physics_context_vtable_rva",
        "ptrace=0 game_writes=0",
    )
    forbidden_source = (
        "O_RDWR", "pwrite(", "process_vm_writev", "PTRACE_",
        "ptrace(", "<sys/ptrace.h>", "RegisterCallback", "InstallHook",
    )
    required_runner = (
        'ValidateSet("OfflineValidate", "Observe")',
        "PHYSICS_INTERVAL_READONLY_OFFLINE_VALID",
        "--json",
        "TracerPid",
    )
    failures = [f"missing source marker: {x}" for x in required_source if x not in source]
    failures += [f"forbidden source marker: {x}" for x in forbidden_source if x in source]
    failures += [f"missing runner marker: {x}" for x in required_runner if x not in runner]
    if failures:
        for failure in failures:
            print(failure)
        return 1
    print("PHYSICS_INTERVAL_READONLY_POLICY passed=1 ptrace=0 game_writes=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
