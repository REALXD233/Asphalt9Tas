#!/usr/bin/env python3
"""Interactive upstream-style Camera Tool Free Flight controller.

The already-installed ARM64 Camera Tool payload remains the sole game-facing
backend.  This host keeps one privileged adb stream alive and publishes only
payload control commands.  It never installs hooks or writes game-owned state.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import pathlib
import queue
import re
import subprocess
import sys
import threading
import time

HERE = pathlib.Path(__file__).resolve().parent
ANDROID_PORT = HERE.parent
sys.path.insert(0, str(HERE))

from camera_tool_controller_math_v1 import (  # noqa: E402
    Camera,
    FreeFlightController,
    InputFrame,
    camera_to_stream_values,
    game_position_to_opengl,
    game_rotation_to_opengl,
    Vec3,
)


STREAM = ANDROID_PORT / "build" / "camera-tool-stream-v1" / "a9tas_camera_tool_stream_v1"
STREAM_SHA256 = "289d057eea5e6d7242722d897a293c3cc042dd245b7dee550d4acfb456997225"
DEFAULT_SESSION = ANDROID_PORT / "build" / "camera-tool-runner-v1" / "active-camera-tool-v1.json"
REMOTE_STREAM = "/data/local/tmp/a9tas_camera_tool_stream_v1"
ACK = "I_ACCEPT_CAMERA_TOOL_PERSISTENT_STREAM_V1"
READY_RE = re.compile(
    r"^CAMERA_TOOL_STREAM_READY active=(?P<active>[01]) sequence=(?P<sequence>\d+) "
    r"entries=(?P<entries>\d+) failures=(?P<failures>\d+) .* "
    r"natural=(?P<natural>[^\s]+)$"
)
TARGET_RE = re.compile(
    r"^CAMERA_TOOL_STREAM_TARGET reads=(?P<reads>\d+) "
    r"game=(?P<x>[^,]+),(?P<y>[^,]+),(?P<z>[^\s]+)$"
)

VK_W = 0x57
VK_A = 0x41
VK_S = 0x53
VK_D = 0x44
VK_E = 0x45
VK_Q = 0x51
VK_SPACE = 0x20
VK_LSHIFT = 0xA0
VK_CONTROL = 0x11
VK_MENU = 0x12
VK_M = 0x4D
VK_X = 0x58


class Point(ctypes.Structure):
    _fields_ = (("x", ctypes.c_long), ("y", ctypes.c_long))


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_hex_field(value: object, field: str) -> str:
    text = str(value).lower().removeprefix("0x")
    if not re.fullmatch(r"[0-9a-f]+", text) or int(text, 16) == 0:
        raise ValueError(f"invalid {field}: {value!r}")
    return text


def read_session(path: pathlib.Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    for field in ("pid", "start_ticks"):
        if int(value[field]) <= 0:
            raise ValueError(f"invalid {field}")
    for field in ("game_base", "manager", "shape"):
        value[field] = parse_hex_field(value[field], field)
    return value


class StreamBridge:
    def __init__(self, adb: pathlib.Path, device: str, session: dict[str, object]) -> None:
        self.adb = str(adb)
        self.device = device
        self.session = session
        self.process: subprocess.Popen[str] | None = None
        self.lines: queue.Queue[str] = queue.Queue(maxsize=256)
        self.reader: threading.Thread | None = None

    def _run(self, *args: str) -> str:
        completed = subprocess.run(
            [self.adb, "-s", self.device, *args],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        output = (completed.stdout + completed.stderr).strip()
        if completed.returncode != 0:
            raise RuntimeError(f"adb failed ({completed.returncode}): {output}")
        return output

    def start(self) -> Camera:
        if not STREAM.is_file() or sha256(STREAM) != STREAM_SHA256:
            raise RuntimeError("pinned Camera Tool stream artifact mismatch")
        self._run("push", str(STREAM), REMOTE_STREAM)
        self._run("shell", "chmod", "700", REMOTE_STREAM)
        remote_hash = self._run("shell", "sha256sum", REMOTE_STREAM).split()[0].lower()
        if remote_hash != STREAM_SHA256:
            raise RuntimeError("remote Camera Tool stream hash mismatch")

        command = " ".join((
            REMOTE_STREAM,
            str(int(self.session["pid"])),
            str(int(self.session["start_ticks"])),
            str(self.session["game_base"]),
            str(self.session["manager"]),
            str(self.session["shape"]),
            ACK,
        ))
        self.process = subprocess.Popen(
            [self.adb, "-s", self.device, "shell", f"su -c '{command}'"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        self.reader = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader.start()
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=0.1)
            except queue.Empty:
                if self.process.poll() is not None:
                    raise RuntimeError(f"Camera Tool stream exited: {self.process.returncode}")
                continue
            match = READY_RE.match(line)
            if match is None:
                continue
            if match.group("failures") != "0":
                raise RuntimeError(f"payload already faulted: {line}")
            natural = [float(item) for item in match.group("natural").split(",")]
            quaternion_norm = sum(value * value for value in natural[3:7])
            if (match.group("active") != "0" or int(match.group("entries")) == 0 or
                    len(natural) != 8 or not all(math.isfinite(value) for value in natural) or
                    quaternion_norm < 0.25 or not (0.05 <= natural[7] <= math.pi)):
                raise RuntimeError(f"invalid natural camera receipt: {line}")
            position = game_position_to_opengl(Vec3(*natural[:3]))
            rotation = game_rotation_to_opengl(tuple(natural[3:7]))
            return Camera(position, rotation, natural[7])
        raise RuntimeError("Camera Tool stream READY timeout")

    def _reader_loop(self) -> None:
        assert self.process is not None and self.process.stdout is not None
        for raw in self.process.stdout:
            line = raw.rstrip("\r\n")
            try:
                self.lines.put_nowait(line)
            except queue.Full:
                try:
                    self.lines.get_nowait()
                except queue.Empty:
                    pass
                self.lines.put_nowait(line)

    def publish(self, camera: Camera) -> None:
        if self.process is None or self.process.stdin is None or self.process.poll() is not None:
            raise RuntimeError("Camera Tool stream is not alive")
        values = camera_to_stream_values(camera)
        command = "SET 7 " + " ".join(format(value, ".9g") for value in values) + "\n"
        self.process.stdin.write(command)
        self.process.stdin.flush()

    def request_target(self, timeout: float = 2.0) -> Vec3:
        if self.process is None or self.process.stdin is None or self.process.poll() is not None:
            raise RuntimeError("Camera Tool stream is not alive")
        self.process.stdin.write("TARGET\n")
        self.process.stdin.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                remaining = max(0.0, deadline - time.monotonic())
                line = self.lines.get(timeout=min(0.1, remaining))
            except queue.Empty:
                if self.process.poll() is not None:
                    raise RuntimeError(f"Camera Tool stream exited: {self.process.returncode}")
                continue
            match = TARGET_RE.match(line)
            if match is None:
                continue
            values = Vec3(float(match.group("x")), float(match.group("y")), float(match.group("z")))
            if not all(math.isfinite(value) for value in (values.x, values.y, values.z)):
                raise RuntimeError(f"invalid vehicle target receipt: {line}")
            return values
        raise RuntimeError("Camera Tool vehicle TARGET timeout")

    def close(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None and self.process.stdin is not None:
            try:
                self.process.stdin.write("QUIT\n")
                self.process.stdin.flush()
            except (BrokenPipeError, OSError):
                pass
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
        self.process = None


class Keyboard:
    def __init__(self) -> None:
        if sys.platform != "win32":
            raise RuntimeError("interactive Free Flight requires Windows")
        self.user32 = ctypes.windll.user32
        self.previous: dict[int, bool] = {}

    def down(self, key: int) -> bool:
        return bool(self.user32.GetAsyncKeyState(key) & 0x8000)

    def pressed(self, key: int) -> bool:
        now = self.down(key)
        before = self.previous.get(key, False)
        self.previous[key] = now
        return now and not before

    def chord_pressed(self, key: int) -> bool:
        identity = 0x10000 + key
        now = self.down(VK_CONTROL) and self.down(VK_MENU) and self.down(key)
        before = self.previous.get(identity, False)
        self.previous[identity] = now
        return now and not before

    def input_frame(self, mouse_dx: float = 0.0, mouse_dy: float = 0.0) -> InputFrame:
        return InputFrame(
            w=self.down(VK_W), s=self.down(VK_S), a=self.down(VK_A), d=self.down(VK_D),
            # Android host adaptation: Shift replaces upstream Left Control so
            # Ctrl+Alt camera hotkeys cannot also move the camera downward.
            space=self.down(VK_SPACE), left_control=self.down(VK_LSHIFT),
            e=self.down(VK_E), q=self.down(VK_Q),
            mouse_dx=mouse_dx, mouse_dy=mouse_dy,
        )


class MouseSampler:
    """AluTasV2-style accumulated Win32 cursor delta with recentering."""

    def __init__(self, enabled: bool) -> None:
        self.user32 = ctypes.windll.user32
        self.kernel32 = ctypes.windll.kernel32
        self.user32.GetForegroundWindow.argtypes = ()
        self.user32.GetForegroundWindow.restype = ctypes.c_void_p
        self.user32.GetWindowThreadProcessId.argtypes = (ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong))
        self.user32.GetWindowThreadProcessId.restype = ctypes.c_ulong
        self.kernel32.OpenProcess.argtypes = (ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong)
        self.kernel32.OpenProcess.restype = ctypes.c_void_p
        self.kernel32.QueryFullProcessImageNameW.argtypes = (
            ctypes.c_void_p, ctypes.c_ulong, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_ulong)
        )
        self.kernel32.QueryFullProcessImageNameW.restype = ctypes.c_int
        self.kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        self.kernel32.CloseHandle.restype = ctypes.c_int
        self.enabled = enabled
        self.running = True
        self.lock = threading.Lock()
        self.delta_x = 0
        self.delta_y = 0
        self.last = self._position()
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _position(self) -> Point:
        point = Point()
        if not self.user32.GetCursorPos(ctypes.byref(point)):
            raise RuntimeError("GetCursorPos failed")
        return point

    def _game_is_foreground(self) -> bool:
        window = self.user32.GetForegroundWindow()
        if not window:
            return False
        process_id = ctypes.c_ulong()
        self.user32.GetWindowThreadProcessId(window, ctypes.byref(process_id))
        access = 0x1000  # PROCESS_QUERY_LIMITED_INFORMATION
        process = self.kernel32.OpenProcess(access, False, process_id.value)
        if not process:
            return False
        try:
            capacity = ctypes.c_ulong(1024)
            buffer = ctypes.create_unicode_buffer(capacity.value)
            if not self.kernel32.QueryFullProcessImageNameW(process, 0, buffer, ctypes.byref(capacity)):
                return False
            return pathlib.Path(buffer.value).name.lower() == "dnplayer.exe"
        finally:
            self.kernel32.CloseHandle(process)

    def _loop(self) -> None:
        reset_x = 100
        reset_y = 100
        while self.running:
            if not self.enabled or not self._game_is_foreground():
                self.last = self._position()
                time.sleep(0.01)
                continue
            now = self._position()
            with self.lock:
                self.delta_x += now.x - self.last.x
                self.delta_y += now.y - self.last.y
            self.user32.SetCursorPos(reset_x, reset_y)
            self.last = Point(reset_x, reset_y)
            time.sleep(0.001)

    def toggle(self) -> bool:
        self.enabled = not self.enabled
        self.last = self._position()
        with self.lock:
            self.delta_x = 0
            self.delta_y = 0
        return self.enabled

    def consume(self) -> tuple[int, int]:
        with self.lock:
            result = (self.delta_x, self.delta_y)
            self.delta_x = 0
            self.delta_y = 0
        return result

    def close(self) -> None:
        self.running = False
        self.thread.join(timeout=1.0)


def run(args: argparse.Namespace) -> int:
    session = read_session(args.session)
    bridge = StreamBridge(args.adb, args.device, session)
    keyboard = Keyboard()
    camera = bridge.start()
    mouse: MouseSampler | None = None
    controller = FreeFlightController(move_speed=args.speed, sensitivity=args.sensitivity)
    print(
        "CAMERA_TOOL_FREE_FLIGHT_READY controls=WASD/Space/Shift speed=E/Q "
        "mouse_toggle=Ctrl+Alt+M exit=Ctrl+Alt+X root_sessions=1"
    )
    deadline = None if args.duration == 0 else time.perf_counter() + args.duration
    interval = 1.0 / args.rate
    last = time.perf_counter()
    next_update = last
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
            dt = now - last
            last = now
            next_update += interval
            if next_update < now:
                next_update = now + interval
            mouse_dx, mouse_dy = mouse.consume()
            controller.update(camera, keyboard.input_frame(mouse_dx, mouse_dy), dt)
            bridge.publish(camera)
            updates += 1
    except KeyboardInterrupt:
        pass
    finally:
        if mouse is not None:
            mouse.close()
        bridge.close()
    print(f"CAMERA_TOOL_FREE_FLIGHT_END updates={updates} active=0 callback_retained=1")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--session", type=pathlib.Path, default=DEFAULT_SESSION)
    parser.add_argument("--adb", type=pathlib.Path, default=pathlib.Path(r"D:\leidian\LDPlayer9\adb.exe"))
    parser.add_argument("--device", default="emulator-5554")
    parser.add_argument("--rate", type=float, default=60.0)
    parser.add_argument("--duration", type=float, default=0.0, help="seconds; 0 runs until Ctrl+Alt+X")
    parser.add_argument("--speed", type=float, default=100.0)
    parser.add_argument("--sensitivity", type=float, default=0.2)
    parser.add_argument("--enable-mouse-on-start", action="store_true")
    args = parser.parse_args()
    if not (1.0 <= args.rate <= 240.0) or args.duration < 0.0:
        parser.error("invalid rate or duration")
    if not (0.1 <= args.speed <= 1000.0) or not (0.01 <= args.sensitivity <= 0.5):
        parser.error("speed or sensitivity outside upstream GUI bounds")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
