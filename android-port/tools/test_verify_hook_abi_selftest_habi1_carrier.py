#!/usr/bin/env python3
"""Host-only negative matrix for the fixed HABI-1 early carrier policy."""

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


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(args: argparse.Namespace, source: pathlib.Path, elf: pathlib.Path,
        *, bootstrap_path: str | None = None, libc_sha: str | None = None,
        trap_rva: str | None = None) -> subprocess.CompletedProcess[str]:
    command = [
        sys.executable, "-B", str(args.verifier),
        "--source", str(source),
        "--controller-source", str(args.controller_source),
        "--elf", str(elf),
        "--bootstrap-path", bootstrap_path or args.bootstrap_path,
        "--bootstrap-sha256", args.bootstrap_sha256,
        "--bootstrap-build-id", args.bootstrap_build_id,
        "--payload-path", args.payload_path,
        "--payload-sha256", args.payload_sha256,
        "--payload-build-id", args.payload_build_id,
        "--payload-source-sha256", args.payload_source_sha256,
        "--libc-file", str(args.libc_file),
        "--libc-device-path", args.libc_device_path,
        "--libc-sha256", libc_sha or args.libc_sha256,
        "--libc-build-id", args.libc_build_id,
        "--libc-trap-rva", trap_rva or args.libc_trap_rva,
    ]
    return subprocess.run(command, check=False, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def expect_failure(name: str, result: subprocess.CompletedProcess[str]) -> None:
    require(result.returncode != 0, f"negative case unexpectedly passed: {name}")
    require("passed=0" in result.stderr, f"negative case was not fail-closed: {name}")


def make_wx(data: bytearray) -> None:
    header = ELF_HEADER.unpack_from(data)
    phoff, phentsize, phnum = header[5], header[9], header[10]
    require(phentsize == PROGRAM_HEADER.size, "unexpected program-header size")
    for index in range(phnum):
        offset = phoff + index * phentsize
        program = PROGRAM_HEADER.unpack_from(data, offset)
        if program[0] == 1 and program[1] == 5:
            struct.pack_into("<I", data, offset + 4, 7)
            return
    raise RuntimeError("R-X PT_LOAD not found")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verifier", type=pathlib.Path, required=True)
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
    args = parser.parse_args()
    try:
        baseline = run(args,args.source,args.elf)
        require(baseline.returncode == 0,
                f"carrier policy baseline failed: {baseline.stdout}{baseline.stderr}")
        failures = 0
        with tempfile.TemporaryDirectory(prefix="a9tas-habi1-carrier-") as raw:
            temp = pathlib.Path(raw)
            expect_failure("wrong_bootstrap_path",run(
                args,args.source,args.elf,
                bootstrap_path="/data/local/tmp/wrong-bootstrap.so"))
            failures += 1
            expect_failure("wrong_libc_sha",run(
                args,args.source,args.elf,libc_sha="0"*64))
            failures += 1
            expect_failure("wrong_trap_rva",run(
                args,args.source,args.elf,trap_rva="0x5b1"))
            failures += 1

            capability = temp / "capability.cpp"
            capability.write_text(
                args.source.read_text(encoding="utf-8") +
                "\n// process_vm_writev forbidden\n", encoding="utf-8")
            expect_failure("forbidden_source_capability",run(
                args,capability,args.elf))
            failures += 1

            wrong_name = temp / "wrong-carrier"
            shutil.copyfile(args.elf,wrong_name)
            expect_failure("wrong_carrier_filename",run(
                args,args.source,wrong_name))
            failures += 1

            wx_dir = temp / "wx"
            wx_dir.mkdir()
            wx = wx_dir / args.elf.name
            data = bytearray(args.elf.read_bytes())
            make_wx(data)
            wx.write_bytes(data)
            expect_failure("writable_executable_load",run(
                args,args.source,wx))
            failures += 1

        print("HABI1_CARRIER_VERIFIER_SELFTEST passed=1 baseline=1 "
              f"negative_cases={failures} device_access=0 deployed=0")
        return 0
    except (OSError, RuntimeError, ValueError, struct.error) as error:
        print(f"HABI1_CARRIER_VERIFIER_SELFTEST passed=0 error={error}",file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
