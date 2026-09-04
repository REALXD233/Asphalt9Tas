#!/usr/bin/env python3
"""Reproducibly verify the exact Gate 5 Retry Run 2 evidence pair."""

from __future__ import annotations

import argparse
from pathlib import Path

from verify_gate5_run1_evidence_v1 import verify_evidence_pair


EXPECTED_RECORDING_SHA256 = (
    "f062cc82ea460c2f417c0932b9daa2204ad0d0323a85d9e36f2893c8d5756a1a"
)
EXPECTED_REPORT_SHA256 = (
    "3e2c6d284910d45e650c39b200325674405b93cef550ccfe7c8a19636e6f4d3b"
)


def verify_run2(recording_path: Path, report_path: Path) -> list[str]:
    return verify_evidence_pair(
        recording_path,
        report_path,
        expected_recording_hash=EXPECTED_RECORDING_SHA256,
        expected_report_hash=EXPECTED_REPORT_SHA256,
        expected_equal=2,
        expected_corrected=3,
        expected_pid=2668,
        expected_initial_threads=268,
        expected_final_threads=268,
    )


def main() -> int:
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "recording",
        nargs="?",
        type=Path,
        default=project
        / "evidence"
        / "a9tas_gate5_handoff_input_run2_retry_20260817.a9nps1",
    )
    parser.add_argument(
        "report",
        nargs="?",
        type=Path,
        default=project
        / "evidence"
        / "a9tas_gate5_handoff_audit_run2_retry_20260817.a9cdt1",
    )
    parser.add_argument("--require-supported", action="store_true")
    args = parser.parse_args()
    problems = verify_run2(args.recording, args.report)
    supported = not problems
    print(
        "gate5_run2_frames=5 equal=2 corrected=3 "
        f"recording_sha256={EXPECTED_RECORDING_SHA256} "
        f"report_sha256={EXPECTED_REPORT_SHA256} supported={int(supported)}"
    )
    for problem in problems:
        print(f"reject={problem}")
    return 0 if supported or not args.require_supported else 1


if __name__ == "__main__":
    raise SystemExit(main())
