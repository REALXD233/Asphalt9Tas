#!/usr/bin/env python3
"""Static identity and ABI verifier for the G2 single-function hook."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


EXPECTED_EXPORTS = {
    "a9tas_g2_physics_interval_command_v1",
    "a9tas_g2_physics_interval_control_data_v1",
    "a9tas_g2_physics_interval_control_v1",
    "a9tas_g2_physics_interval_events_data_v1",
    "a9tas_g2_physics_interval_events_v1",
    "a9tas_g2_physics_interval_evidence_data_v1",
    "a9tas_g2_physics_interval_evidence_v1",
    "a9tas_g2_physics_interval_expected_path_v1",
    "a9tas_g2_physics_interval_game_sha256_v1",
    "a9tas_g2_physics_interval_protocol_v1",
    "a9tas_g2_physics_interval_source_sha256_v1",
    "a9tas_g2_physics_interval_wrapper_data_v1",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def function_body(text: str, name: str) -> str:
    match = re.search(
        rf"^[0-9a-f]+ <{re.escape(name)}>:\n(.*?)(?=^[0-9a-f]+ <|\Z)",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if not match:
        raise ValueError(f"missing function {name}")
    return match.group(1)


def require_order(body: str, fragments: tuple[str, ...]) -> None:
    cursor = 0
    for fragment in fragments:
        position = body.find(fragment, cursor)
        if position < 0:
            raise ValueError(f"missing/out-of-order instruction: {fragment}")
        cursor = position + len(fragment)


def verify_source(header: str, source: str) -> None:
    joined = header + "\n" + source
    required = (
        'kGameBuildId =',
        'e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b',
        '671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0',
        'kTargetRva = 0x3695474',
        'kPatchSize = 16',
        '0xff, 0xc3, 0x00, 0xd1',
        'G2PhysicsIntervalOriginalTrampolineV1',
        'G2PhysicsIntervalEntryV1',
        'G2PhysicsIntervalAfterOriginalV1',
        'kTargetPageIsolated',
        'ControlConfigured()',
        'TargetInExecutableLoad(image, target)',
        'PrivateWritableNoExec',
        '__builtin___clear_cache',
        'a9tas_g2_physics_interval_command_v1',
        'installer=explicit-only',
    )
    forbidden = (
        'ptrace(',
        'pwrite(',
        'process_vm_writev',
        'pthread_create(',
        'std::thread',
        '__atomic_store_n(output',
        'memcpy(output',
        '*output =',
    )
    missing = [item for item in required if item not in joined]
    present = [item for item in forbidden if item in joined]
    if missing:
        raise ValueError(f"missing source marker: {missing[0]}")
    if present:
        raise ValueError(f"forbidden source marker: {present[0]}")
    onload = re.search(r"void OnLoad\(\) \{(.*?)\n\}", source, re.DOTALL)
    if not onload or "InstallAndArm" in onload.group(1):
        raise ValueError("constructor must remain passive")


def verify_abi(disassembly: str) -> None:
    trampoline = function_body(disassembly, "G2PhysicsIntervalOriginalTrampolineV1")
    require_order(trampoline, (
        "sub\tsp, sp, #0x30",
        "stp\tx21, x20, [sp, #0x10]",
        "stp\tx19, x30, [sp, #0x20]",
        "mov\tw21, #0x8889",
        "adr\tx17,",
        "ldr\tx17, [x17]",
        "br\tx17",
    ))
    if re.search(r"\bblr?\b", trampoline):
        raise ValueError("trampoline must tail-branch after relocated prologue")

    entry = function_body(disassembly, "G2PhysicsIntervalEntryV1")
    require_order(entry, (
        "sub\tsp, sp, #0x160",
        "stp\tx19, x20, [sp]",
        "str\tx30, [sp, #0x10]",
        "mov\tx19, x0",
        "mov\tx20, x8",
        "G2PhysicsIntervalOriginalTrampolineV1",
        "stp\tx0, x1, [sp, #0x20]",
        "stp\tx2, x3, [sp, #0x30]",
        "stp\tx4, x5, [sp, #0x40]",
        "stp\tx6, x7, [sp, #0x50]",
        "stp\tx8, x9, [sp, #0x60]",
        "stp\tx10, x11, [sp, #0x70]",
        "stp\tx12, x13, [sp, #0x80]",
        "stp\tx14, x15, [sp, #0x90]",
        "stp\tx16, x17, [sp, #0xa0]",
        "str\tx18, [sp, #0xb0]",
        "stp\tq0, q1, [sp, #0xc0]",
        "stp\tq2, q3, [sp, #0xe0]",
        "stp\tq4, q5, [sp, #0x100]",
        "stp\tq6, q7, [sp, #0x120]",
        "mrs\tx9, NZCV",
        "mrs\tx10, FPCR",
        "stp\tx9, x10, [sp, #0x140]",
        "mrs\tx9, FPSR",
        "str\tx9, [sp, #0x150]",
        "mov\tx0, x19",
        "mov\tx1, x20",
        "G2PhysicsIntervalAfterOriginalV1",
        "ldr\tx9, [sp, #0x150]",
        "msr\tFPSR, x9",
        "ldp\tx9, x10, [sp, #0x140]",
        "msr\tNZCV, x9",
        "msr\tFPCR, x10",
        "ldp\tq6, q7, [sp, #0x120]",
        "ldp\tq4, q5, [sp, #0x100]",
        "ldp\tq2, q3, [sp, #0xe0]",
        "ldp\tq0, q1, [sp, #0xc0]",
        "ldr\tx18, [sp, #0xb0]",
        "ldp\tx16, x17, [sp, #0xa0]",
        "ldp\tx14, x15, [sp, #0x90]",
        "ldp\tx12, x13, [sp, #0x80]",
        "ldp\tx10, x11, [sp, #0x70]",
        "ldp\tx8, x9, [sp, #0x60]",
        "ldp\tx6, x7, [sp, #0x50]",
        "ldp\tx4, x5, [sp, #0x40]",
        "ldp\tx2, x3, [sp, #0x30]",
        "ldp\tx0, x1, [sp, #0x20]",
        "ldr\tx30, [sp, #0x10]",
        "ldp\tx19, x20, [sp]",
        "add\tsp, sp, #0x160",
        "ret",
    ))
    if len(re.findall(r"\bbl\t", entry)) != 2:
        raise ValueError("entry must contain exactly original+observer calls")


def run_text(command: list[str]) -> str:
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    return completed.stdout


def verify_elf(elf: Path, objdump: Path, readelf: Path, nm: Path,
               source_sha: str, device_path: str) -> dict[str, object]:
    header = run_text([str(readelf), "-h", "-n", str(elf)])
    if "Machine:                           AArch64" not in header or \
       "Type:                              DYN (Shared object file)" not in header:
        raise ValueError("payload ELF identity mismatch")
    build = re.search(r"Build ID: ([0-9a-f]{40})", header)
    if not build:
        raise ValueError("missing SHA1 build-id")
    dynamic = run_text([str(nm), "-D", "--defined-only", str(elf)])
    exports = {line.split()[-1] for line in dynamic.splitlines() if line.split()}
    if exports != EXPECTED_EXPORTS:
        raise ValueError(f"unexpected project exports: {sorted(exports ^ EXPECTED_EXPORTS)}")
    undefined = run_text([str(nm), "-u", str(elf)])
    for forbidden in ("ptrace", "pwrite", "process_vm_writev", "pthread_create"):
        if forbidden in undefined:
            raise ValueError(f"forbidden import: {forbidden}")
    raw = elf.read_bytes()
    for literal in (source_sha, device_path,
                    "671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0"):
        if literal.encode() not in raw:
            raise ValueError(f"missing embedded identity: {literal}")
    disassembly = run_text([str(objdump), "-d", "--demangle",
                            "--no-show-raw-insn", str(elf)])
    verify_abi(disassembly)
    return {"build_id": build.group(1), "disassembly": disassembly}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--objdump", type=Path, required=True)
    parser.add_argument("--readelf", type=Path, required=True)
    parser.add_argument("--nm", type=Path, required=True)
    parser.add_argument("--expected-device-path", required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    try:
        header_text = args.header.read_text(encoding="utf-8")
        source_text = args.source.read_text(encoding="utf-8")
        verify_source(header_text, source_text)
        source_hash = sha256(args.source)
        header_hash = sha256(args.header)
        elf_result = verify_elf(args.elf, args.objdump, args.readelf, args.nm,
                                source_hash, args.expected_device_path)
        report = {
            "schema": "a9tas-g2-physics-interval-passthrough-policy-v1",
            "passed": 1,
            "source_sha256": source_hash,
            "header_sha256": header_hash,
            "elf_sha256": sha256(args.elf),
            "build_id": elf_result["build_id"],
            "expected_device_path": args.expected_device_path,
            "game_sha256": "671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0",
            "game_build_id": "e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b",
            "target_rva": "0x3695474",
            "patch_size": 16,
            "original_calls_per_entry": 1,
            "observer_calls_per_entry": 1,
            "output_writes": 0,
            "volatile_integer_state_restored": 1,
            "volatile_vector_state_restored": 1,
            "nzcv_fpcr_fpsr_restored": 1,
            "constructor_passive": 1,
        }
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        print(f"G2_PHYSICS_INTERVAL_POLICY passed=0 reason={exc}")
        return 1
    print("G2_PHYSICS_INTERVAL_POLICY passed=1 original_once=1 output_writes=0 abi_restored=1")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
