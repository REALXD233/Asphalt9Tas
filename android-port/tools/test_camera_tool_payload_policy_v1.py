#!/usr/bin/env python3
"""Offline source/ELF policy for the standalone Camera Tool carrier."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_camera_tool_v1.cpp"
PROTOCOL = ROOT / "src" / "camera_tool_protocol_v1.h"


def main() -> int:
    if len(sys.argv) == 1:
        payload = ROOT / "build" / "camera-tool-v1" / "liba9tas_camera_tool_v1_build_only.so"
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
    for artifact in (payload, readelf, objdump, SOURCE, PROTOCOL):
        assert artifact.is_file(), artifact
    combined = SOURCE.read_text(encoding="utf-8") + PROTOCOL.read_text(encoding="utf-8")
    for needle in (
        "Exact upstream Camera Tool order",
        "reinterpret_cast<OriginalCallback>(original_address)(manager)",
        "reinterpret_cast<CombinedTransform>(combined_address)",
        "kManagerEmbeddedOffset = 0x08",
        "kManagerWorldOffset = 0x2C",
        "kManagerSourceOffset = 0xD8",
        "kManagerShapeOffset = 0xE8",
        "kManagerFovOffset = 0x108",
        "kEmbeddedCombinedSlot = 0x40",
        "kSourcePositionOffset = 0x38",
        "kSourceRotationOffset = 0x44",
        "kSourceFovOffset = 0x124",
        "kSourcePositionSetterSlot = 0x88",
        "kSourceRotationSetterSlot = 0x90",
        "reinterpret_cast<PositionSetter>(position_setter)",
        "reinterpret_cast<RotationSetter>(rotation_setter)",
        "reinterpret_cast<FovSetter>(fov_setter)",
        "kSourceReadbackMismatch",
        "A synchronous",
        "reinterpret_cast<OriginalCallback>(original_address)(manager)",
        "ReadStableCommand",
        "command_sequence",
        "kUnsupportedRelativeMode",
        "a9tas_camera_tool_arm_v1",
    ):
        assert needle in combined, needle
    for forbidden in (
        "pwrite", "process_vm_writev", "ptrace(", "PTRACE_", "mprotect(",
        "dlopen(", "dlsym(", "camera_replay", "A9G4R", "A9G5D",
    ):
        assert forbidden not in combined, forbidden
    assert combined.index("reinterpret_cast<OriginalCallback>(original_address)(manager)") < \
        combined.index("reinterpret_cast<PositionSetter>(position_setter)") < \
        combined.index("reinterpret_cast<CombinedTransform>(combined_address)")
    elf = subprocess.run(
        [str(readelf), "-h", "--dyn-syms", "--wide", str(payload)],
        check=True, capture_output=True, text=True,
    ).stdout
    assert "Machine:                           AArch64" in elf
    for symbol in (
        "a9tas_camera_tool_callback_v1",
        "a9tas_camera_tool_control_storage_v1",
        "a9tas_camera_tool_evidence_storage_v1",
        "a9tas_camera_tool_arm_v1",
    ):
        assert symbol in elf
    disassembly = subprocess.run(
        [str(objdump), "-d", str(payload)], check=True,
        capture_output=True, text=True,
    ).stdout
    assert "a9tas_camera_tool_callback_v1" in disassembly
    print(
        "CAMERA_TOOL_PAYLOAD_POLICY passed=1 standalone=1 original_first=1 "
        "source_authority=1 recursion_passthrough=1 absolute_transform=1 fov=1 seqlock=1 "
        "replay_track=0 installer=0 "
        "ptrace=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
