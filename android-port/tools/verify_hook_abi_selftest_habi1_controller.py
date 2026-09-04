#!/usr/bin/env python3
"""Offline capability and ELF verifier for the HABI-1 one-shot controller."""

from __future__ import annotations

import argparse
import ast
import json
import pathlib
import re
import sys

from verify_hook_abi_selftest_habi1 import (
    EM_X86_64,
    PF_R,
    PF_W,
    PF_X,
    PT_LOAD,
    Elf64,
    require,
    sha256,
)


FORBIDDEN_IMPORTS = {
    "dlopen", "dlclose", "dlsym", "dlerror", "mmap", "munmap", "mprotect",
    "process_vm_writev", "process_vm_readv",
}
REQUIRED_IMPORTS = {"ptrace", "waitpid", "pread", "kill", "lstat", "fstat"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--payload-source-file", type=pathlib.Path, required=True)
    parser.add_argument("--elf", type=pathlib.Path, required=True)
    parser.add_argument("--bootstrap-path", required=True)
    parser.add_argument("--bootstrap-sha256", required=True)
    parser.add_argument("--bootstrap-build-id", required=True)
    parser.add_argument("--payload-path", required=True)
    parser.add_argument("--payload-sha256", required=True)
    parser.add_argument("--payload-build-id", required=True)
    parser.add_argument("--payload-source-sha256", required=True)
    parser.add_argument("--payload-header-sha256", required=True)
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args()

    try:
        require(args.source.is_file() and args.payload_source_file.is_file() and
                args.elf.is_file(), "missing controller input")
        source = args.source.read_text(encoding="utf-8")
        payload_source = args.payload_source_file.read_text(encoding="utf-8")
        required_source_tokens = (
            "if (argc != 4)",
            "PID START_TICKS NONCE",
            "UniqueSignalCatcher",
            'ThreadName(pid, tid) == "Signal Catcher"',
            "ProcessStartTicks(pid)",
            "TracerPid(pid)",
            "ReadPinnedRegularFile(kPayloadPath, kPayloadSha256",
            "ReadPinnedRegularFile(kBootstrapPath, kBootstrapSha256",
            "ReadStableLocator",
            "locator.version != 2",
            "locator.size != sizeof(Locator)",
            "locator.return_trap_address",
            "locator.calibrate_tid_address",
            "PTRACE_GETSIGINFO",
            "info.si_code == SI_KERNEL || info.si_code == TRAP_BRKPT",
            "rollback_succeeded",
            "detach_safe",
            "KillUncertainProcess(pid)",
            "ParseReceipt(receipt,pid,tid,nonce)",
            "physics_token_gate_sha256",
            "guest_args[6] = {0,0,nonce,0,0,0}",
            "process_terminated=1",
        )
        for token in required_source_tokens:
            require(token in source, f"controller source token missing: {token}")
        forbidden_source_tokens = (
            '#include "injector.cpp"', "dlopen(", "dlsym(", "dlclose(",
            "mmap(", "munmap(", "process_vm_writev", "process_vm_readv",
            "PTRACE_POKETEXT", "FindInt3Stub", "RemoteSymbolByName",
        )
        for token in forbidden_source_tokens:
            require(token not in source, f"forbidden controller capability: {token}")
        require(source.count("RemoteCallOnce(") == 3,
                "controller must contain one helper plus calibration and one guest call")
        require(source.count("const std::uint64_t guest_args[6]") == 1,
                "guest call arguments must be constructed exactly once")
        require(source.count("PTRACE_ATTACH") == 1 and source.count("PTRACE_DETACH") == 1,
                "controller must have one attach/detach code path")

        format_start = payload_source.index('"protocol=hook-abi-selftest-habi1')
        format_end = payload_source.index("static_cast<int>(getpid())", format_start)
        literals = re.findall(r'"((?:\\.|[^"\\])*)"',
                              payload_source[format_start:format_end])
        receipt_format = "".join(ast.literal_eval(f'"{literal}"')
                                 for literal in literals)
        payload_keys = [line.split("=", 1)[0]
                        for line in receipt_format.splitlines() if "=" in line]
        require(len(payload_keys) == len(set(payload_keys)),
                "payload receipt contains duplicate keys")
        parser_start = source.index("bool ParseReceipt(")
        parser_end = source.index("bool IsAlive(", parser_start)
        parser_literals = set(re.findall(r'"([a-z][a-z0-9_]*)"',
                                         source[parser_start:parser_end]))
        controller_keys = set(payload_keys) & parser_literals
        require(controller_keys == set(payload_keys),
                f"controller receipt schema mismatch: {sorted(set(payload_keys) - controller_keys)}")

        expected_name = (
            "a9tas_habi1_one_shot_controller_"
            f"{args.payload_source_sha256[:16]}"
        )
        require(args.elf.name == expected_name,
                "controller filename is not payload-source-bound")
        elf = Elf64(args.elf, expected_machine=EM_X86_64)
        loads = [program for program in elf.programs if program.type == PT_LOAD]
        require(loads and all((p.flags & (PF_W | PF_X)) != (PF_W | PF_X)
                              for p in loads), "controller has W+X PT_LOAD")
        require(any(p.flags == (PF_R | PF_X) for p in loads),
                "controller R-X PT_LOAD missing")
        stack = [p for p in elf.programs if p.type == 0x6474E551]
        require(len(stack) == 1 and stack[0].flags == (PF_R | PF_W),
                "controller GNU_STACK must be RW")

        dynsym = elf.symbols(".dynsym")
        imports = {
            symbol.name.split("@", 1)[0] for symbol in dynsym
            if symbol.name and symbol.section_index == 0 and symbol.binding in (1, 2)
        }
        require(not (imports & FORBIDDEN_IMPORTS),
                f"controller imports forbidden capability: {sorted(imports & FORBIDDEN_IMPORTS)}")
        require(REQUIRED_IMPORTS <= imports,
                f"controller missing required imports: {sorted(REQUIRED_IMPORTS - imports)}")
        defined_default = {
            symbol.name for symbol in dynsym
            if symbol.name and symbol.section_index != 0 and
            symbol.binding in (1, 2) and symbol.visibility == 0
        }
        require(not defined_default, f"controller unexpectedly exports symbols: {defined_default}")

        raw = elf.data
        identity_tokens = (
            args.bootstrap_path, args.bootstrap_sha256, args.bootstrap_build_id,
            args.payload_path, args.payload_sha256, args.payload_build_id,
            args.payload_source_sha256, args.payload_header_sha256,
            "/data/user/0/com.aligames.kuang.kybc.aligames/cache/"
            "a9tas-hook-abi-selftest-habi1.status",
            "a9tas_bootstrap_hook_abi_selftest_habi1_locator",
            "a9tas_hook_abi_selftest_habi1_run",
            "Signal Catcher",
        )
        for token in identity_tokens:
            require(raw.count(token.encode()) >= 1,
                    f"controller embedded identity missing: {token}")
        require(b"UNSET" not in raw, "controller contains UNSET identity")

        report = {
            "protocol": "hook-abi-selftest-habi1-controller",
            "revision": 1,
            "source_sha256": sha256(args.source),
            "payload_receipt_key_count": len(payload_keys),
            "elf_sha256": sha256(args.elf),
            "build_id": elf.build_id(),
            "bootstrap_path": args.bootstrap_path,
            "bootstrap_sha256": args.bootstrap_sha256,
            "bootstrap_build_id": args.bootstrap_build_id,
            "payload_path": args.payload_path,
            "payload_sha256": args.payload_sha256,
            "payload_build_id": args.payload_build_id,
            "payload_source_sha256": args.payload_source_sha256,
            "payload_header_sha256": args.payload_header_sha256,
            "cli": ["PID", "START_TICKS", "NONCE"],
            "guest_calls": 1,
            "arbitrary_path_arguments": 0,
            "arbitrary_symbol_arguments": 0,
            "arbitrary_address_arguments": 0,
            "remote_dlopen_capability": 0,
            "fixed_bootstrap_trap": 1,
            "strict_receipt": 1,
            "rollback_required": 1,
            "success_terminates_disposable_process": 1,
            "device_access": 0,
            "deployed": 0,
            "dynamic": "NOT_RUN",
            "passed": 1,
        }
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                   encoding="utf-8", newline="\n")
        print(
            "HABI1_CONTROLLER_POLICY passed=1 guest_calls=1 arbitrary_paths=0 "
            f"elf_sha256={report['elf_sha256']} build_id={report['build_id']} "
            "device_access=0 deployed=0 dynamic=NOT_RUN"
        )
        return 0
    except (OSError, ValueError) as error:
        print(f"HABI1_CONTROLLER_POLICY passed=0 error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
