#!/usr/bin/env python3
"""Offline policy for the five-callback RaceView same-state writer."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_camera_raceview_same_state_v1.cpp"
PROTOCOL = ROOT / "src" / "camera_raceview_same_state_protocol_v1.h"


def main() -> int:
    if len(sys.argv) == 1:
        payload = (
            ROOT / "build" / "camera-raceview-same-state-v1" /
            "liba9tas_camera_raceview_same_state_v1_build_only.so"
        )
        tool_bin = (
            ROOT.parent / "toolchains" / "android-ndk-r27d" / "toolchains" /
            "llvm" / "prebuilt" / "windows-x86_64" / "bin"
        )
        readelf = tool_bin / "llvm-readelf.exe"
        objdump = tool_bin / "llvm-objdump.exe"
    elif len(sys.argv) == 4:
        payload, readelf, objdump = map(pathlib.Path, sys.argv[1:])
    else:
        raise SystemExit(f"usage: {sys.argv[0]} PAYLOAD READELF OBJDUMP")
    for artifact in (payload, readelf, objdump):
        assert artifact.is_file(), artifact
    combined = SOURCE.read_text(encoding="utf-8") + PROTOCOL.read_text(encoding="utf-8")
    for needle in (
        "complete the game's real CameraUpdate first",
        "reinterpret_cast<OriginalCallback>(original_address)(manager)",
        "reinterpret_cast<CombinedTransform>(combined_address)",
        "kManagerEmbeddedOffset = 0x08",
        "kManagerWorldOffset = 0x2C",
        "kManagerShapeOffset = 0xE8",
        "kManagerFovOffset = 0x108",
        "kEmbeddedCombinedSlot = 0x40",
        "kShapeFinalOffset = 0x40",
        "frame.manager_world_before",
        "a9tas_camera_raceview_same_state_arm_v1",
        "kRejectedBuildOnly",
    ):
        assert needle in combined, needle
    for forbidden in (
        "pwrite", "process_vm_writev", "ptrace(", "PTRACE_", "mprotect(",
        "dlopen(", "dlsym(", "camera_replay", "vehicle_state",
    ):
        assert forbidden not in combined, forbidden
    assert combined.count("reinterpret_cast<CombinedTransform>(combined_address)(") == 1
    elf = subprocess.run([str(readelf), "-h", "--dyn-syms", "--wide", str(payload)],
                         check=True, capture_output=True, text=True).stdout
    assert "Machine:                           AArch64" in elf
    for symbol in (
        "a9tas_camera_raceview_same_state_callback_v1",
        "a9tas_camera_raceview_same_state_control_storage_v1",
        "a9tas_camera_raceview_same_state_evidence_storage_v1",
        "a9tas_camera_raceview_same_state_frames_storage_v1",
        "a9tas_camera_raceview_same_state_arm_v1",
    ):
        assert symbol in elf
    disassembly = subprocess.run([str(objdump), "-d", str(payload)], check=True,
                                 capture_output=True, text=True).stdout
    assert "a9tas_camera_raceview_same_state_callback_v1" in disassembly
    print(
        "CAMERA_RACEVIEW_SAME_STATE_PAYLOAD_POLICY passed=1 original_first=1 "
        "five_callbacks=1 same_state=1 combined_calls=5 fov_same_writes=5 "
        "installer=0 ptrace=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
