#!/usr/bin/env python3
"""Offline identity and capability verifier for the fixed HABI-1 early carrier."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

from verify_hook_abi_selftest_habi1 import (
    EM_X86_64, PF_R, PF_W, PF_X, PT_LOAD, Elf64, require, sha256,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--controller-source", type=pathlib.Path, required=True)
    parser.add_argument("--elf", type=pathlib.Path, required=True)
    parser.add_argument("--bootstrap-path", required=True)
    parser.add_argument("--bootstrap-sha256", required=True)
    parser.add_argument("--bootstrap-build-id", required=True)
    parser.add_argument("--payload-path", required=True)
    parser.add_argument("--payload-sha256", required=True)
    parser.add_argument("--payload-build-id", required=True)
    parser.add_argument("--payload-source-sha256", required=True)
    parser.add_argument("--libc-file", type=pathlib.Path, required=True)
    parser.add_argument("--libc-device-path", required=True)
    parser.add_argument("--libc-sha256", required=True)
    parser.add_argument("--libc-build-id", required=True)
    parser.add_argument("--libc-trap-rva", required=True)
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args()

    try:
        require(args.source.is_file() and args.controller_source.is_file() and
                args.elf.is_file() and args.libc_file.is_file(),
                "missing carrier input")
        source = args.source.read_text(encoding="utf-8")
        core = args.controller_source.read_text(encoding="utf-8")
        required = (
            '#define A9TAS_HABI1_CONTROLLER_CORE_ONLY 1',
            '#include "habi1_one_shot_controller.cpp"',
            "argc < 1 || argc > 2", "WaitForFixedPreloadWindow",
            "ValidGameProcessName", "FindExactGameProcess",
            '"com.aligames.kuang.kybc.aligames"', "argv[1]",
            "UniqueSignalCatcher(pid)", "ProcessStartTicks(pid)",
            "TracerPid(pid)", "kRequiredBridgePath",
            '"/system/lib64/libnb.so"', '"libAsphalt9.so"',
            "!HasPath(candidate_maps,kBootstrapPath)",
            "ReadPinnedRegularFile(kBootstrapPath, kBootstrapSha256",
            "ReadPinnedRegularFile(kPayloadPath, kPayloadSha256",
            "ReadPinnedRegularFile(kCarrierLibcPath, kCarrierLibcSha256",
            "kCarrierLibcTrapRva >= libc_file.bytes.size()",
            "libc_file.bytes[kCarrierLibcTrapRva] != 0xcc",
            'ResolveRemoteFixedSymbol(maps,"gettid"',
            'ResolveRemoteFixedSymbol(maps,"mmap"',
            'ResolveRemoteFixedSymbol(maps,"munmap"',
            'ResolveRemoteFixedSymbol(maps,"dlopen"',
            "PTRACE_ATTACH", "PTRACE_DETACH", "KillUncertainProcess(pid)",
            "stage != 2 && stage != 5", "ReadStableLocator",
            "rollback=1 detach=1",
        )
        for token in required:
            require(token in source, f"carrier source token missing: {token}")
        for token in ('#include "injector.cpp"', "process_vm_writev",
                      "process_vm_readv", "PTRACE_POKETEXT", "FindInt3Stub",
                      "RemoteSymbolByName", "argv[2]", "argv[3]",
                      "argv[4]", "argv[5]"):
            require(token not in source, f"forbidden carrier capability: {token}")
        require(source.count("RemoteCallOnce(") == 4,
                "carrier must contain calibration, mmap, dlopen and munmap call sites")
        require(source.count("PTRACE_ATTACH") == 1 and
                source.count("PTRACE_DETACH") == 1,
                "carrier must have one attach/detach path")
        require("#ifndef A9TAS_HABI1_CONTROLLER_CORE_ONLY" in core,
                "controller core-only build guard missing")

        expected_name = "a9tas_habi1_early_carrier_" + args.payload_source_sha256[:16]
        require(args.elf.name == expected_name,
                "carrier filename is not payload-identity-bound")
        require(sha256(args.libc_file) == args.libc_sha256,
                "pinned libc evidence SHA mismatch")
        trap_rva = int(args.libc_trap_rva, 0)
        libc_bytes = args.libc_file.read_bytes()
        require(0 <= trap_rva < len(libc_bytes) and libc_bytes[trap_rva] == 0xCC,
                "pinned libc trap byte mismatch")

        elf = Elf64(args.elf, expected_machine=EM_X86_64)
        loads = [program for program in elf.programs if program.type == PT_LOAD]
        require(loads and all((p.flags & (PF_W | PF_X)) != (PF_W | PF_X)
                              for p in loads), "carrier has W+X PT_LOAD")
        require(any(p.flags == (PF_R | PF_X) for p in loads),
                "carrier R-X PT_LOAD missing")
        stack = [p for p in elf.programs if p.type == 0x6474E551]
        require(len(stack) == 1 and stack[0].flags == (PF_R | PF_W),
                "carrier GNU_STACK must be RW")
        imports = {
            symbol.name.split("@", 1)[0] for symbol in elf.symbols(".dynsym")
            if symbol.name and symbol.section_index == 0 and symbol.binding in (1, 2)
        }
        require({"ptrace", "waitpid", "dlsym", "lstat", "fstat", "kill"} <= imports,
                "carrier required imports missing")
        require(not ({"process_vm_writev", "process_vm_readv", "mprotect"} & imports),
                "carrier imports forbidden capability")

        embedded = (
            args.bootstrap_path, args.bootstrap_sha256, args.bootstrap_build_id,
            args.payload_path, args.payload_sha256, args.payload_build_id,
            args.payload_source_sha256,
            args.libc_device_path, args.libc_sha256, args.libc_build_id,
            "/system/lib64/libnb.so", "libAsphalt9.so", "Signal Catcher",
        )
        for token in embedded:
            require(elf.data.count(token.encode()) >= 1,
                    f"carrier embedded identity missing: {token}")
        require(b"UNSET" not in elf.data, "carrier contains UNSET identity")

        report = {
            "protocol": "hook-abi-selftest-habi1-early-carrier",
            "revision": 1,
            "source_sha256": sha256(args.source),
            "controller_core_sha256": sha256(args.controller_source),
            "elf_sha256": sha256(args.elf),
            "build_id": elf.build_id(),
            "bootstrap_path": args.bootstrap_path,
            "bootstrap_sha256": args.bootstrap_sha256,
            "bootstrap_build_id": args.bootstrap_build_id,
            "payload_path": args.payload_path,
            "payload_sha256": args.payload_sha256,
            "payload_build_id": args.payload_build_id,
            "payload_source_sha256": args.payload_source_sha256,
            "libc_device_path": args.libc_device_path,
            "libc_sha256": args.libc_sha256,
            "libc_build_id": args.libc_build_id,
            "libc_trap_rva": hex(trap_rva),
            "cli": [],
            "arbitrary_path_arguments": 0,
            "arbitrary_symbol_arguments": 0,
            "arbitrary_address_arguments": 0,
            "fixed_bootstrap_loads": 1,
            "fixed_signal_catcher": 1,
            "rollback_required": 1,
            "disposable_process_only": 1,
            "device_access": 0,
            "deployed": 0,
            "dynamic": "NOT_RUN",
            "passed": 1,
        }
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n",
                                   encoding="utf-8", newline="\n")
        print("HABI1_CARRIER_POLICY passed=1 fixed_bootstrap_loads=1 "
              f"elf_sha256={report['elf_sha256']} build_id={report['build_id']} "
              "device_access=0 deployed=0 dynamic=NOT_RUN")
        return 0
    except (OSError, ValueError) as error:
        print(f"HABI1_CARRIER_POLICY passed=0 error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
