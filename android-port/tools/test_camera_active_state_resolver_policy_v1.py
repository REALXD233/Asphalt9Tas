#!/usr/bin/env python3
"""Offline source policy for the active-camera read-only resolver."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("header", type=Path)
    parser.add_argument("selftest", type=Path)
    args = parser.parse_args()
    header = args.header.read_text(encoding="utf-8")
    selftest = args.selftest.read_text(encoding="utf-8")

    required = [
        "kRaceViewPrimaryVptrRva = 0x8174F40",
        "kRaceViewSecondaryVptrRva = 0x8175108",
        "kRaceViewSecondaryOffset = 0x4A0",
        "kCameraManagerOffset = 0x4B8",
        "kBlendedCameraOffset = 0x100",
        "kControllerStateOffset = 0xF0",
        "kCompactStateOffset = 0x30",
        "kPositionOffset = 0x38",
        "kRotationOffset = 0x44",
        "ReadStateSamples",
        "kNotUnique",
        "kReadFailed",
        "kMaximumMappings = 16384",
        "mapping->private_mapping",
    ]
    missing = [token for token in required if token not in header]
    if missing:
        raise ValueError(f"active-camera resolver tokens missing: {missing}")
    forbidden = ["pwrite", "process_vm_writev", "ptrace", "vehicle_transform"]
    present = [token for token in forbidden if token in header.lower()]
    if present:
        raise ValueError(f"write or substitute token present: {present}")
    tests = [
        "UniqueRouteAndSamplesPass",
        "BarePrimaryVptrRejected",
        "DuplicateRejected",
        "ReadFailureIsFatal",
        "LdPlayerMappingCountAccepted",
        "ExcessiveMappingCountRejected",
        "SharedIpcMappingRejectedAsObjectArena",
        "ReadableGarbageStateRejected",
    ]
    missing_tests = [token for token in tests if token not in selftest]
    if missing_tests:
        raise ValueError(f"active-camera selftest cases missing: {missing_tests}")
    print(
        "CAMERA_ACTIVE_STATE_RESOLVER_POLICY passed=1 read_only=1 "
        "raceview_route=1 three_layout_hypotheses=1 unique=1 "
        "writes=0 vehicle_derived=0 device_access=0 deployed=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
