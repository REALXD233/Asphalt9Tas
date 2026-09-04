#!/usr/bin/env python3
"""Audit the pure authoritative steering-pair transport core."""

from __future__ import annotations

import pathlib
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = WORKSPACE / "android-port" / "src" / "authoritative_steering_transport_v1.cpp"
HEADER = WORKSPACE / "android-port" / "src" / "authoritative_steering_transport_v1.h"


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
        "kSteeringOnlySkipMask",
        "frame.skip_override_flags != kSteeringOnlySkipMask",
        "static_cast<std::uint64_t>(plan.steering_bits) << 32",
        "live_pair_before & 0xffffffffULL",
        "live_pair_after != plan.pair_intended",
        "c98.steering_bits != c9c.steering_bits",
        "runtime=disabled",
        "device_access=0 game_writes=0",
    ):
        require(token in combined, f"missing steering contract: {token}")

    for token in (
        "PTRACE_",
        "process_vm_",
        "/proc/",
        "socket(",
        "connect(",
        "pwrite(",
        "pread(",
        "RemoteCall",
        "NitroEnable",
        "WriteExactVerified",
    ):
        require(token not in combined, f"forbidden runtime primitive: {token}")

    passive_header = run(str(readelf), "-h", str(passive))
    require("Machine:" in passive_header and "X86-64" in passive_header,
            "passive artifact is not Android x86_64 ELF")
    undefined = run(str(readelf), "-Ws", str(passive))
    for symbol in ("ptrace", "pread", "pwrite", "socket", "connect", "kill", "waitpid"):
        require(symbol not in undefined, f"forbidden passive import: {symbol}")

    symbols = run(str(readelf), "-Ws", str(selftest_object))
    require("a9tas_authoritative_steering_transport_selftest_v1" in symbols,
            "selftest entry is missing")
    disassembly = run(str(objdump), "-d", "--demangle", str(selftest_object))
    for token in ("FrameIsSteeringOnly", "PlanPair", "VerifyAppliedPair", "VerifyDualBoundary"):
        require(token in disassembly, f"transport function absent from disassembly: {token}")

    print(
        "AUTHORITATIVE_STEERING_TRANSPORT_CPP_POLICY passed=1 "
        "a9utk1_bound=1 pair_high32=steering pair_low32=preserved "
        "runtime=disabled device_access=0 game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"AUTHORITATIVE_STEERING_TRANSPORT_CPP_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)

