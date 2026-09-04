#!/usr/bin/env python3
"""Offline policy gate for the disabled native-ARM64 initial loader."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/native_arm64_early_loader_v1.cpp"
CALL = ROOT / "src/native_arm64_remote_call_v1.cpp"
TRAP = ROOT / "src/native_arm64_immutable_trap_resolver_v1.cpp"
TX = ROOT / "src/native_arm64_loader_transaction_v1.h"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    source = SOURCE.read_text("utf-8")
    call = CALL.read_text("utf-8")
    trap = TRAP.read_text("utf-8")
    tx = TX.read_text("utf-8")
    joined = source + "\n" + call + "\n" + trap + "\n" + tx

    for forbidden in ("PTRACE_POKETEXT", "PTRACE_POKEDATA",
                      "FreezeStable(pid", "libhoudini", "libnb.so"):
        require(forbidden not in source, f"forbidden loader mechanism: {forbidden}")
    for token in (
        'ThreadName(pid,tid)=="Signal Catcher"',
        'dlsym(RTLD_DEFAULT,"dlopen")',
        "trap_resolver::Resolve(pid,&trap_report)",
        "transaction::IsBrkInstruction(instruction)",
        "remote_instruction!=instruction",
        'used_null_return=true',
        '"null-return-fallback"',
        "info.si_code == SEGV_MAPERR",
        "ReturnedAtExactTrap(returned, trap)",
        'constexpr char user_zero[]="/data/user/0/"',
        'constexpr char legacy[]="/data/data/"',
        "PayloadPathMatches(map.path,payload_path)",
        "call::CallStoppedThread(tid,call_origin,remote_dlopen",
        "ledger.stack_restored=WriteAndReadback",
        "ledger.registers_restored=call::SetRegistersExact(tid,original)",
        "!tx::DetachSafe(ledger)",
        "KillUncertain(pid)",
        "HasExactPayloadMap(ReadMaps(pid),payload_path,payload)",
        "argc==3 ? argv[2] : kPayloadPath",
        "ValidPayloadPath(payload_path)",
        'host_payload_path="/proc/"+std::to_string(pid)+"/root"+payload_path',
    ):
        require(token in joined, f"missing immutable-loader invariant: {token}")
    require(source.index("WaitForGame(argv[1],&pid,&start_ticks,&maps)") <
            source.index('host_payload_path="/proc/"'),
            "process-root payload fallback must bind to the newly started PID")
    require(source.index("ledger.stack_restored=WriteAndReadback") <
            source.index("ledger.registers_restored=call::SetRegistersExact"),
            "temporary stack must be restored before original SP is published")
    require(source.index("!tx::DetachSafe(ledger)") <
            source.index("PTRACE_DETACH"),
            "detach must follow full rollback proof")
    require("backend_enabled=0" not in source,
            "runtime source must not claim registry enablement")
    print("NATIVE_ARM64_EARLY_LOADER_POLICY passed=1 immutable_code=1 "
          "null_return_fallback=1 precise_transaction_receipt=1 "
          "single_signal_catcher=1 exact_payload=1 rollback_before_detach=1 "
          "nativebridge_dependency=0")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, OSError) as error:
        print(f"NATIVE_ARM64_EARLY_LOADER_POLICY passed=0 error={error}")
        raise SystemExit(1)
