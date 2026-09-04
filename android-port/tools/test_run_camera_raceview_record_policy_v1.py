#!/usr/bin/env python3
"""Offline policy for the guarded RaceView record-only live runner."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-camera-raceview-record-v1.ps1"


def main() -> int:
    source = RUNNER.read_text(encoding="utf-8")
    for needle in (
        '"OfflineValidateOnly"',
        '"PrepareFreshProcess"',
        '"ExecuteCountdownCapture"',
        '"Status"',
        '"Rollback"',
        "AcknowledgeAncientRuinsZl1Countdown3Paused",
        "AcknowledgeExactlyOneEscapeResume",
        "AcknowledgeSingleRaceViewCallbackSlot",
        "AcknowledgeBriefWholeProcessStop",
        "input keyevent 111",
        "CAMERA_MANAGER_GRAPH_V1",
        "CAMERA_MANAGER_FIELD offset=0xe8",
        "RACEVIEW_RECORD_STATUS complete=1",
        "rollback",
        "c253eecb2244756edb53d54ad5f546301522378b7c83e4bbaaa1d491342b2639",
        "5e94b69c335dcdb22e0efb46888cadceb3903d216f5d189f458aa99313678a0a",
        "88a31fbd698fc18652a43479b03437e48a76a7361767f1431645b2153abf4077",
    ):
        assert needle in source, needle
    offline = source.index('if ($Mode -eq "OfflineValidateOnly")')
    adb_check = source.index("if (-not (Test-Path -LiteralPath $AdbPath")
    assert offline < adb_check
    assert "ptrace" not in source.lower()
    print(
        "CAMERA_RACEVIEW_RECORD_RUNNER_POLICY passed=1 default_offline=1 "
        "single_esc=1 automatic_finalize=1 rollback=1 ptrace=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
