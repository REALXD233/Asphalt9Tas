#!/usr/bin/env python3
"""Offline policy for the guarded standalone Camera Tool runner."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-camera-tool-v1.ps1"
INSPECTOR = ROOT / "tools" / "inspect_camera_tool_report_v1.py"
HELPER = ROOT / "tools" / "run_camera_tool_preload_v1.sh"


def main() -> int:
    combined = RUNNER.read_text(encoding="utf-8")
    for needle in (
        '"OfflineValidateOnly","PrepareFreshProcess","Install","SetAbsolute","Disable","Status","Uninstall"',
        "I_ACCEPT_CAMERA_TOOL_SINGLE_SLOT_V1",
        "AcknowledgeSingleRaceViewCallbackSlot",
        "AcknowledgeBriefWholeProcessStop",
        "AcknowledgeCameraOverrideWrites",
        "AcknowledgeCrashRisk",
        "Resolve-RaceView",
        "run_camera_tool_preload_v1.sh",
        "inspect_camera_tool_report_v1.py",
        "CAMERA_TOOL_DISABLED game_camera_natural=1",
        "CAMERA_TOOL_UNINSTALLED original_callback_restored=1",
    ):
        assert needle in combined, needle
    for forbidden in (
        "input keyevent", "camera smoothing", "CameraReplay", "A9G4R", "A9G5D",
    ):
        assert forbidden not in combined, forbidden
    inspector = INSPECTOR.read_text(encoding="utf-8")
    assert "REPORT_SIZE = 504" in inspector
    assert "EVIDENCE_OFFSET = 248" in inspector
    assert '"source_override_calls"' in inspector
    assert '"source_readback_passes"' in inspector
    helper = HELPER.read_text(encoding="utf-8")
    assert "--wait-window" in helper
    assert "liba9tas_bootstrap_camera_tool_v1.so" in helper
    print(
        "CAMERA_TOOL_RUNNER_POLICY passed=1 guarded=1 auto_esc=0 "
        "single_slot=1 source_authority=1 dynamic_update=1 disable=1 uninstall=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
