#!/usr/bin/env python3
"""Interactive input publisher for the phase-aligned Camera Tool v2 runtime."""

from __future__ import annotations

import argparse
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

from run_camera_tool_free_flight_v1 import (  # noqa: E402
    Keyboard,
    MouseSampler,
    VK_E,
    VK_Q,
    VK_M,
    VK_X,
)


STREAM = (
    ANDROID_PORT / "build" / "camera-tool-runtime-v2" /
    "a9tas_camera_tool_runtime_input_stream_v2"
)
STREAM_SHA256 = "49a212d730a0365b9a0ff04394c0a7883f2503a3bc2cc1d703a2835d233c1cf3"
DEFAULT_SESSION = (
    ANDROID_PORT / "build" / "camera-tool-runtime-runner-v2" /
    "active-camera-tool-runtime-v2.json"
)
REMOTE_STREAM = "/data/local/tmp/a9tas_camera_tool_runtime_input_stream_v2"
ACK = "I_ACCEPT_CAMERA_TOOL_INPUT_STREAM_V2"
READY_RE = re.compile(
    r"^CAMERA_TOOL_INPUT_READY active=(?P<active>[01]) mode=(?P<mode>\d+) "
    r"bits=0x(?P<bits>[0-9a-f]+) sequence=(?P<sequence>\d+) "
    r"entries=(?P<entries>\d+) steps=(?P<steps>\d+) "
    r"vehicle_reads=(?P<vehicle_reads>\d+) failures=(?P<failures>\d+) "
    r"status=(?P<status>-?\d+) dt=(?P<dt>[^ ]+) max_dt=(?P<max_dt>[^ ]+)$"
)

MODE_FREE = 2
MODE_ORBITAL = 3

BIT_FORWARD = 1 << 0
BIT_BACKWARD = 1 << 1
BIT_LEFT = 1 << 2
BIT_RIGHT = 1 << 3
BIT_UP = 1 << 4
BIT_DOWN = 1 << 5
BIT_ZOOM_IN = 1 << 6
BIT_ZOOM_OUT = 1 << 7


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse_hex(value: object, field: str) -> str:
    text = str(value).lower().removeprefix("0x")
    if not re.fullmatch(r"[0-9a-f]+", text) or int(text, 16) == 0:
        raise ValueError(f"invalid {field}: {value!r}")
    return text


def read_session(path: pathlib.Path) -> dict[str, object]:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    for field in ("pid", "start_ticks"):
        if int(value[field]) <= 0:
            raise ValueError(f"invalid {field}")
    for field in ("control", "evidence"):
        value[field] = parse_hex(value[field], field)
    return value


