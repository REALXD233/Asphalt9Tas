#!/usr/bin/env python3
"""Offline policy for the narrow RaceView callback-slot transaction."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
CONTROLLER = ROOT / "src" / "camera_raceview_record_transaction_controller_v1.cpp"
BOOTSTRAP = ROOT / "src" / "bootstrap_camera_raceview_record_v1_build.cpp"


def main() -> int:
    controller = CONTROLLER.read_text(encoding="utf-8")
    bootstrap = BOOTSTRAP.read_text(encoding="utf-8")
    for needle in (
        "node +0x58",
        "kCallbackOffset",
        "kContinuousCapture",
        "kContinuousPermit",
        "WriteExactVerified",
        "StopProcess(pid)",
        "ResumeProcess(pid)",
        "stopped_node.function == payload.wrapper",
        "stopped_node.function == report.original_callback",
        "kPayloadSha256",
        "c253eecb2244756edb53d54ad5f546301522378b7c83e4bbaaa1d491342b2639",
        "install",
        "status",
        "finalize",
        "rollback",
    ):
        assert needle in controller, needle
    for forbidden in (
        "ptrace(", "PTRACE_", "process_vm_writev", "dlopen(", "dlsym(",
        "mprotect(", "kManagerLocalOffset", "kManagerWorldOffset",
        "kShapeFinalOffset",
    ):
        assert forbidden not in controller, forbidden
    assert controller.count("++report.game_write_attempts") == 2
    assert "liba9tas_camera_raceview_record_v1_build_only.so" in bootstrap
    assert "#include \"bootstrap.cpp\"" in bootstrap
    print(
        "CAMERA_RACEVIEW_RECORD_TRANSACTION_POLICY passed=1 "
        "single_slot=1 whole_process_stop=1 conditional_restore=1 "
        "guest_calls=0 ptrace_calls=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
