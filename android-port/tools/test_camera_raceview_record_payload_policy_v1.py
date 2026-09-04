#!/usr/bin/env python3
"""Offline policy for the build-only RaceView post-original recorder."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_camera_raceview_record_v1.cpp"
PROTOCOL = ROOT / "src" / "camera_raceview_record_protocol_v1.h"


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit(f"usage: {sys.argv[0]} PAYLOAD READELF OBJDUMP")
    payload, readelf, objdump = map(pathlib.Path, sys.argv[1:])
    combined = SOURCE.read_text(encoding="utf-8") + PROTOCOL.read_text(
        encoding="utf-8"
    )
    for needle in (
        "the complete real CameraUpdate runs first",
        "reinterpret_cast<OriginalCallback>(original_address)(manager)",
        "kManagerLocalOffset = 0x10",
        "kManagerWorldOffset = 0x2C",
        "kManagerShapeOffset = 0xE8",
        "kManagerFovOffset = 0x108",
        "kShapeFinalOffset = 0x40",
        "FramePermit(frame_index)",
        "kContinuousCapture",
        "kContinuousPermit",
        "continuous_capture",
        "a9tas_camera_raceview_record_arm_v1",
        "a9tas_camera_raceview_record_control_storage_v1",
        "a9tas_camera_raceview_record_evidence_storage_v1",
        "a9tas_camera_raceview_record_frames_storage_v1",
        "Fail open:",
        "if (!callable_original)",
        "kRejectedBuildOnly",
    ):
        assert needle in combined, needle
    for forbidden in (
        "pwrite", "process_vm_writev", "ptrace(", "PTRACE_", "mprotect(",
        "dlopen(", "dlsym(", "memcpy(", "camera_replay", "OverridePosition",
    ):
        assert forbidden not in combined, forbidden
    assert payload.is_file()
    elf = subprocess.run(
        [str(readelf), "-h", "-s", str(payload)], check=True,
        capture_output=True, text=True,
    ).stdout
    assert "Machine:                           AArch64" in elf
    assert "a9tas_camera_raceview_record_callback_v1" in elf
    assert "a9tas_camera_raceview_record_arm_v1" in elf
    assert "a9tas_camera_raceview_record_control_storage_v1" in elf
    assert "a9tas_camera_raceview_record_evidence_storage_v1" in elf
    assert "a9tas_camera_raceview_record_frames_storage_v1" in elf
    disassembly = subprocess.run(
        [str(objdump), "-d", str(payload)], check=True,
        capture_output=True, text=True,
    ).stdout
    assert "a9tas_camera_raceview_record_callback_v1" in disassembly
    print(
        "CAMERA_RACEVIEW_RECORD_PAYLOAD_POLICY passed=1 build_only=1 "
        "original_first=1 fail_open=1 direct_storage=1 permit_bound=1 "
        "continuous_camera_update=1 "
        "manager_shape_fov=1 "
        "camera_writes=0 installer=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
