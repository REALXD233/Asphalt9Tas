#!/usr/bin/env python3
"""Artifact policy for the authoritative fixed-delta-only candidate."""

from __future__ import annotations

import pathlib
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = WORKSPACE / "android-port" / "src" / "hwbp_authoritative_fixed_delta_v1.cpp"


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 4:
        print("usage: policy CANDIDATE OBJECT READELF OBJDUMP", file=sys.stderr)
        return 2
    candidate, review_object, readelf, objdump = map(pathlib.Path, args)
    for path in (candidate, review_object, readelf, objdump, SOURCE):
        require(path.is_file(), f"missing required file: {path}")
    source = SOURCE.read_text(encoding="utf-8")
    require(source.count("pwrite(") == 1, "source must contain one pwrite primitive")
    require(source.count("WriteFixedDeltaVerified(") == 2,
            "fixed-delta helper must have one definition and one call")
    for token in (
        "open(mem_path, O_RDWR | O_CLOEXEC)",
        "pwrite(mem, &fixed_delta_us, sizeof(fixed_delta_us)",
        "mem, delta_address",
        "verify == fixed_delta_us",
        "kSupportedSkipMask",
        "report.delta_writes == kFrameCount",
        "report.gameplay_action_calls == 0",
    ):
        require(token in source, f"missing fixed-delta contract: {token}")
    for token in (
        "WriteExactVerified", "process_vm_writev", "PTRACE_POKEDATA",
        "PTRACE_POKETEXT", "RemoteCall", "NitroEnable", "ApplySteering",
    ):
        require(token not in source, f"forbidden capability: {token}")

    header = run(str(readelf), "-h", str(candidate))
    require("Machine:" in header and "X86-64" in header,
            "candidate is not Android x86_64 ELF")
    symbols = run(str(readelf), "-Ws", str(candidate))
    require("pwrite" in symbols, "required pwrite import missing")
    for symbol in ("process_vm_writev", "socket", "connect"):
        require(symbol not in symbols, f"forbidden import: {symbol}")
    disassembly = run(str(objdump), "-d", "--demangle", str(candidate))
    require(disassembly.count("<pwrite@plt>") == 2,
            "expected one pwrite call plus one PLT label")
    require("WriteFixedDeltaVerified" in disassembly,
            "narrow fixed-delta helper missing")
    object_symbols = run(str(readelf), "-Ws", str(review_object))
    require(" main" in object_symbols, "candidate main missing")
    print(
        "AUTHORITATIVE_FIXED_DELTA_CPP_POLICY passed=1 frames=5 "
        "delta_write_primitive=1 delta_callsite=1 all_other_capabilities=skipped "
        "deployed=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"AUTHORITATIVE_FIXED_DELTA_CPP_POLICY passed=0 error={error}",
              file=sys.stderr)
        raise SystemExit(1)
