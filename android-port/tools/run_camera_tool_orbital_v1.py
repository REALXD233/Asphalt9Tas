#!/usr/bin/env python3
"""Interactive upstream-style Camera Tool Orbital controller."""

from __future__ import annotations

import argparse
import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from camera_tool_controller_math_v1 import (  # noqa: E402
    OrbitalController,
    game_position_to_opengl,
)
from run_camera_tool_free_flight_v1 import (  # noqa: E402
    DEFAULT_SESSION,
    Keyboard,
    MouseSampler,
    StreamBridge,
    VK_M,
    VK_X,
    read_session,
)


def run(args: argparse.Namespace) -> int:
    session = read_session(args.session)
    bridge = StreamBridge(args.adb, args.device, session)
    keyboard = Keyboard()
    camera = bridge.start()
    mouse: MouseSampler | None = None
    controller = OrbitalController(
        distance=args.distance,
        sensitivity=args.sensitivity,
        zoom_speed=1.0,
    )
    print(
        "CAMERA_TOOL_ORBITAL_READY zoom=E/Q mouse_toggle=Ctrl+Alt+M "
        "exit=Ctrl+Alt+X target=authoritative_native_transform root_sessions=1"
    )
    deadline = None if args.duration == 0 else time.perf_counter() + args.duration
    interval = 1.0 / args.rate
    next_update = time.perf_counter()
    updates = 0
    try:
        mouse = MouseSampler(args.enable_mouse_on_start)
        while deadline is None or time.perf_counter() < deadline:
            now = time.perf_counter()
            if keyboard.chord_pressed(VK_X):
                break
            if keyboard.chord_pressed(VK_M):
                print(f"CAMERA_TOOL_MOUSE enabled={1 if mouse.toggle() else 0}")
            if now < next_update:
                time.sleep(min(next_update - now, 0.002))
                continue
            next_update += interval
            if next_update < now:
                next_update = now + interval
            game_target = bridge.request_target(timeout=10.0 if updates == 0 else 2.0)
            target = game_position_to_opengl(game_target)
            mouse_dx, mouse_dy = mouse.consume()
            controller.update(camera, target, keyboard.input_frame(mouse_dx, mouse_dy))
            bridge.publish(camera)
            updates += 1
    except KeyboardInterrupt:
        pass
    finally:
        if mouse is not None:
            mouse.close()
        bridge.close()
    print(
        f"CAMERA_TOOL_ORBITAL_END updates={updates} target_reads={updates} "
        "active=0 callback_retained=1"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--session", type=pathlib.Path, default=DEFAULT_SESSION)
    parser.add_argument("--adb", type=pathlib.Path, default=pathlib.Path(r"D:\leidian\LDPlayer9\adb.exe"))
    parser.add_argument("--device", default="emulator-5554")
    parser.add_argument("--rate", type=float, default=60.0)
    parser.add_argument("--duration", type=float, default=0.0)
    parser.add_argument("--distance", type=float, default=5.0)
    parser.add_argument("--sensitivity", type=float, default=0.2)
    parser.add_argument("--enable-mouse-on-start", action="store_true")
    args = parser.parse_args()
    if not (1.0 <= args.rate <= 120.0) or args.duration < 0.0:
        parser.error("invalid rate or duration")
    if not (0.1 <= args.distance <= 20.0) or not (0.01 <= args.sensitivity <= 0.5):
        parser.error("distance or sensitivity outside upstream GUI bounds")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
