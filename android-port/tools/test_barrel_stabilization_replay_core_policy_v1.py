#!/usr/bin/env python3
"""Audit the build-only AluTasV2 barrel stabilization replay tail."""

from __future__ import annotations

import pathlib
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = WORKSPACE / "android-port" / "src" / "barrel_stabilization_replay_core_v1.cpp"
HEADER = WORKSPACE / "android-port" / "src" / "barrel_stabilization_replay_core_v1.h"
UPSTREAM = WORKSPACE / "source" / "AluTasV2-main" / "AsphaltTool" / "dll" / "src" / "DetourFunctions.cpp"


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
    for path in (passive, selftest_object, readelf, objdump, SOURCE, HEADER, UPSTREAM):
        require(path.is_file(), f"missing required file: {path}")

    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    upstream = UPSTREAM.read_text(encoding="utf-8")
    combined = source + "\n" + header

    # Bind the model to the literal two independent upstream detours and their
    # call-first, optional-override, capture-last ordering.
    roll = upstream[upstream.index("void REROUTE_FUNCTION Detour_BarrelRollStabilization"):
                    upstream.index("bool SetupHook() noexcept", upstream.index("void REROUTE_FUNCTION Detour_BarrelRollStabilization"))]
    yaw = upstream[upstream.index("void REROUTE_FUNCTION Detour_BarrelYawStabilization"):
                   upstream.index("bool SetupHook() noexcept", upstream.index("void REROUTE_FUNCTION Detour_BarrelYawStabilization"))]
    require(roll.index("RealBarrelRollStabilizationCall") < roll.index("BARREL_RBX") < roll.index("g_current_state"),
            "upstream roll ordering changed")
    require(yaw.index("RealBarrelYawStabilizationCall") < yaw.index("BARREL_ANGULAR") < yaw.index("g_current_state"),
            "upstream yaw ordering changed")

    for token in (
        "OnBarrelRollPostOriginal",
        "OnBarrelYawPostOriginal",
        "unified_tick_v1::kSkipBarrelRbx",
        "unified_tick_v1::kSkipBarrelAngular",
        "CopyBits(live_bits, target_bits)",
        "CopyBits(captured_bits, live_bits)",
        "!SamePermit(active.permit, context.expected_permit)",
        "context.original_returned",
        "BARREL_STABILIZATION_REPLAY_BUILD_ONLY runtime=disabled",
    ):
        require(token in combined, f"missing replay-tail contract: {token}")

    forbidden = (
        "PTRACE_", "process_vm_", "/proc/", "socket(", "connect(",
        "pwrite(", "pread(", "RemoteCall", "NitroEnable", "Respawn",
        "stunt_type", "barrel_active", "flat_spin", "std::fabs",
        "std::abs(",
    )
    for token in forbidden:
        require(token not in combined, f"forbidden transport/substitute primitive: {token}")

    passive_header = run(str(readelf), "-h", str(passive))
    require("Machine:" in passive_header and "X86-64" in passive_header,
            "passive artifact is not Android x86_64 ELF")
    undefined = run(str(readelf), "-Ws", str(passive))
    for symbol in ("ptrace", "pread", "pwrite", "socket", "connect", "kill", "waitpid"):
        require(symbol not in undefined, f"forbidden passive import: {symbol}")

    symbols = run(str(readelf), "-Ws", str(selftest_object))
    require("a9tas_barrel_stabilization_replay_selftest_v1" in symbols,
            "selftest entry is missing")
    disassembly = run(str(objdump), "-d", "--demangle", str(selftest_object))
    require("OnBarrelRollPostOriginal" in disassembly and
            "OnBarrelYawPostOriginal" in disassembly,
            "both replay tails must remain reviewable")

    print(
        "BARREL_STABILIZATION_REPLAY_POLICY passed=1 source_bound=1 "
        "post_original=1 independent_skip=1 multi_call=1 runtime=disabled "
        "device_access=0 game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"BARREL_STABILIZATION_REPLAY_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
