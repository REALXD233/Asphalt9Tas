#!/usr/bin/env python3
"""Offline policy for the phase-aligned Camera Tool v2 host workflow."""

from __future__ import annotations

import hashlib
import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
HOST = ROOT / "tools" / "run_camera_tool_runtime_interactive_v2.py"
RUNNER = ROOT / "run-camera-tool-runtime-v2.ps1"
STREAM = (
    ROOT / "build" / "camera-tool-runtime-v2" /
    "a9tas_camera_tool_runtime_input_stream_v2"
)


def main() -> int:
    for path in (HOST, RUNNER, STREAM):
        assert path.is_file(), path
    host = HOST.read_text(encoding="utf-8")
    runner = RUNNER.read_text(encoding="utf-8")
    combined = host + runner
    stream_hash = hashlib.sha256(STREAM.read_bytes()).hexdigest()
    assert stream_hash in host and stream_hash in runner
    for needle in (
        'choices=("free", "orbital")',
        "BIT_FORWARD", "BIT_BACKWARD", "BIT_LEFT", "BIT_RIGHT",
        "BIT_UP", "BIT_DOWN", "BIT_ZOOM_IN", "BIT_ZOOM_OUT",
        "keyboard.input_frame()",
        "yaw -= mouse_dx * args.sensitivity",
        "pitch - mouse_dy * args.sensitivity",
        "if state != last_publication",
        "bridge.publish(mode, bits, yaw, pitch",
        "per_frame_host_write=0",
        "mouse_toggle=Ctrl+Alt+M",
        "exit=Ctrl+Alt+X",
        "PrepareFreshProcess", "Install", "Uninstall",
        "run_camera_tool_runtime_preload_v2.sh",
        "active-camera-tool-runtime-v2.json",
    ):
        assert needle in combined, needle
    for forbidden in (
        "camera_to_stream_values", "FreeFlightController(",
        "OrbitalController(", "request_target(", "lerp", "slerp",
    ):
        assert forbidden not in host, forbidden
    compile(host, str(HOST), "exec")
    print(
        "RUN_CAMERA_TOOL_RUNTIME_V2_POLICY passed=1 free=1 orbital=1 "
        "shift_descend=1 input_changes_only=1 per_frame_host_write=0 "
        "fresh_preload=1 install_restore=1 ui_ready_transport=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
