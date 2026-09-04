#!/usr/bin/env python3
"""Audit the pure C++ authoritative-tick core and its Android objects."""

from __future__ import annotations

import pathlib
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = WORKSPACE / "android-port" / "src" / "authoritative_tick_state_machine_v1.cpp"
HEADER = WORKSPACE / "android-port" / "src" / "authoritative_tick_state_machine_v1.h"


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 4:
        print("usage: policy PASSIVE SELFTEST_OBJECT READELF OBJDUMP", file=sys.stderr)
        return 2
    passive, selftest_object, readelf, objdump = map(pathlib.Path, args)
    for path in (passive, selftest_object, readelf, objdump, SOURCE, HEADER):
        require(path.is_file(), f"missing required file: {path}")

    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    combined = source + "\n" + header
    for token in (
        "SelectionOutcome::kBlockedEmpty",
        "SelectionOutcome::kFutureHeadGap",
        "SelectionOutcome::kStaleDroppedNoBlock",
        "packet_tick < input.current_tick",
        "std::memcpy(published, current",
        "current->nitro_activation_count = 0",
        "++current->race_tick",
        "std::numeric_limits<std::uint32_t>::max()",
        "has_selected_packet, true",
        "runtime=disabled return=-100 device_access=0",
    ):
        require(token in source, f"missing source contract: {token}")
    require("RecordingFrameV1" in header, "core is not bound to A9UTK1 frame ABI")

    forbidden_source = (
        "PTRACE_",
        "process_vm_",
        "/proc/",
        "socket(",
        "connect(",
        "pwrite(",
        "pread(",
        "RemoteCall",
        "NitroEnable",
    )
    for token in forbidden_source:
        require(token not in combined, f"forbidden runtime primitive: {token}")

    passive_header = run(str(readelf), "-h", str(passive))
    require("Machine:" in passive_header and "X86-64" in passive_header,
            "passive artifact is not Android x86_64 ELF")
    undefined = run(str(readelf), "-Ws", str(passive))
    for symbol in ("ptrace", "pread", "pwrite", "socket", "connect", "kill", "waitpid"):
        require(symbol not in undefined, f"forbidden passive import: {symbol}")

    symbols = run(str(readelf), "-Ws", str(selftest_object))
    require("a9tas_authoritative_tick_selftest_v1" in symbols,
            "selftest entry is missing")
    require("SelectOnNewTick" in symbols and "EndTick" in symbols,
            "review object does not retain both core functions")
    disassembly = run(str(objdump), "-d", "--demangle", str(selftest_object))
    require("SelectOnNewTick" in disassembly and "EndTick" in disassembly,
            "core functions missing from disassembly")

    print(
        "AUTH_TICK_CPP_POLICY passed=1 source_bound=1 a9utk1_bound=1 "
        "runtime=disabled device_access=0 game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"AUTH_TICK_CPP_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
