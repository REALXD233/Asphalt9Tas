#!/usr/bin/env python3
"""Offline verifier for the fixed x86_64 HABI-1 NativeBridge carrier."""

from __future__ import annotations

import argparse
import json
import pathlib
import struct
import sys

from verify_hook_abi_selftest_habi1 import (
    EM_X86_64,
    PF_R,
    PF_W,
    PF_X,
    PROJECT_EXPORTS,
    PT_LOAD,
    RELA,
    SHT_RELA,
    Elf64,
    require,
    sha256,
)


LOCATOR_MAGIC = 0x48414249314C4F43
LOCATOR_VERSION = 2
LOCATOR_SIZE = 512
LOCATOR_SYMBOL = "a9tas_bootstrap_hook_abi_selftest_habi1_locator"
EXPECTED_EXPORTS = {
    "a9tas_bootstrap_status",
    "a9tas_bootstrap_stage",
    "a9tas_bootstrap_same_thread_probe_status",
    "a9tas_bootstrap_same_thread_probe_trampoline",
    "a9tas_bootstrap_hook_abi_selftest_habi1_return_trap",
    "a9tas_bootstrap_hook_abi_selftest_habi1_calibrate_tid",
    LOCATOR_SYMBOL,
}
EXPECTED_IMPORTS = {
    "__cxa_finalize", "__cxa_atexit", "__register_atfork", "getpid",
    "__android_log_print", "dl_iterate_phdr", "fopen", "memset", "fgets",
    "strstr", "fclose", "dlopen", "dlsym", "sysconf", "mprotect",
    "pthread_create", "pthread_detach", "usleep", "syscall", "sscanf",
}


