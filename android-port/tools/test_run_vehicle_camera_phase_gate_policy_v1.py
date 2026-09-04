#!/usr/bin/env python3
"""Offline policy proof for the guarded vehicle/camera phase Gate runner."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-vehicle-camera-phase-gate-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    text = RUNNER.read_text(encoding="utf-8")
    for needle in (
        '[string]$Mode = "OfflineValidate"',
        '"PrepareFreshProcess", "ExecutePhaseGate"',
        "$maximumEvents = 4096",
        "a9tas_lifecycle_source_360f_20260821_203328_152.a9utk1",
        "I_ACCEPT_VEHICLE_CAMERA_PHASE_SINGLE_SLOT_V1",
        "I_ACCEPT_FINAL_WRITER_UNIFIED_REVIEW_ONLY_V1",
        "AcknowledgeCurrentLifecycleAndRaceViewAddresses",
        "AcknowledgeSingleEscAndNoManualInput",
        "AcknowledgeFailureForceStopsFreshProcess",
        "VEHICLE_CAMERA_PHASE_RUNNER_OFFLINE",
        "VEHICLE_CAMERA_PHASE_LIVE_PASSED",
    ):
        require(needle in text, f"runner contract missing {needle}")

    offline = text.index("if ($Mode -eq 'OfflineValidate')")
    adb_exists = text.index("Test-Path -LiteralPath $AdbPath", offline)
    require(offline < adb_exists, "offline mode no longer returns before ADB")
    install = text.index("$installCommand =")
    mutation = text.index("$mutationStarted = $true", install)
    install_call = text.index("$installText = Invoke-AdbChecked", mutation)
    writer = text.index("$writerCommand =", install)
    writer_wait = text.index("$writerHandle.Process.WaitForExit", writer)
    finalize = text.index("$finalizeCommand =", writer_wait)
    pull = text.index("'pull phase Gate report'", finalize)
    validate = text.index("python -B $phaseValidator", pull)
    analyze = text.index("python -B $phaseAnalyzer", validate)
    require(install < writer < writer_wait < finalize < pull < validate < analyze,
            "install/replay/restore/export/analyze order changed")
    require(install < mutation < install_call < writer,
            "install transaction is not covered by failure retirement")
    require("$remoteTransaction install" in text and
            "$remoteTransaction finalize" in text,
            "camera single-slot transaction is incomplete")
    require("am force-stop $package" in text and
            "-not $success -and $mutationStarted" in text,
            "uncertain post-mutation failure does not retire the fresh process")
    require("manager +" not in text.lower() and "shape +" not in text.lower(),
            "runner contains a camera-state address write expression")
    require("camera_write=0" in text,
            "offline receipt does not state the camera-write invariant")
    print("RUN_VEHICLE_CAMERA_PHASE_GATE_POLICY passed=1 default_offline=1 "
          "lifecycle_start=1 single_slot_restore=1 fail_stop=1 camera_write=0 "
          "device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
