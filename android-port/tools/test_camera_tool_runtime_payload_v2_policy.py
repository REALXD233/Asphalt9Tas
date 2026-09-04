#!/usr/bin/env python3
"""Offline source/ELF policy for the phase-aligned Camera Tool v2 payload."""

from __future__ import annotations

import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_camera_tool_runtime_v2.cpp"
PROTOCOL = ROOT / "src" / "camera_tool_runtime_protocol_v2.h"
CORE = ROOT / "src" / "camera_tool_runtime_core_v2.h"


def main() -> int:
    if len(sys.argv) == 1:
        payload = (
            ROOT / "build" / "camera-tool-runtime-v2" /
            "liba9tas_camera_tool_runtime_v2_build_only.so"
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
    for artifact in (payload, readelf, objdump, SOURCE, PROTOCOL, CORE):
        assert artifact.is_file(), artifact

    text = "\n".join(
        path.read_text(encoding="utf-8") for path in (SOURCE, PROTOCOL, CORE)
    )
    for needle in (
        "a9tas_camera_tool_runtime_callback_v2",
        "reinterpret_cast<OriginalCallback>(original_address)(manager)",
        "ReadStableCommand",
        "clock_gettime(CLOCK_MONOTONIC",
        "core::Step(&g_runtime_state",
        "ReadVehicleTarget(&input.vehicle_target_game)",
        "native_body + kVehicleNativePoseOffset",
        "reinterpret_cast<PositionSetter>(position_setter)",
        "reinterpret_cast<RotationSetter>(rotation_setter)",
        "reinterpret_cast<FovSetter>(fov_setter)",
        "reinterpret_cast<CombinedTransform>(combined_address)",
        "kSourceReadbackMismatch",
        "g_wrapper_depth.fetch_add",
        "std::clamp(input.dt_seconds, 0.0f, 0.05f)",
        "a9tas_camera_tool_runtime_arm_v2",
        "expected_hud_hidden_getter",
        "published_hud_visibility",
        "hud_original_code",
        "FlushHudInstructionCacheIfPending",
        "__builtin___clear_cache(reinterpret_cast<char*>(target)",
        "(load & 0xFFC003FFu) != 0x39400000u",
        "publication & protocol::kHudPublicationActive",
        "reinterpret_cast<const volatile std::uint8_t*>(object)[offset]",
    ):
        assert needle in text, needle
    for forbidden in (
        "pwrite", "process_vm_writev", "ptrace(", "PTRACE_", "mprotect(",
        "dlopen(", "dlsym(", "CameraReplay", "camera_replay", "lerp", "slerp",
        "HudVisibilitySetter",
        "reinterpret_cast<HudVisibilitySetter>",
        "RejectUnsafeHudPublication",
    ):
        assert forbidden not in text, forbidden
    assert text.index("reinterpret_cast<OriginalCallback>(original_address)(manager)") < \
        text.index("ReadVehicleTarget(&input.vehicle_target_game)") < \
        text.index("reinterpret_cast<PositionSetter>(position_setter)") < \
        text.index("reinterpret_cast<CombinedTransform>(combined_address)")

    elf = subprocess.run(
        [str(readelf), "-h", "--dyn-syms", "--wide", str(payload)],
        check=True, capture_output=True, text=True,
    ).stdout
    assert "Machine:                           AArch64" in elf
    for symbol in (
        "a9tas_camera_tool_runtime_callback_v2",
        "a9tas_camera_tool_runtime_control_storage_v2",
        "a9tas_camera_tool_runtime_evidence_storage_v2",
        "a9tas_camera_tool_hud_visible_getter_v1",
        "a9tas_camera_tool_runtime_arm_v2",
    ):
        assert symbol in elf, symbol
    disassembly = subprocess.run(
        [str(objdump), "-d", str(payload)], check=True,
        capture_output=True, text=True,
    ).stdout
    assert "a9tas_camera_tool_runtime_callback_v2" in disassembly
    print(
        "CAMERA_TOOL_RUNTIME_PAYLOAD_V2_POLICY passed=1 arm64=1 "
        "original_first=1 phase_aligned=1 vehicle_target_in_callback=1 "
        "source_authority=1 combined_same_callback=1 double_buffer=1 "
        "hud_native_setter=0 hud_direct_visibility_query=1 hud_non_target_emulation=1 hud_cache_flush=1 interpolation=0 replay_track=0 "
        "installer=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