class Bridge:
    def __init__(self, adb: pathlib.Path, device: str,
                 session: dict[str, object]) -> None:
        self.adb = str(adb)
        self.device = device
        self.session = session
        self.process: subprocess.Popen[str] | None = None
        self.lines: queue.Queue[str] = queue.Queue(maxsize=512)
        self.reader: threading.Thread | None = None

    def _run(self, *args: str) -> str:
        completed = subprocess.run(
            [self.adb, "-s", self.device, *args], check=False,
            capture_output=True, text=True, timeout=30,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        output = (completed.stdout + completed.stderr).strip()
        if completed.returncode != 0:
            raise RuntimeError(f"adb failed ({completed.returncode}): {output}")
        return output

    def start(self) -> None:
        if not STREAM.is_file() or sha256(STREAM) != STREAM_SHA256:
            raise RuntimeError("pinned Camera Tool v2 input stream mismatch")
        self._run("push", str(STREAM), REMOTE_STREAM)
        self._run("shell", "chmod", "700", REMOTE_STREAM)
        remote_hash = self._run("shell", "sha256sum", REMOTE_STREAM).split()[0].lower()
        if remote_hash != STREAM_SHA256:
            raise RuntimeError("remote Camera Tool v2 input stream hash mismatch")
        command = " ".join((
            REMOTE_STREAM,
            str(int(self.session["pid"])),
            str(int(self.session["start_ticks"])),
            str(self.session["control"]),
            str(self.session["evidence"]),
            ACK,
        ))
        self.process = subprocess.Popen(
            [self.adb, "-s", self.device, "shell", f"su -c '{command}'"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1,
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
                    raise RuntimeError(
                        f"Camera Tool v2 stream exited: {self.process.returncode}"
                    )
                continue
            match = READY_RE.match(line)
            if match is None:
                continue
            if match.group("active") != "0" or match.group("failures") != "0":
                raise RuntimeError(f"Camera Tool v2 not clean: {line}")
            return
        raise RuntimeError("Camera Tool v2 stream READY timeout")

    def _reader_loop(self) -> None:
        assert self.process is not None and self.process.stdout is not None
        for raw in self.process.stdout:
            line = raw.rstrip("\r\n")
            try:
                self.lines.put_nowait(line)
            except queue.Full:
                self.lines.get_nowait()
                self.lines.put_nowait(line)

    def command(self, text: str) -> None:
        if (self.process is None or self.process.stdin is None or
                self.process.poll() is not None):
            raise RuntimeError("Camera Tool v2 stream is not alive")
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()

    def publish(self, mode: int, bits: int, yaw: float, pitch: float,
                speed: float, sensitivity: float, distance: float,
                zoom_speed: float, fov: float) -> None:
        values = (yaw, pitch, speed, sensitivity, distance, zoom_speed, fov)
        if not all(math.isfinite(value) for value in values):
            raise RuntimeError("non-finite Camera Tool v2 input")
        self.command(
            f"SET {mode} {bits} " +
            " ".join(format(value, ".9g") for value in values)
        )

    def close(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            try:
                self.command("DISABLE")
                self.command("STATUS")
                self.command("QUIT")
            except (BrokenPipeError, OSError, RuntimeError):
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


def input_bits(keyboard: Keyboard, mode: int) -> int:
    bits = 0
    frame = keyboard.input_frame()
    if mode == MODE_FREE:
        if frame.w:
            bits |= BIT_FORWARD
        if frame.s:
            bits |= BIT_BACKWARD
        if frame.a:
            bits |= BIT_LEFT
        if frame.d:
            bits |= BIT_RIGHT
        if frame.space:
            bits |= BIT_UP
        if frame.left_control:
            bits |= BIT_DOWN
    if keyboard.down(VK_E):
        bits |= BIT_ZOOM_IN
    if keyboard.down(VK_Q):
        bits |= BIT_ZOOM_OUT
    return bits


def run(args: argparse.Namespace) -> int:
    mode = MODE_FREE if args.mode == "free" else MODE_ORBITAL
    session = read_session(args.session)
    bridge = Bridge(args.adb, args.device, session)
    keyboard = Keyboard()
    mouse: MouseSampler | None = None
    yaw = 0.0
    pitch = 0.0
    last_publication: tuple[object, ...] | None = None
    publications = 0
    bridge.start()
    print(
        "CAMERA_TOOL_RUNTIME_V2_READY "
        f"mode={args.mode} controls=WASD/Space/Shift/E/Q "
        "mouse_toggle=Ctrl+Alt+M exit=Ctrl+Alt+X "
        "camera_phase_aligned=1 per_frame_host_write=0 root_sessions=1"
    )
    interval = 1.0 / args.input_rate
    deadline = None if args.duration == 0 else time.perf_counter() + args.duration
    try:
        mouse = MouseSampler(args.enable_mouse_on_start)
        next_poll = time.perf_counter()
        while deadline is None or time.perf_counter() < deadline:
            now = time.perf_counter()
            if keyboard.chord_pressed(VK_X):
                break
            if keyboard.chord_pressed(VK_M):
                print(f"CAMERA_TOOL_RUNTIME_MOUSE enabled={1 if mouse.toggle() else 0}")
            if now < next_poll:
                time.sleep(min(next_poll - now, 0.002))
                continue
            next_poll = now + interval
            mouse_dx, mouse_dy = mouse.consume()
            if mouse_dx or mouse_dy:
                yaw -= mouse_dx * args.sensitivity
                pitch = max(-89.0, min(89.0,
                    pitch - mouse_dy * args.sensitivity))
            bits = input_bits(keyboard, mode)
            state = (
                mode, bits, round(yaw, 7), round(pitch, 7), args.speed,
                args.sensitivity, args.distance, args.zoom_speed, args.fov,
            )
            if state != last_publication:
                bridge.publish(mode, bits, yaw, pitch, args.speed,
                               args.sensitivity, args.distance,
                               args.zoom_speed, args.fov)
                last_publication = state
                publications += 1
    except KeyboardInterrupt:
        pass
    finally:
        if mouse is not None:
            mouse.close()
        bridge.close()
    print(
        f"CAMERA_TOOL_RUNTIME_V2_END publications={publications} "
        "active=0 callback_retained=1"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("free", "orbital"), required=True)
    parser.add_argument("--session", type=pathlib.Path, default=DEFAULT_SESSION)
    parser.add_argument("--adb", type=pathlib.Path,
                        default=pathlib.Path(r"D:\leidian\LDPlayer9\adb.exe"))
    parser.add_argument("--device", default="emulator-5554")
    parser.add_argument("--input-rate", type=float, default=120.0)
    parser.add_argument("--duration", type=float, default=0.0)
    parser.add_argument("--speed", type=float, default=30.0)
    parser.add_argument("--sensitivity", type=float, default=0.2)
    parser.add_argument("--distance", type=float, default=5.0)
    parser.add_argument("--zoom-speed", type=float, default=1.0)
    parser.add_argument("--fov", type=float, default=0.959931076)
    parser.add_argument("--enable-mouse-on-start", action="store_true")
    args = parser.parse_args()
    if not (1.0 <= args.input_rate <= 500.0) or args.duration < 0.0:
        parser.error("invalid input rate or duration")
    if not (0.1 <= args.speed <= 1000.0):
        parser.error("speed outside upstream bounds")
    if not (0.01 <= args.sensitivity <= 0.5) or args.distance < 0.1 or \
            args.zoom_speed < 0.0 or args.fov <= 0.0:
        parser.error("invalid Camera Tool parameter")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
