#!/usr/bin/env python3
"""Audit the pure post-natural BarrelRBX two-store certificate."""

from __future__ import annotations

import pathlib
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
HEADER = WORKSPACE / "android-port" / "src" / "barrel_rbx_two_store_transaction_v1.h"
SELFTEST = WORKSPACE / "android-port" / "src" / "barrel_rbx_two_store_transaction_selftest_v1.cpp"
UPSTREAM = WORKSPACE / "source" / "AluTasV2-main" / "AsphaltTool" / "dll" / "src" / "DetourFunctions.cpp"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 2:
        print("usage: policy SELFTEST READELF", file=sys.stderr)
        return 2
    selftest_artifact, readelf = map(pathlib.Path, args)
    for path in (selftest_artifact, readelf, HEADER, SELFTEST, UPSTREAM):
        require(path.is_file(), f"missing required file: {path}")
    header = HEADER.read_text(encoding="utf-8")
    selftest = SELFTEST.read_text(encoding="utf-8")
    upstream = UPSTREAM.read_text(encoding="utf-8")
    combined = header + "\n" + selftest
    for token in (
        "kFirstStoreRva = 0x369E058",
        "kSecondStoreRva = 0x369E068",
        "kCallerReturnRva = 0x369CC08",
        "kFirstFieldOffset = 0x1968",
        "kSecondFieldOffset = 0x196C",
        "enum class StoreIdentity",
        "StoreIdentity::kFirstField",
        "StoreIdentity::kSecondField",
        "struct StoreCertificate",
        "caller_return_count != 1",
        "kUnqualifiedCallStack",
        "Houdini data",
        "semantic::OnBarrelRollPostOriginal",
        "kFirstStoreCertified",
        "kUnexpectedStoreOrder",
        "kIncompletePair",
        "kSkipBarrelRbx",
        "first_store_certificate_bits",
        "authoritative_before_bits",
        "class RemotePairTransaction",
        "stopped_tid == context_.expected_tid",
        "((context.expected_owner + kFirstFieldOffset) & 7u) != 0",
        "io.read(io.context, pair_address_, before, sizeof(before))",
        "io.write(io.context, pair_address_, desired, sizeof(desired))",
        "std::memcmp(readback, desired, sizeof(readback))",
        "RemoteResult::kWriteFailedRolledBack",
        "RemoteResult::kMutationUncertain",
        "rollback_succeeded_",
    ):
        require(token in combined, f"missing two-store contract: {token}")
    roll_start = upstream.index("void REROUTE_FUNCTION Detour_BarrelRollStabilization")
    roll_end = upstream.index("bool SetupHook() noexcept", roll_start)
    roll = upstream[roll_start:roll_end]
    require(roll.index("RealBarrelRollStabilizationCall") <
            roll.index("m_value_rbx_2228") < roll.index("g_current_state"),
            "upstream BarrelRoll post-original order changed")
    for token in ("PTRACE_", "process_vm_", "/proc/", "pread(", "pwrite(",
                  "kill(", "waitpid(", "RemoteCall", "stunt_type",
                  "barrel_active"):
        require(token not in combined, f"pure RBX core gained forbidden primitive: {token}")
    elf = run(str(readelf), "-h", str(selftest_artifact))
    require("Machine:" in elf and "X86-64" in elf,
            "RBX selftest is not Android x86_64 ELF")
    rodata = run(str(readelf), "-p", ".rodata", str(selftest_artifact))
    require("BARREL_RBX_TWO_STORE_SELFTEST" in rodata,
            "RBX selftest receipt missing")
    print(
        "BARREL_RBX_TWO_STORE_POLICY passed=1 source_bound=1 post_original=1 "
        "exact_two_phase_watch_pair=1 second_stop_snapshot=1 unique_caller_marker=1 "
        "independent_skip=1 exact_8byte_transport=1 stopped_tid_binding=1 "
        "single_publish_readback=1 conditional_rollback=1 "
        "mutation_uncertain_fail_closed=1 alignment_guard=1 "
        "runtime=disabled device_access=0 game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"BARREL_RBX_TWO_STORE_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
