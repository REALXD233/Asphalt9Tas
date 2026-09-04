#!/usr/bin/env python3
"""Verify the exact offline-reviewed inputs for the first A9PHEX1 live gate."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "android-port/baselines/phase_paced_five_frame_candidate_v1.json"


def main() -> int:
    try:
        data = json.loads(MANIFEST.read_text(encoding="utf-8"))
        if data.get("schema") != "a9tas.phase_paced_five_frame_candidate.v1":
            raise ValueError("schema")
        if data.get("status") != "live_failed_retired":
            raise ValueError("status")
        if data.get("frames") != 5 or data.get("report_magic") != "A9PHEX1":
            raise ValueError("gate identity")
        if data.get("boundaries") != [
            "fixed_delta", "controller_c98", "controller_c9c", "world_commit"
        ]:
            raise ValueError("boundary topology")
        if data.get("external_gameplay_writes") != ["fixed_delta"]:
            raise ValueError("write ownership")
        artifacts = data.get("artifacts")
        if not isinstance(artifacts, list) or len(artifacts) != 14:
            raise ValueError("artifact count")
        seen: set[str] = set()
        for item in artifacts:
            rel = item["path"]
            expected = item["sha256"]
            if rel in seen or len(expected) != 64:
                raise ValueError(f"artifact entry {rel}")
            seen.add(rel)
            path = ROOT / rel
            actual = hashlib.sha256(path.read_bytes()).hexdigest()
            if actual != expected:
                raise ValueError(f"hash mismatch {rel}")
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        print(f"PHASE_PACED_5F_CANDIDATE_FAIL reason={error}")
        return 1
    print(
        "PHASE_PACED_5F_CANDIDATE_OK frames=5 artifacts=14 "
        "boundaries=4 external_writes=fixed_delta_only "
        "live_authorized=0 live_failed=1 retired=1 device_access=0 deployed=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
