#!/usr/bin/env python3
"""Audit the stable bridge from runtime events to authoritative selection."""

from __future__ import annotations

import pathlib
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = WORKSPACE / "android-port" / "src" / "authoritative_adapter_bridge_v1.cpp"
HEADER = WORKSPACE / "android-port" / "src" / "authoritative_adapter_bridge_v1.h"


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 5:
        print("usage: policy PASSIVE SELFTEST REVIEW_OBJECT READELF OBJDUMP", file=sys.stderr)
        return 2
    passive, selftest, review_object, readelf, objdump = map(pathlib.Path, args)
    for path in (passive, selftest, review_object, readelf, objdump, SOURCE, HEADER):
        require(path.is_file(), f"missing bridge input: {path}")

    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    combined = source + "\n" + header
    for token in (
        '#include "authoritative_unified_adapter_v1.cpp"',
        "InitializeAdapter(&bridge->state",
        "AdvanceAdapter(&bridge->state",
        "output.selected_packet = bridge->state.selected_packet",
        "consumed.race_tick = consumed_tick",
        "pair_writes == 688",
        "runtime=disabled",
    ):
        require(token in combined, f"missing bridge contract: {token}")
    for token in ("PTRACE_", "process_vm_", "/proc/", "pwrite(", "pread(", "socket(", "RemoteCall"):
        require(token not in combined, f"forbidden bridge runtime primitive: {token}")

    for binary in (passive, selftest):
        elf = run(str(readelf), "-h", str(binary))
        require("Machine:" in elf and "X86-64" in elf,
                f"not Android x86_64 ELF: {binary}")
        symbols = run(str(readelf), "-Ws", str(binary))
        for symbol in ("ptrace", "pread", "pwrite", "socket", "connect", "kill", "waitpid"):
            require(symbol not in symbols, f"forbidden bridge import {symbol}: {binary}")

    selftest_symbols = run(str(readelf), "-Ws", str(selftest))
    require("a9tas_authoritative_adapter_bridge_selftest_v1" in selftest_symbols,
            "linked bridge selftest entry missing")
    disassembly = run(str(objdump), "-d", "--demangle", str(review_object))
    for token in ("authoritative_bridge_v1::Create", "authoritative_bridge_v1::Advance", "authoritative_bridge_v1::Destroy"):
        require(token in disassembly, f"bridge API absent from review disassembly: {token}")

    print(
        "AUTHORITATIVE_ADAPTER_BRIDGE_CPP_POLICY passed=1 adapter_bound=1 "
        "selected_packet_exported=1 consumed_tick_bound=1 runtime=disabled "
        "device_access=0 game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"AUTHORITATIVE_ADAPTER_BRIDGE_CPP_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)

