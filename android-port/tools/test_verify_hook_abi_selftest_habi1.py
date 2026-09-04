#!/usr/bin/env python3
"""Host-only negative tests for the HABI-1 payload verifier."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile


ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
RELA = struct.Struct("<QQq")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run_verifier(args: argparse.Namespace, source: pathlib.Path,
                 elf: pathlib.Path, device_path: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(args.verifier),
         "--source", str(source), "--header", str(args.header),
         "--elf", str(elf), "--objdump", str(args.objdump),
         "--expected-device-path", device_path],
        check=False, capture_output=True, text=True, encoding="utf-8", errors="replace"
    )


def elf_tables(data: bytearray):
    header = ELF_HEADER.unpack_from(data)
    phoff, shoff = header[5], header[6]
    phentsize, phnum = header[9], header[10]
    shentsize, shnum, shstrndx = header[11], header[12], header[13]
    require(phentsize == PROGRAM_HEADER.size, "unexpected program header size")
    require(shentsize == SECTION_HEADER.size, "unexpected section header size")
    programs = [PROGRAM_HEADER.unpack_from(data, phoff + i * phentsize)
                for i in range(phnum)]
    sections = [SECTION_HEADER.unpack_from(data, shoff + i * shentsize)
                for i in range(shnum)]
    names_section = sections[shstrndx]
    names = bytes(data[names_section[4]:names_section[4] + names_section[5]])

    def name_at(offset: int) -> str:
        end = names.index(b"\0", offset)
        return names[offset:end].decode("ascii")

    named_sections = {name_at(section[0]): section for section in sections}
    return phoff, programs, named_sections


def mutate_prologue(data: bytearray) -> None:
    _phoff, programs, _sections = elf_tables(data)
    target = 0x6000
    for p_type, _flags, offset, vaddr, _paddr, filesz, _memsz, _align in programs:
        if p_type == 1 and vaddr <= target < vaddr + filesz:
            data[offset + target - vaddr] ^= 0x01
            return
    raise RuntimeError("target RVA not found")


def mutate_rx_to_wx(data: bytearray) -> None:
    phoff, programs, _sections = elf_tables(data)
    for index, program in enumerate(programs):
        p_type, flags = program[0], program[1]
        if p_type == 1 and flags == 5:
            struct.pack_into("<I", data, phoff + index * PROGRAM_HEADER.size + 4, 7)
            return
    raise RuntimeError("R-X PT_LOAD not found")


def mutate_init_array(data: bytearray) -> None:
    _phoff, _programs, sections = elf_tables(data)
    init = sections[".init_array"]
    rela = sections[".rela.dyn"]
    init_address, init_size = init[3], init[5]
    for offset in range(rela[4], rela[4] + rela[5], RELA.size):
        relocation_offset, _info, _addend = RELA.unpack_from(data, offset)
        if init_address <= relocation_offset < init_address + init_size:
            struct.pack_into("<q", data, offset + 16, 0x353C)
            return
    raise RuntimeError("init-array relocation not found")


def expect_failure(name: str, completed: subprocess.CompletedProcess[str]) -> None:
    require(completed.returncode != 0, f"negative case unexpectedly passed: {name}")
    require("passed=0" in completed.stderr,
            f"negative case lacked fail-closed receipt: {name}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verifier", type=pathlib.Path, required=True)
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--header", type=pathlib.Path, required=True)
    parser.add_argument("--elf", type=pathlib.Path, required=True)
    parser.add_argument("--objdump", type=pathlib.Path, required=True)
    parser.add_argument("--expected-device-path", required=True)
    args = parser.parse_args()

    try:
        for path in (args.verifier, args.source, args.header, args.elf, args.objdump):
            require(path.is_file(), f"missing input: {path}")
        baseline = run_verifier(args, args.source, args.elf,
                                args.expected_device_path)
        require(baseline.returncode == 0,
                f"baseline verifier failed: {baseline.stdout}{baseline.stderr}")

        failures = 0
        with tempfile.TemporaryDirectory(prefix="a9tas-habi1-policy-") as raw_temp:
            temp = pathlib.Path(raw_temp)

            expect_failure("wrong_device_path", run_verifier(
                args, args.source, args.elf, "/data/local/tmp/wrong.so"))
            failures += 1

            wrong_name = temp / "wrong.so"
            shutil.copyfile(args.elf, wrong_name)
            expect_failure("wrong_elf_filename", run_verifier(
                args, args.source, wrong_name, args.expected_device_path))
            failures += 1

            changed_source = temp / args.source.name
            changed_source.write_bytes(args.source.read_bytes() + b"\n")
            expect_failure("changed_source", run_verifier(
                args, changed_source, args.elf, args.expected_device_path))
            failures += 1

            for name, mutation in (
                ("prologue_drift", mutate_prologue),
                ("writable_executable_load", mutate_rx_to_wx),
                ("selftest_init_array", mutate_init_array),
            ):
                case_dir = temp / name
                case_dir.mkdir()
                mutated = case_dir / args.elf.name
                data = bytearray(args.elf.read_bytes())
                mutation(data)
                mutated.write_bytes(data)
                expect_failure(name, run_verifier(
                    args, args.source, mutated, args.expected_device_path))
                failures += 1

            capability_dir = temp / "forbidden_capability"
            capability_dir.mkdir()
            capability = capability_dir / args.elf.name
            capability.write_bytes(args.elf.read_bytes() + b"ptrace\0")
            expect_failure("forbidden_capability", run_verifier(
                args, args.source, capability, args.expected_device_path))
            failures += 1

        print(
            "HABI1_PAYLOAD_VERIFIER_SELFTEST passed=1 baseline=1 "
            f"negative_cases={failures} device_access=0 deployed=0"
        )
        return 0
    except (OSError, RuntimeError, struct.error, ValueError) as error:
        print(f"HABI1_PAYLOAD_VERIFIER_SELFTEST passed=0 error={error}",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
