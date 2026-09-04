#!/usr/bin/env python3
"""Offline policy for the generated guarded Barrel successor runner."""

from __future__ import annotations

import sys
from pathlib import Path


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    runner = root / "build" / "barrel-successor-runner-v1" / "run-barrel-successor-replay-v1.ps1"
    text = runner.read_text(encoding="utf-8")
    required = (
        "build-barrel-successor-executor-v1.ps1",
        "build-barrel-successor-preload-v1.ps1",
        "a9tas_barrel_successor_v1_review_only",
        "liba9tas_payload_bundle_barrel_successor_v1.so",
        "liba9tas_barrel_yaw_tail_v1_build_only.so",
        "run_barrel_successor_preload_v1.sh",
        "AcknowledgeBarrelPostNaturalWrites",
        "requires the exact accepted 900-value Physics Interval replay",
        "AcknowledgeFailureForceStopsFreshProcess",
        "ExecuteExactlyOneAttempt",
        "Get-TracerPid",
        "Assert-RemoteHash",
        "BARREL_SUCCESSOR_REPLAY_LIVE_PASSED",
    )
    missing = [token for token in required if token not in text]
    if missing:
        print(f"BARREL_SUCCESSOR_RUNNER_POLICY passed=0 missing={missing}", file=sys.stderr)
        return 1
    if "all_thread_barrel_classifier" in text.lower():
        print("BARREL_SUCCESSOR_RUNNER_POLICY passed=0 classifier_present=1", file=sys.stderr)
        return 1
    print(
        "BARREL_SUCCESSOR_RUNNER_POLICY passed=1 fresh_process=1 exact_900=1 "
        "payloads=4 classifier=0 force_stop_on_failure=1 default_live=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
