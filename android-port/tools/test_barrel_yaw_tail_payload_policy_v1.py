#!/usr/bin/env python3
"""Audit the build-only ARM64 BarrelYaw late-boundary payload."""

from __future__ import annotations

import pathlib
import hashlib
import re
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
SOURCE = WORKSPACE / "android-port" / "src" / "payload_barrel_yaw_tail_v1.cpp"
PROTOCOL = WORKSPACE / "android-port" / "src" / "barrel_yaw_tail_payload_protocol_v1.h"
SEMANTIC = WORKSPACE / "android-port" / "src" / "barrel_stabilization_replay_core_v1.cpp"
RESOLVER = WORKSPACE / "android-port" / "src" / "barrel_yaw_tail_payload_elf_resolver_v1.h"


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 3:
        print("usage: policy PAYLOAD READELF OBJDUMP", file=sys.stderr)
        return 2
    payload, readelf, objdump = map(pathlib.Path, args)
    for path in (payload, readelf, objdump, SOURCE, PROTOCOL, SEMANTIC,
                 RESOLVER):
        require(path.is_file(), f"missing required file: {path}")

    source = SOURCE.read_text(encoding="utf-8")
    protocol = PROTOCOL.read_text(encoding="utf-8")
    resolver = RESOLVER.read_text(encoding="utf-8")
    combined = source + "\n" + protocol
    for token in (
        "kProtocolVersion = 2",
        "kPhysicsBackendVptrRva = 0x9d5ed40",
        "kOriginalBoundaryCallbackRva = 0x4cc5aac",
        "kShadowSize = 0x1c0",
        "kBoundarySlotOffset = 0x68",
        "kMaximumFrames = 3600",
        "kMaximumTransactionsPerFrame = 2",
        "active_token",
        "active_frame_index",
        "expected_first_caller_return",
        "expected_second_caller_return",
        "__builtin_return_address(0)",
        "RestoreOriginalVptrFromShadow",
        "__atomic_compare_exchange_n",
        "original(object, value, mode);",
        "if (!first_caller && !second_caller) return;",
        "if (call_count == 1)",
        "if (call_count != 2)",
        "__atomic_store_n(&g_evidence.active_call_count, 0u",
        "semantic::OnBarrelYawPostOriginal",
        "RestoreOriginalVptr",
        "kAuditTokenDisarmed",
        "kAuditVptrRestored",
        "__builtin_trap()",
        "kStatusRejectedBuildOnly",
        "a9tas_barrel_yaw_tail_arm_v1",
        "g_control.expected_tid",
    ):
        require(token in combined, f"missing payload contract: {token}")
    require(source.index("original(object, value, mode);") <
            source.index("semantic::OnBarrelYawPostOriginal"),
            "natural boundary method must run before replay tail")
    require(source.index("original(object, value, mode);") <
            source.index("if (!first_caller && !second_caller) return;"),
            "unrelated vslot calls must run naturally before passive return")
    require(source.index("if (!first_caller && !second_caller) return;") <
            source.index("if (call_count == 1)"),
            "unrelated vslot calls must not consume the transaction")
    success_restore = source.rindex("if (!RestoreOriginalVptrFromShadow())")
    success_disarm = source.index(
        "&g_control.active_token, protocol::kDisarmedToken", success_restore)
    require(success_restore < success_disarm,
            "success must restore original vptr before token disarm")

    forbidden = (
        "PTRACE_", "process_vm_", "/proc/", "pwrite(", "pread(",
        "RemoteCall", "Respawn", "NitroEnable", "stunt_type",
        "barrel_active", "flat_spin", "memcpy(live,",
        "persistent shadow",
    )
    for token in forbidden:
        require(token not in combined, f"forbidden installer/substitute primitive: {token}")

    header = run(str(readelf), "-h", str(payload))
    require("Machine:" in header and "AArch64" in header,
            "payload is not AArch64")
    symbols = run(str(readelf), "-Ws", str(payload))
    for symbol in (
        "a9tas_barrel_yaw_tail_boundary_v1",
        "a9tas_barrel_yaw_tail_arm_v1",
        "a9tas_barrel_yaw_tail_control_storage_data_v1",
        "a9tas_barrel_yaw_tail_target_storage_data_v1",
        "a9tas_barrel_yaw_tail_audit_storage_data_v1",
        "a9tas_barrel_yaw_tail_evidence_storage_data_v1",
    ):
        require(symbol in symbols, f"missing exported symbol: {symbol}")

    expected_hash_block = re.search(
        r"kExpectedSha256\[32\]\s*=\s*\{(.*?)\};", resolver, re.S)
    require(expected_hash_block is not None, "resolver SHA-256 pin missing")
    pinned_hash = bytes(int(value, 16) for value in re.findall(
        r"0x([0-9a-fA-F]{2})", expected_hash_block.group(1)))
    require(len(pinned_hash) == 32, "resolver SHA-256 pin length")
    require(pinned_hash == hashlib.sha256(payload.read_bytes()).digest(),
            "resolver SHA-256 pin does not match rebuilt payload")

    symbol_to_constant = {
        "a9tas_barrel_yaw_tail_boundary_v1": "kBoundaryRva",
        "a9tas_barrel_yaw_tail_shadow_storage_data_v1": "kShadowLocatorRva",
        "a9tas_barrel_yaw_tail_control_storage_data_v1": "kControlLocatorRva",
        "a9tas_barrel_yaw_tail_target_storage_data_v1": "kTargetsLocatorRva",
        "a9tas_barrel_yaw_tail_audit_storage_data_v1": "kAuditsLocatorRva",
        "a9tas_barrel_yaw_tail_evidence_storage_data_v1": "kEvidenceLocatorRva",
    }
    for symbol, constant in symbol_to_constant.items():
        symbol_match = re.search(
            rf"^\s*\d+:\s+([0-9a-fA-F]+)\s+\d+\s+\S+\s+GLOBAL\s+"
            rf"DEFAULT\s+\d+\s+{re.escape(symbol)}$", symbols, re.M)
        constant_match = re.search(
            rf"{constant}\s*=\s*0x([0-9a-fA-F]+)", resolver)
        require(symbol_match is not None, f"symbol RVA unavailable: {symbol}")
        require(constant_match is not None, f"resolver RVA missing: {constant}")
        require(int(symbol_match.group(1), 16) ==
                int(constant_match.group(1), 16),
                f"resolver RVA drift: {constant}")
    for forbidden_import in ("ptrace", "pread", "pwrite", "socket", "connect", "kill", "waitpid"):
        require(forbidden_import not in symbols,
                f"forbidden payload import: {forbidden_import}")

    disassembly = run(str(objdump), "-d", "--demangle", str(payload))
    require("a9tas_barrel_yaw_tail_boundary_v1" in disassembly,
            "boundary wrapper missing from disassembly")
    require("OnBarrelYawPostOriginal" in disassembly,
            "source-bound semantic tail missing from payload")

    print(
        "BARREL_YAW_TAIL_PAYLOAD_POLICY passed=1 arm64=1 build_only=1 "
        "natural_first=1 exact_lr_pair=1 transient_full_vtable=1 resolver_pinned=1 fatal_on_fault=1 "
        "second_boundary_call=1 source_core=1 installer=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"BARREL_YAW_TAIL_PAYLOAD_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
