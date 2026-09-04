#!/usr/bin/env python3
"""Static no-device policy for the Orbital host controller."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools" / "run_camera_tool_orbital_v1.py"


def main() -> int:
    text = SOURCE.read_text(encoding="utf-8")
    for needle in (
        "OrbitalController",
        "StreamBridge",
        "request_target(timeout=",
        "timeout=10.0 if updates == 0 else 2.0",
        "game_position_to_opengl",
        "target=authoritative_native_transform",
        "distance=args.distance",
        "zoom_speed=1.0",
        "keyboard.input_frame(mouse_dx, mouse_dy)",
        "Ctrl+Alt+M",
        "Ctrl+Alt+X",
        "target_reads={updates}",
        "callback_retained=1",
    ):
        assert needle in text, needle
    for forbidden in (
        "su -c", "ptrace", "/proc/", "process_vm_writev", "RaceView",
        "camera smoothing", "CameraReplay", "SetCursorPos",
    ):
        assert forbidden not in text, forbidden
    print(
        "CAMERA_TOOL_ORBITAL_POLICY passed=1 shared_persistent_stream=1 "
        "authoritative_vehicle_target=1 upstream_math=1 new_scan=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
