#!/usr/bin/env python3
"""Offline policy for the generated AluTasV2 barrel-successor executor."""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path


EXPECTED_ANCHORS = {
    "include_barrel_integration",
    "runtime_capability_mask",
    "barrel_setup",
    "barrel_runtime_state",
    "c9c_barrel_layout",
    "post_phase_dr0",
    "f64_barrel_terminal",
    "barrel_deferred_release",
    "world_barrel_commit",
    "barrel_cleanup",
    "barrel_success_condition",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"BARREL_SUCCESSOR_EXECUTOR_POLICY passed=0 error={message}")


def main() -> int:
    if len(sys.argv) == 1:
        root = Path(__file__).resolve().parents[1]
        build = root / "build" / "barrel-successor-executor-v1"
        candidates = sorted(
            (path for path in build.glob("source-*") if path.is_dir()),
            key=lambda path: path.stat().st_mtime_ns,
            reverse=True,
        )
        require(bool(candidates), "missing_generated_directory")
        generated = candidates[0]
        candidate = build / "a9tas_barrel_successor_v1_review_only"
    elif len(sys.argv) == 4:
        root = Path(sys.argv[1]).resolve()
        generated = Path(sys.argv[2]).resolve()
        candidate = Path(sys.argv[3]).resolve()
    else:
        raise SystemExit(
            "usage: test_barrel_successor_executor_policy_v1.py "
            "<android-port-root> <generated-dir> <candidate>"
        )
    manifest_path = generated / "generation-manifest.json"
    source_path = generated / "hwbp_unified_tick_executor_v1.cpp"
    entry_path = generated / "barrel_successor_entry_v1.cpp"
    require(manifest_path.is_file(), "missing_manifest")
    require(source_path.is_file(), "missing_generated_source")
    require(entry_path.is_file(), "missing_entry")
    require(candidate.is_file() and candidate.stat().st_size != 0,
            "missing_candidate")

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    require(manifest.get("schema") ==
            "a9tas-barrel-successor-generation-manifest-v1", "schema")
    require(manifest.get("complete_successor") is True, "partial_successor")
    require(manifest.get("baseline_unchanged") is True, "baseline_changed")
    require(set(manifest.get("required_anchor_ids", [])) == EXPECTED_ANCHORS,
            "anchor_set")
    require(len(manifest.get("anchor_records", [])) == len(EXPECTED_ANCHORS),
            "anchor_record_count")

    outputs = {item["path"]: item["sha256"]
               for item in manifest.get("output_files", [])}
    require(outputs.get(source_path.name) == sha256(source_path),
            "generated_source_hash")
    for item in manifest.get("input_files", []):
        path = root / item["path"]
        require(path.is_file() and sha256(path) == item["sha256"],
                f"immutable_input_{item['path']}")

    source = source_path.read_text(encoding="utf-8")
    entry = entry_path.read_text(encoding="utf-8")
    for anchor in EXPECTED_ANCHORS:
        require(source.count(f"A9TAS_GENERATED_BEGIN anchor={anchor}") == 1,
                f"begin_{anchor}")
        require(source.count(f"A9TAS_GENERATED_END anchor={anchor}") == 1,
                f"end_{anchor}")
    require("#define A9TAS_BARREL_SUCCESSOR_V1 1" in entry,
            "entry_macro")
    require('#include "barrel_successor_runtime_v1.h"' in source,
            "runtime_missing")
    require("barrel_runtime.OnCertifiedC9C" in source, "c9c_missing")
    require("barrel_runtime.OnRbxFirstStore" in source, "rbx_first_missing")
    require("barrel_runtime.OnRbxSecondStore" in source, "rbx_second_missing")
    require("barrel_runtime.OnAngularAux" in source, "yaw_arm_missing")
    require("barrel_runtime.OnF64" in source, "f64_missing")
    require("barrel_runtime.OnWorldCommit" in source, "world_missing")
    require("barrel_runtime.Success(frames.size())" in source,
            "terminal_receipt_missing")
    require("if (signal == SIGTRAP)" in source and "kill(pid, SIGKILL)" in source,
            "payload_brk_not_fail_closed")
    forbidden = (
        "all_thread_barrel_classifier",
        "barrel_classifier_gate",
        "parse_hwbp_barrel_angular",
    )
    require(not any(token in source.lower() for token in forbidden),
            "retired_classifier_present")
    print(
        "BARREL_SUCCESSOR_EXECUTOR_POLICY passed=1 anchors=11 "
        "baseline_unchanged=1 classifier=0 payload_brk_fail_closed=1 "
        f"candidate_sha256={sha256(candidate)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