def fixed_string(blob: bytes, offset: int, size: int) -> str:
    field = blob[offset:offset + size]
    end = field.find(b"\0")
    require(end >= 0, f"locator string at {offset} is not terminated")
    require(all(byte == 0 for byte in field[end + 1:]),
            f"locator string at {offset} has nonzero tail")
    return field[:end].decode("ascii", "strict")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--bootstrap-core", type=pathlib.Path, required=True)
    parser.add_argument("--elf", type=pathlib.Path, required=True)
    parser.add_argument("--payload-sha256", required=True)
    parser.add_argument("--payload-build-id", required=True)
    parser.add_argument("--payload-source-sha256", required=True)
    parser.add_argument("--payload-device-path", required=True)
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args()

    try:
        for path in (args.source, args.bootstrap_core, args.elf):
            require(path.is_file(), f"missing input: {path}")
        source_text = args.source.read_text(encoding="utf-8")
        core_text = args.bootstrap_core.read_text(encoding="utf-8")
        for token in (
            "#define A9TAS_ENABLE_SAME_THREAD_PROBE 1",
            "#define A9TAS_ENABLE_UNSAFE_LATE_ARM_LOAD 0",
            '"a9tas_hook_abi_selftest_habi1_run"',
            'A9TAS_SAME_THREAD_PROBE_SHORTY "JJ"',
            "Habi1BootstrapLocator",
            "a9tas_bootstrap_hook_abi_selftest_habi1_return_trap",
            "a9tas_bootstrap_hook_abi_selftest_habi1_calibrate_tid",
            "static_assert(sizeof(Habi1BootstrapLocator) == 512)",
        ):
            require(token in source_text, f"bootstrap source token missing: {token}")
        require("A9TAS_ENABLE_UNSAFE_LATE_ARM_LOAD 1" not in source_text,
                "unsafe late ARM load must remain disabled")
        require("RestoreNativeBridgeCallbacks" in core_text,
                "carrier must restore NativeBridge callbacks before nested load")

        expected_name = (
            "liba9tas_habi1_bootstrap_"
            f"{args.payload_source_sha256[:16]}.so"
        )
        require(args.elf.name == expected_name,
                "bootstrap filename is not payload-source-bound")
        elf = Elf64(args.elf, expected_machine=EM_X86_64)
        loads = [program for program in elf.programs if program.type == PT_LOAD]
        require(loads and all((p.flags & (PF_W | PF_X)) != (PF_W | PF_X)
                              for p in loads), "bootstrap has W+X PT_LOAD")
        stack = [p for p in elf.programs if p.type == 0x6474E551]
        require(len(stack) == 1 and stack[0].flags == (PF_R | PF_W),
                "bootstrap GNU_STACK must be RW")
        relro = [p for p in elf.programs if p.type == 0x6474E552]
        require(len(relro) == 1, "bootstrap must have one GNU_RELRO")

        dynsym = elf.symbols(".dynsym")
        defined = {
            symbol.name: symbol for symbol in dynsym
            if symbol.name and symbol.section_index != 0 and
            symbol.binding == 1 and symbol.visibility == 0
        }
        require(set(defined) == EXPECTED_EXPORTS,
                f"bootstrap export set drifted: {sorted(set(defined) ^ EXPECTED_EXPORTS)}")
        imports = {
            symbol.name.split("@", 1)[0] for symbol in dynsym
            if symbol.name and symbol.section_index == 0 and symbol.binding in (1, 2)
        }
        require(imports == EXPECTED_IMPORTS,
                f"bootstrap import set drifted: {sorted(imports ^ EXPECTED_IMPORTS)}")

        locator = defined[LOCATOR_SYMBOL]
        require(locator.type == 1 and locator.size == LOCATOR_SIZE,
                "locator must be a 512-byte OBJECT")
        require(relro[0].vaddr <= locator.value and
                locator.value + locator.size <= relro[0].vaddr + relro[0].memsz,
                "locator must be entirely within GNU_RELRO")
        blob = elf.virtual_bytes(locator.value, LOCATOR_SIZE)
        magic, version, size = struct.unpack_from("<QII", blob)
        require((magic, version, size) ==
                (LOCATOR_MAGIC, LOCATOR_VERSION, LOCATOR_SIZE),
                "locator header mismatch")
        require(fixed_string(blob, 16, 192) == args.payload_device_path,
                "locator payload path mismatch")
        require(fixed_string(blob, 208, 65) == args.payload_sha256,
                "locator payload SHA mismatch")
        require(fixed_string(blob, 273, 41) == args.payload_build_id,
                "locator payload build-id mismatch")
        require(fixed_string(blob, 314, 65) == args.payload_source_sha256,
                "locator payload source SHA mismatch")
        require(fixed_string(blob, 379, 64) in PROJECT_EXPORTS,
                "locator run symbol mismatch")
        require(fixed_string(blob, 443, 8) == "JJ", "locator shorty mismatch")
        require(struct.unpack_from("<IIII", blob, 480) == (4, 4, 8, 0),
                "locator atomic sizes/reserved mismatch")

        symtab = {symbol.name: symbol for symbol in elf.symbols(".symtab")
                  if symbol.name}
        expected_relative = {
            locator.value + 456: symtab["_ZN12_GLOBAL__N_17g_stageE"].value,
            locator.value + 464:
                symtab["_ZN12_GLOBAL__N_126g_same_thread_probe_statusE"].value,
            locator.value + 472:
                symtab["_ZN12_GLOBAL__N_130g_same_thread_probe_trampolineE"].value,
        }
        expected_symbolic = {
            locator.value + 496:
                "a9tas_bootstrap_hook_abi_selftest_habi1_return_trap",
            locator.value + 504:
                "a9tas_bootstrap_hook_abi_selftest_habi1_calibrate_tid",
        }
        dyn_names = [symbol.name for symbol in dynsym]
        found_relative: dict[int, int] = {}
        found_symbolic: dict[int, str] = {}
        for section in elf.sections:
            if section.type != SHT_RELA or section.entsize != RELA.size:
                continue
            for offset in range(section.offset, section.offset + section.size, RELA.size):
                relocation_offset, info, addend = RELA.unpack_from(elf.data, offset)
                symbol_index = info >> 32
                relocation_type = info & 0xFFFFFFFF
                if relocation_offset in expected_relative:
                    require(symbol_index == 0 and relocation_type == 8,
                            "locator atomic pointer must use R_X86_64_RELATIVE")
                    found_relative[relocation_offset] = addend
                if relocation_offset in expected_symbolic:
                    require(relocation_type == 1 and symbol_index < len(dyn_names),
                            "locator code pointer must use R_X86_64_64")
                    found_symbolic[relocation_offset] = dyn_names[symbol_index]
        require(found_relative == expected_relative,
                "locator atomic relocation targets drifted")
        require(found_symbolic == expected_symbolic,
                "locator code relocation targets drifted")

        trap = defined["a9tas_bootstrap_hook_abi_selftest_habi1_return_trap"]
        calibrate = defined["a9tas_bootstrap_hook_abi_selftest_habi1_calibrate_tid"]
        require(elf.virtual_bytes(trap.value, 2) == b"\xCC\xC3",
                "fixed return trap must be int3;ret")
        calibrate_bytes = elf.virtual_bytes(calibrate.value, calibrate.size)
        require(calibrate_bytes.startswith(b"\xBF\xBA\x00\x00\x00\x31\xC0"),
                "calibration function must call syscall(__NR_gettid=186)")

        raw = elf.data
        for token in (args.payload_device_path.encode(), args.payload_sha256.encode(),
                      args.payload_build_id.encode(),
                      args.payload_source_sha256.encode(),
                      b"a9tas_hook_abi_selftest_habi1_run", b"JJ"):
            require(raw.count(token) >= 1, f"bootstrap identity token missing: {token!r}")
        require(b"UNSET" not in raw, "bootstrap contains UNSET identity")
        require(b"ptrace" not in raw and b"process_vm" not in raw,
                "carrier must not contain controller capabilities")

        report = {
            "protocol": "hook-abi-selftest-habi1-bootstrap",
            "revision": 2,
            "source_sha256": sha256(args.source),
            "bootstrap_core_sha256": sha256(args.bootstrap_core),
            "elf_sha256": sha256(args.elf),
            "build_id": elf.build_id(),
            "payload_sha256": args.payload_sha256,
            "payload_build_id": args.payload_build_id,
            "payload_source_sha256": args.payload_source_sha256,
            "payload_device_path": args.payload_device_path,
            "locator_rva": f"0x{locator.value:x}",
            "locator_size": LOCATOR_SIZE,
            "return_trap_rva": f"0x{trap.value:x}",
            "calibrate_tid_rva": f"0x{calibrate.value:x}",
            "nativebridge_callback_restore_required": 1,
            "carrier_is_passive": 0,
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
        print(
            "HABI1_BOOTSTRAP_POLICY passed=1 revision=2 locator_size=512 "
            f"elf_sha256={report['elf_sha256']} build_id={report['build_id']} "
            "passive=0 disposable_process_only=1 device_access=0 deployed=0"
        )
        return 0
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f"HABI1_BOOTSTRAP_POLICY passed=0 error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
