#!/usr/bin/env python3
"""Audit Android artifacts for the transport-free replay-session core."""

from __future__ import annotations

import pathlib
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = WORKSPACE / "android-port" / "src" / "authoritative_replay_session_v1.cpp"
HEADER = WORKSPACE / "android-port" / "src" / "authoritative_replay_session_v1.h"


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 5:
        print(
            "usage: policy PASSIVE SESSION_OBJECT TICK_OBJECT READELF OBJDUMP",
            file=sys.stderr,
        )
        return 2
    passive, session_object, tick_object, readelf, objdump = map(pathlib.Path, args)
    for path in (passive, session_object, tick_object, readelf, objdump, SOURCE, HEADER):
        require(path.is_file(), f"missing required file: {path}")

    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    combined = source + "\n" + header
    for token in (
        "kReplayQueueCapacity = 1000",
        "current_race_tick >= final_tick_",
        "std::min(playback_index_ + 1, frame_count_ - 1)",
        "authoritative_tick_v1::SelectOnNewTick",
        "authoritative_tick_v1::EndTick",
        "SelectionOutcome::kModeCancelled",
        "installed_fixed_interval_us_ = queued_fixed_interval_us_",
        "ClearInputQueue();",
        "AUTH_REPLAY_SESSION_BUILD_ONLY runtime=disabled",
    ):
        require(token in combined, f"missing source contract: {token}")

    for token in (
        "PTRACE_",
        "process_vm_",
        "/proc/",
        "socket(",
        "connect(",
        "pwrite(",
        "RemoteCall",
        "NitroEnable",
    ):
        require(token not in combined, f"forbidden runtime primitive: {token}")

    passive_header = run(str(readelf), "-h", str(passive))
    require("Machine:" in passive_header and "X86-64" in passive_header,
            "passive artifact is not Android x86_64 ELF")
    undefined = run(str(readelf), "-Ws", str(passive))
    for symbol in ("ptrace", "pread", "pwrite", "socket", "connect", "kill", "waitpid"):
        require(symbol not in undefined, f"forbidden passive import: {symbol}")

    session_symbols = run(str(readelf), "-Ws", str(session_object))
    tick_symbols = run(str(readelf), "-Ws", str(tick_object))
    require("a9tas_authoritative_replay_session_selftest_v1" in session_symbols,
            "session selftest entry is missing")
    session_disassembly = run(str(objdump), "-d", "--demangle", str(session_object))
    tick_disassembly = run(str(objdump), "-d", "--demangle", str(tick_object))
    for name in ("ReplaySessionV1::OnUpdate", "ReplaySessionV1::BeginTick", "ReplaySessionV1::EndTick"):
        require(name in session_disassembly, f"session function missing: {name}")
    require("SelectOnNewTick" in tick_disassembly and "EndTick" in tick_disassembly,
            "authoritative tick definitions missing from companion object")

    print(
        "AUTH_REPLAY_SESSION_CPP_POLICY passed=1 source_bound=1 "
        "a9utk1_bound=1 queue_capacity=1000 runtime=disabled "
        "device_access=0 game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"AUTH_REPLAY_SESSION_CPP_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
