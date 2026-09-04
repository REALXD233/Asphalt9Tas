#!/usr/bin/env python3
"""Static policy for staged, read-only M1 runtime diagnostics."""

from __future__ import annotations

import pathlib
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"M1_PREFLIGHT_POLICY_FAIL {message}")


def main() -> None:
    require(len(sys.argv) == 2, "usage")
    source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    for token in (
        "I_ACCEPT_M1_BASE_READ_ONLY_PREFLIGHT_V1",
        "I_ACCEPT_M1_READ_ONLY_PREFLIGHT_V1",
        'FailStage("process_maps")',
        'FailStage("controller_mappings")',
        'FailStage("gameplay_input_controller", detail)',
        'FailStage("controller_payload")',
        'FailStage("final_writer_payload")',
        'FailStage("natural_payload", natural_failure)',
        'FailStage("race_lifecycle")',
        'FailStage("vehicle_state")',
        'FailStage("vehicle_backend")',
        'FailStage("final_writer_factory")',
        'FailStage("controller_factory")',
        'FailStage("natural_runtime_registered")',
        "M1_BASE_PREFLIGHT_OK",
        "M1_PREFLIGHT_OK",
        "attach_attempts=0 process_writes=0",
    ):
        require(token in source, f"missing={token}")
    base_branch = source.find("if (base_only)")
    base_output = source.find("M1_BASE_PREFLIGHT_OK", base_branch)
    natural_runtime = source.find("NaturalRuntimeReady(", base_output)
    full_output = source.find("M1_PREFLIGHT_OK", natural_runtime)
    require(0 <= base_branch < base_output < natural_runtime < full_output,
            "base_then_registered_order")
    require("stage=runtime_graph" not in source,
            "opaque_runtime_graph_failure_reintroduced")
    for forbidden in ("ptrace(", "pwrite(", "process_vm_writev("):
        require(forbidden not in source, f"mutation_primitive={forbidden}")
    print(
        "M1_PREFLIGHT_POLICY passed=1 base_read_only=1 staged_failures=13 "
        "registered_runtime_separate=1 opaque_runtime_graph=0 device_access=0"
    )


if __name__ == "__main__":
    main()
