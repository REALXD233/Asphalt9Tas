#!/usr/bin/env python3
"""Offline identity, ELF, ABI and source-policy verifier for HABI-1.

This verifier deliberately performs no device access.  It accepts only the
identity-named ARM64 payload produced from the supplied source/header pair.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import struct
import subprocess
import sys
from dataclasses import dataclass


ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
SYMBOL = struct.Struct("<IBBHQQ")
RELA = struct.Struct("<QQq")

PT_LOAD = 1
PT_NOTE = 4
PF_X = 1
PF_W = 2
PF_R = 4
SHT_RELA = 4
SHT_DYNSYM = 11
SHT_SYMTAB = 2
ET_DYN = 3
EM_AARCH64 = 183
EM_X86_64 = 62

TARGET_RVA = 0x6000
PAGE_END_RVA = 0x7000
PATCH_SIZE = 16
EXPECTED_PROLOGUE = bytes.fromhex(
    "ff8301d1 f55304a9 f37b05a9 f40308aa".replace(" ", "")
)
PROJECT_EXPORTS = {
    "a9tas_hook_abi_selftest_habi1_protocol",
    "a9tas_hook_abi_selftest_habi1_run",
}
ALLOWED_RUNTIME_DEFINITION = "__emutls_get_address"
ALLOWED_INIT_SYMBOLS = {
    "init_have_lse_atomics",
    "__init_cpu_features",
}
FORBIDDEN_BYTES = (
    b"libAsphalt9",
    b"Barrel",
    b"ptrace",
    b"process_vm",
    b"dlopen",
    b"pthread_create",
)


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def c_string(table: bytes, offset: int) -> str:
    require(0 <= offset < len(table), f"string offset out of range: {offset}")
    end = table.find(b"\0", offset)
    require(end >= 0, "unterminated ELF string")
    return table[offset:end].decode("utf-8", "strict")


@dataclass(frozen=True)
class Program:
    type: int
    flags: int
    offset: int
    vaddr: int
    filesz: int
    memsz: int
    align: int


@dataclass(frozen=True)
class Section:
    name: str
    type: int
    flags: int
    address: int
    offset: int
    size: int
    link: int
    info: int
    align: int
    entsize: int


@dataclass(frozen=True)
class SymbolRecord:
    name: str
    info: int
    other: int
    section_index: int
    value: int
    size: int

    @property
    def binding(self) -> int:
        return self.info >> 4

    @property
    def type(self) -> int:
        return self.info & 0xF

    @property
    def visibility(self) -> int:
        return self.other & 0x3


class Elf64:
    def __init__(self, path: pathlib.Path, expected_machine: int = EM_AARCH64):
        self.path = path
        self.data = path.read_bytes()
        require(len(self.data) >= ELF_HEADER.size, "ELF is truncated")
        fields = ELF_HEADER.unpack_from(self.data)
        ident = fields[0]
        require(ident[:4] == b"\x7fELF", "not an ELF file")
        require(ident[4] == 2 and ident[5] == 1, "ELF must be 64-bit little-endian")
        require(fields[1] == ET_DYN, "ELF type must be ET_DYN")
        require(fields[2] == expected_machine,
                f"ELF machine mismatch: expected {expected_machine}, got {fields[2]}")
        self.phoff, self.shoff = fields[5], fields[6]
        self.phentsize, self.phnum = fields[9], fields[10]
        self.shentsize, self.shnum, self.shstrndx = fields[11], fields[12], fields[13]
        require(self.phentsize == PROGRAM_HEADER.size and self.phnum > 0,
                "unexpected program-header table")
        require(self.shentsize == SECTION_HEADER.size and self.shnum > 0,
                "unexpected section-header table")
        require(self.phoff + self.phnum * self.phentsize <= len(self.data),
                "program-header table is truncated")
        require(self.shoff + self.shnum * self.shentsize <= len(self.data),
                "section-header table is truncated")
        self.programs = self._programs()
        self.sections = self._sections()

    def _programs(self) -> list[Program]:
        programs: list[Program] = []
        for index in range(self.phnum):
            values = PROGRAM_HEADER.unpack_from(self.data, self.phoff + index * self.phentsize)
            p_type, flags, offset, vaddr, _paddr, filesz, memsz, align = values
            require(offset + filesz <= len(self.data), "program segment exceeds file")
            programs.append(Program(p_type, flags, offset, vaddr, filesz, memsz, align))
        return programs

    def _sections(self) -> list[Section]:
        raw = [
            SECTION_HEADER.unpack_from(self.data, self.shoff + i * self.shentsize)
            for i in range(self.shnum)
        ]
        require(0 <= self.shstrndx < len(raw), "invalid shstrndx")
        names_raw = raw[self.shstrndx]
        names = self.data[names_raw[4]: names_raw[4] + names_raw[5]]
        sections: list[Section] = []
        for values in raw:
            name_offset, kind, flags, address, offset, size, link, info, align, entsize = values
            require(offset + size <= len(self.data) or kind == 8, "section exceeds file")
            sections.append(Section(c_string(names, name_offset), kind, flags, address,
                                    offset, size, link, info, align, entsize))
        return sections

    def section(self, name: str) -> Section:
        matches = [section for section in self.sections if section.name == name]
        require(len(matches) == 1, f"expected one section {name}, got {len(matches)}")
        return matches[0]

    def symbols(self, section_name: str) -> list[SymbolRecord]:
        section = self.section(section_name)
        require(section.type in (SHT_DYNSYM, SHT_SYMTAB), "not a symbol table")
        require(section.entsize == SYMBOL.size and section.size % SYMBOL.size == 0,
                f"invalid {section_name} entry size")
        require(0 <= section.link < len(self.sections), "invalid symbol string-table link")
        strings_section = self.sections[section.link]
        strings = self.data[strings_section.offset:strings_section.offset + strings_section.size]
        records: list[SymbolRecord] = []
        for offset in range(section.offset, section.offset + section.size, SYMBOL.size):
            name, info, other, shndx, value, size = SYMBOL.unpack_from(self.data, offset)
            records.append(SymbolRecord(c_string(strings, name), info, other, shndx, value, size))
        return records

    def virtual_bytes(self, address: int, size: int) -> bytes:
        for program in self.programs:
            if program.type != PT_LOAD:
                continue
            if address >= program.vaddr and address + size <= program.vaddr + program.filesz:
                offset = program.offset + address - program.vaddr
                return self.data[offset:offset + size]
        raise ValueError(f"virtual range not file-backed: 0x{address:x}+0x{size:x}")

    def build_id(self) -> str:
        for program in self.programs:
            if program.type != PT_NOTE:
                continue
            cursor = program.offset
            end = cursor + program.filesz
            while cursor + 12 <= end:
                namesz, descsz, kind = struct.unpack_from("<III", self.data, cursor)
                cursor += 12
                name = self.data[cursor:cursor + namesz]
                cursor += (namesz + 3) & ~3
                desc = self.data[cursor:cursor + descsz]
                cursor += (descsz + 3) & ~3
                if kind == 3 and name == b"GNU\0":
                    require(0 < len(desc) <= 32, "invalid GNU build-id length")
                    return desc.hex()
        raise ValueError("GNU build-id missing")


def function_disassembly(objdump: pathlib.Path, elf: pathlib.Path, symbol: str) -> str:
    completed = subprocess.run(
        [str(objdump), "-d", f"--disassemble-symbols={symbol}", str(elf)],
        check=False, capture_output=True, text=True, encoding="utf-8", errors="replace"
    )
    require(completed.returncode == 0, f"objdump failed for {symbol}: {completed.stderr}")
    return completed.stdout.lower()


def verify_abi_disassembly(objdump: pathlib.Path, elf: pathlib.Path) -> dict[str, bool]:
    entry = function_disassembly(objdump, elf, "HookAbiSelftestEntry")
    invoke = function_disassembly(objdump, elf, "HookAbiInvokeRaw")

    def has_register(disassembly: str, register: str) -> bool:
        return re.search(rf"\b{re.escape(register)}\b", disassembly) is not None

    entry_checks = {
        "entry_x18_saved":
            ("str\tx18" in entry or "stp\tx18" in entry) and
            ("ldr\tx18" in entry or "ldp\tx18" in entry),
        "entry_q0_q31_saved": all(has_register(entry, f"q{i}") for i in range(32)),
        "entry_nzcv": "nzcv" in entry,
        "entry_fpcr": "fpcr" in entry,
        "entry_fpsr": "fpsr" in entry,
        "entry_calls_filter": "hookabiselftestfilter" in entry,
        "entry_branches_continue": "g_hook_abi_selftest_continue" in entry,
    }
    invoke_checks = {
        "invoke_x18_saved":
            ("str\tx18" in invoke or "stp\tx18" in invoke) and
            ("ldr\tx18" in invoke or "ldp\tx18" in invoke),
        "invoke_q0_q31_present": all(has_register(invoke, f"q{i}") for i in range(32)),
        "invoke_nzcv": "nzcv" in invoke,
        "invoke_fpcr": "fpcr" in invoke,
        "invoke_fpsr": "fpsr" in invoke,
        "invoke_calls_target": "hookabiselftesttarget" in invoke,
    }
    checks = entry_checks | invoke_checks
    for name, passed in checks.items():
        require(passed, f"ABI disassembly check failed: {name}")
    return checks


def verify_source_policy(source: pathlib.Path) -> None:
    text = source.read_text(encoding="utf-8")
    required = (
        "constexpr int kLogicalCodeProtection = PROT_READ | PROT_EXEC;",
        "IsPrivateGuestCodeView",
        "protection == PROT_READ",
        "protection == (PROT_READ | PROT_EXEC)",
        "IsPrivateWritableNoExec",
        "TargetRangeInExecutableLoad",
        "selftest_constructor_trigger=0",
        "revision=2",
        "return 2;",
        "results->target_page_w_xor_x =",
        "results->restore_rw_no_exec_readback",
        "/data/user/0/com.aligames.kuang.kybc.aligames/cache/",
    )
    for token in required:
        require(token in text, f"source-policy token missing: {token}")
    forbidden = (
        "target_page_w_xor_x=1",
        "original_protection_",
        "MappingProtection(",
        "/data/local/tmp/a9tas-hook-abi-selftest-habi1.status",
    )
    for token in forbidden:
        require(token not in text, f"obsolete source-policy token present: {token}")
    require(re.search(r'(?m)^\s*"constructor_trigger=0\\n', text) is None,
            "obsolete constructor_trigger receipt field is present")
    require(text.count("a9tas_hook_abi_selftest_habi1_run(JNIEnv*") == 1,
            "explicit run export must be unique")
    require("__attribute__((constructor" not in text,
            "project selftest constructor is forbidden")


def verify_init_array(elf: Elf64) -> list[str]:
    init = elf.section(".init_array")
    require(init.size > 0 and init.size % 8 == 0, "unexpected .init_array size")
    symtab = elf.symbols(".symtab")
    symbols_by_value = {
        symbol.value: symbol.name for symbol in symtab
        if symbol.name and symbol.type == 2 and not symbol.name.startswith("$")
    }
    addends: list[int] = []
    for section in elf.sections:
        if section.type != SHT_RELA or section.entsize != RELA.size:
            continue
        for offset in range(section.offset, section.offset + section.size, RELA.size):
            relocation_offset, info, addend = RELA.unpack_from(elf.data, offset)
            if init.address <= relocation_offset < init.address + init.size:
                require((info >> 32) == 0, "init-array relocation unexpectedly references dynsym")
                addends.append(addend)
    require(len(addends) == init.size // 8, "every init-array slot must have one relocation")
    names = [symbols_by_value.get(addend, f"0x{addend:x}") for addend in addends]
    require(set(names) <= ALLOWED_INIT_SYMBOLS,
            f"unexpected init-array target(s): {names}")
    require(not (set(names) & PROJECT_EXPORTS), "selftest export appears in init-array")
    return names


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--header", type=pathlib.Path, required=True)
    parser.add_argument("--elf", type=pathlib.Path, required=True)
    parser.add_argument("--objdump", type=pathlib.Path, required=True)
    parser.add_argument("--expected-device-path", required=True)
    parser.add_argument("--report", type=pathlib.Path)
    args = parser.parse_args()

    try:
        for path in (args.source, args.header, args.elf, args.objdump):
            require(path.is_file(), f"missing input: {path}")
        source_sha = sha256(args.source)
        header_sha = sha256(args.header)
        elf_sha = sha256(args.elf)
        require(re.fullmatch(r"[0-9a-f]{64}", source_sha) is not None, "bad source SHA")
        expected_name = f"liba9tas_habi1_self_hook_{source_sha[:16]}.so"
        require(args.elf.name == expected_name, "ELF filename is not source-identity-bound")
        require(args.expected_device_path == f"/data/local/tmp/{expected_name}",
                "device path is not source-identity-bound")

        verify_source_policy(args.source)
        elf = Elf64(args.elf)
        loads = [program for program in elf.programs if program.type == PT_LOAD]
        require(loads, "no PT_LOAD segments")
        require(all((program.flags & (PF_W | PF_X)) != (PF_W | PF_X) for program in loads),
                "W+X PT_LOAD is forbidden")
        require(any(program.flags == (PF_R | PF_X) for program in loads),
                "exact R-X PT_LOAD missing")
        require(any(program.flags == (PF_R | PF_W) for program in loads),
                "exact RW PT_LOAD missing")
        stack = [program for program in elf.programs if program.type == 0x6474E551]
        require(len(stack) == 1 and stack[0].flags == (PF_R | PF_W),
                "GNU_STACK must be exactly RW")

        symtab = elf.symbols(".symtab")
        dynsym = elf.symbols(".dynsym")
        by_name = {symbol.name: symbol for symbol in symtab if symbol.name}
        for symbol in ("HookAbiSelftestTarget", "HookAbiSelftestTargetPageEnd",
                       "HookAbiSelftestEntry", "HookAbiInvokeRaw"):
            require(symbol in by_name, f"required symbol missing: {symbol}")
        require(by_name["HookAbiSelftestTarget"].value == TARGET_RVA,
                "target RVA drifted")
        require(by_name["HookAbiSelftestTargetPageEnd"].value == PAGE_END_RVA,
                "target page-end RVA drifted")
        require(elf.virtual_bytes(TARGET_RVA, PATCH_SIZE) == EXPECTED_PROLOGUE,
                "target prologue drifted")
        executable_loads = [program for program in loads if program.flags == (PF_R | PF_X)]
        require(any(TARGET_RVA >= program.vaddr and
                    TARGET_RVA + PATCH_SIZE <= program.vaddr + program.memsz
                    for program in executable_loads),
                "target is not in logical R-X PT_LOAD")

        defined_dynamic = {
            symbol.name: symbol for symbol in dynsym
            if symbol.name and symbol.section_index != 0 and
            symbol.binding in (1, 2) and symbol.visibility == 0
        }
        require(PROJECT_EXPORTS <= set(defined_dynamic), "project exports missing")
        allowed_defined = PROJECT_EXPORTS | {ALLOWED_RUNTIME_DEFINITION}
        require(set(defined_dynamic) == allowed_defined,
                f"unexpected defined dynamic symbols: {sorted(set(defined_dynamic) - allowed_defined)}")
        require(defined_dynamic[ALLOWED_RUNTIME_DEFINITION].binding == 2,
                "__emutls_get_address must remain compiler-runtime WEAK")

        raw = elf.data
        for required in (
            source_sha.encode(), header_sha.encode(), args.expected_device_path.encode(),
            b"protocol=hook-abi-selftest-habi1", b"revision=2",
            b"selftest_constructor_trigger=0", b"target_page_w_xor_x=%d",
        ):
            require(raw.count(required) == 1, f"embedded identity/policy token count != 1: {required!r}")
        require(b"UNSET" not in raw, "UNSET identity remains embedded")
        for forbidden in FORBIDDEN_BYTES:
            require(forbidden not in raw, f"forbidden payload capability/string: {forbidden!r}")

        init_targets = verify_init_array(elf)
        abi_checks = verify_abi_disassembly(args.objdump, args.elf)
        build_id = elf.build_id()
        report = {
            "protocol": "hook-abi-selftest-habi1",
            "revision": 2,
            "gate": "HABI-1",
            "source_sha256": source_sha,
            "header_sha256": header_sha,
            "elf_sha256": elf_sha,
            "build_id": build_id,
            "expected_device_path": args.expected_device_path,
            "target_rva": f"0x{TARGET_RVA:x}",
            "target_page_end_rva": f"0x{PAGE_END_RVA:x}",
            "project_exports": sorted(PROJECT_EXPORTS),
            "compiler_runtime_weak_export": ALLOWED_RUNTIME_DEFINITION,
            "init_array_targets": init_targets,
            "abi_checks": abi_checks,
            "logical_exec_proof": "ELF_PT_LOAD_R_X",
            "accepted_houdini_host_views": ["r--p", "r-xp"],
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
            "HABI1_PAYLOAD_POLICY passed=1 revision=2 "
            f"source_sha256={source_sha} elf_sha256={elf_sha} build_id={build_id} "
            "device_access=0 deployed=0 dynamic=NOT_RUN"
        )
        return 0
    except (OSError, ValueError, struct.error) as error:
        print(f"HABI1_PAYLOAD_POLICY passed=0 error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
