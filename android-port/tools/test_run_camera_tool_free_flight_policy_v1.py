#!/usr/bin/env python3
"""Static no-device policy for the Free Flight host controller."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools" / "run_camera_tool_free_flight_v1.py"


def main() -> int:
    text = SOURCE.read_text(encoding="utf-8")
    for needle in (
        "camera_tool_controller_math_v1",
        "active-camera-tool-v1.json",
        "289d057eea5e6d7242722d897a293c3cc042dd245b7dee550d4acfb456997225",
        "I_ACCEPT_CAMERA_TOOL_PERSISTENT_STREAM_V1",
        "CAMERA_TOOL_STREAM_READY",
        "CAMERA_TOOL_STREAM_TARGET",
        'self.process.stdin.write("TARGET\\n")',
        'match.group("active") != "0"',
        'int(match.group("entries")) == 0',
        "quaternion_norm < 0.25",
        "queue.Queue(maxsize=256)",
        '"SET 7 "',
        'self.process.stdin.write("QUIT\\n")',
        "FreeFlightController",
        "GetAsyncKeyState",
        "GetCursorPos",
        "SetCursorPos",
        'name.lower() == "dnplayer.exe"',
        "OpenProcess.restype = ctypes.c_void_p",
        "GetForegroundWindow.restype = ctypes.c_void_p",
        "mouse_toggle=Ctrl+Alt+M",
        "exit=Ctrl+Alt+X",
        "chord_pressed",
        "VK_LSHIFT",
        "controls=WASD/Space/Shift",
        "except KeyboardInterrupt:",
        "root_sessions=1",
        "callback_retained=1",
    ):
        assert needle in text, needle
    assert text.count("su -c") == 1
    for forbidden in (
        "ptrace", "/proc/", "process_vm_writev", "install hook", "CameraReplay",
        "VK_F8", "VK_F10", "VK_LCONTROL", "OrbitalController(", "RaceView",
    ):
        assert forbidden not in text, forbidden
    print(
        "CAMERA_TOOL_FREE_FLIGHT_POLICY passed=1 persistent_su=1 "
        "payload_stream_only=1 upstream_math=1 mouse_input=1 orbital_separate=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
