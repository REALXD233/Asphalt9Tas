#!/usr/bin/env python3
"""Audit Android artifacts for the authoritative unified adapter."""

from __future__ import annotations

import pathlib
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = WORKSPACE / "android-port" / "src" / "authoritative_unified_adapter_v1.cpp"


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 4:
        print("usage: policy PASSIVE ADAPTER_OBJECT READELF OBJDUMP", file=sys.stderr)
        return 2
    passive, adapter_object, readelf, objdump = map(pathlib.Path, args)
    for path in (passive, adapter_object, readelf, objdump, SOURCE):
        require(path.is_file(), f"missing required file: {path}")

    source = SOURCE.read_text(encoding="utf-8")
    for token in (
        '#include "unified_tick_executor_core_v1.cpp"',
        "ReplaySessionV1 pre_c98_checkpoint",
        "state->pre_c98_checkpoint = state->session",
        "state->session = state->pre_c98_checkpoint",
        "state->checkpoint_valid = false",
        "Advance(&state->phase, event",
        "state->session.OnUpdate(published.race_tick)",
        "state->poisoned = true",
        "AUTH_UNIFIED_ADAPTER_BUILD_ONLY runtime=disabled",
    ):
        require(token in source, f"missing adapter contract: {token}")

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
        require(token not in source, f"forbidden live primitive: {token}")

    passive_header = run(str(readelf), "-h", str(passive))
    require("Machine:" in passive_header and "X86-64" in passive_header,
            "passive artifact is not Android x86_64 ELF")
    passive_symbols = run(str(readelf), "-Ws", str(passive))
    for symbol in ("ptrace", "pread", "pwrite", "socket", "connect", "kill", "waitpid"):
        require(symbol not in passive_symbols, f"forbidden passive import: {symbol}")

    object_symbols = run(str(readelf), "-Ws", str(adapter_object))
    require("a9tas_authoritative_unified_adapter_selftest_v1" in object_symbols,
            "adapter selftest entry missing")
    disassembly = run(str(objdump), "-d", "--demangle", str(adapter_object))
    for name in (
        "InitializeAdapter",
        "AdvanceAdapter",
        "a9tas_authoritative_unified_adapter_selftest_v1",
    ):
        require(name in disassembly, f"adapter machine code missing: {name}")

    print(
        "AUTH_UNIFIED_ADAPTER_CPP_POLICY passed=1 phase_core_reused=1 "
        "pause_rollback=1 fail_closed=1 runtime=disabled device_access=0 "
        "game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"AUTH_UNIFIED_ADAPTER_CPP_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
