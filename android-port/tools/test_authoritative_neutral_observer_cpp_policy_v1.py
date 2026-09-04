#!/usr/bin/env python3
"""Artifact policy for the write-neutral authoritative HWBP observer."""

from __future__ import annotations

import pathlib
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (
    WORKSPACE
    / "android-port"
    / "src"
    / "hwbp_authoritative_neutral_observer_v1.cpp"
)


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
    for token in (
        '#include "hwbp_pipeline_order_observer_v1.cpp"',
        '#include "authoritative_unified_adapter_v1.cpp"',
        "open(mem_path, O_RDONLY | O_CLOEXEC)",
        "kNeutralFrameCount = 5",
        "kSupportedSkipMask",
        "target_memory_write_attempts == 0",
        "gameplay_action_calls == 0",
        "BoundaryDr7(true)",
        "PipelineDr7()",
    ):
        require(token in source, f"missing neutral contract: {token}")
    for token in (
        "O_RDWR",
        "pwrite(",
        "WriteExactVerified",
        "process_vm_writev",
        "PTRACE_POKEDATA",
        "PTRACE_POKETEXT",
        "RemoteCall",
        "NitroEnable",
    ):
        require(token not in source, f"forbidden target mutation primitive: {token}")

    elf_header = run(str(readelf), "-h", str(candidate))
    require("Machine:" in elf_header and "X86-64" in elf_header,
            "candidate is not Android x86_64 ELF")
    symbols = run(str(readelf), "-Ws", str(candidate))
    for symbol in ("pwrite", "process_vm_writev", "socket", "connect"):
        require(symbol not in symbols, f"forbidden candidate import: {symbol}")
    for symbol in ("ptrace", "pread", "waitpid"):
        require(symbol in symbols, f"required observer import missing: {symbol}")

    object_symbols = run(str(readelf), "-Ws", str(review_object))
    require(" main" in object_symbols, "candidate main missing")
    disassembly = run(str(objdump), "-d", "--demangle", str(review_object))
    for name in ("ProgramStoppedThreadNeutral", "AdvanceAdapter", "main"):
        require(name in disassembly, f"candidate machine code missing: {name}")

    print(
        "AUTHORITATIVE_NEUTRAL_OBSERVER_CPP_POLICY passed=1 frames=5 "
        "target_mem=readonly gameplay_writes=0 action_calls=0 deployed=0 "
        "device_access=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(
            f"AUTHORITATIVE_NEUTRAL_OBSERVER_CPP_POLICY passed=0 error={error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
