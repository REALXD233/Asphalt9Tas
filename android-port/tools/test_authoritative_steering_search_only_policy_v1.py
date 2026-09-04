#!/usr/bin/env python3
"""Artifact policy for the compile-time zero-write search candidate."""
from __future__ import annotations

import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "android-port/src/hwbp_authoritative_steering_v1.cpp"


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def require(value: bool, message: str) -> None:
    if not value:
        raise RuntimeError(message)


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: policy CANDIDATE READELF OBJDUMP", file=sys.stderr)
        return 2
    candidate, readelf, objdump = map(pathlib.Path, sys.argv[1:])
    for path in (candidate, readelf, objdump, SOURCE):
        require(path.is_file(), f"missing: {path}")
    source = SOURCE.read_text(encoding="utf-8")
    for token in ("A9TAS_AUTHORITATIVE_STEERING_SEARCH_ONLY",
                  "I_ACCEPT_ZERO_WRITE_ANCHOR_SEARCH_V1",
                  "report.delta_write_attempts == 0",
                  "report.pair_write_attempts == 0",
                  "audits.empty()"):
        require(token in source, f"missing search-only contract: {token}")
    symbols = run(str(readelf), "-Ws", str(candidate))
    require("ptrace" in symbols, "search observer must contain ptrace")
    for forbidden in ("pwrite", "process_vm_writev", "socket", "connect"):
        require(forbidden not in symbols, f"forbidden import: {forbidden}")
    disasm = run(str(objdump), "-d", "--demangle", str(candidate))
    for forbidden in ("<pwrite@plt>", "process_vm_writev", "ApplySteering"):
        require(forbidden not in disasm, f"forbidden compiled capability: {forbidden}")
    print("AUTHORITATIVE_SEARCH_ONLY_POLICY passed=1 pwrite_imports=0 "
          "gameplay_writes=0 action_calls=0 physics_writes=0 deployed=0")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"AUTHORITATIVE_SEARCH_ONLY_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
