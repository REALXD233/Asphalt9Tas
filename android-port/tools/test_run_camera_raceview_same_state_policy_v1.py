#!/usr/bin/env python3
"""Offline policy for the guarded five-callback same-state runner."""
from __future__ import annotations
import pathlib

ROOT=pathlib.Path(__file__).resolve().parents[1]
RUNNER=ROOT/"run-camera-raceview-same-state-v1.ps1"

def main()->int:
    text=RUNNER.read_text(encoding="utf-8")
    for needle in (
        '"OfflineValidateOnly"','"PrepareFreshProcess"','"ExecuteCountdownGate"',
        '"Status"','"Rollback"','AcknowledgeAncientRuinsZl1Countdown3Paused',
        'AcknowledgeExactlyOneEscapeResume','AcknowledgeFiveSameStateCallbacks',
        'input keyevent 111','CAMERA_MANAGER_GRAPH_V1',
        'CAMERA_MANAGER_FIELD offset=0xe8','RACEVIEW_SAME_STATE_STATUS complete=1',
        '58e888e4fb606fe3ec1b497c97a2a64c0b4dfc7dbd647ad0c01fa0558efd6ddf',
        'fb040864d2107f075864152b9618a29dcbd8bcaae80f8c3a776ffc2831df88a8',
        'dbad61a2ebc3b25e44c586442dc7285cc3a976fbdf332dd043585d46d21748fc',
        'f8804e3a31fee7520426b0851ab919d5131c9928c96d1ee0cc58dc50ca764fee',
        'validate_camera_raceview_same_state_report_v1.py',
        'final same-state receipt validation failed',
    ): assert needle in text,needle
    assert text.index('if ($Mode -eq "OfflineValidateOnly")') < text.index('if (-not (Test-Path -LiteralPath $AdbPath')
    assert "ptrace" not in text.lower()
    print("CAMERA_RACEVIEW_SAME_STATE_RUNNER_POLICY passed=1 default_offline=1 single_esc=1 five_callbacks=1 automatic_finalize=1 rollback=1 device_access=0")
    return 0

if __name__=="__main__": raise SystemExit(main())
