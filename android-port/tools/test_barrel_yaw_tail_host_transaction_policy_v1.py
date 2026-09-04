#!/usr/bin/env python3
"""Audit the read-only resolver and pure BarrelYaw host transaction."""

from __future__ import annotations

import hashlib
import pathlib
import re
import struct
import subprocess
import sys


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
HEADER = WORKSPACE / "android-port" / "src" / "barrel_yaw_tail_host_transaction_v1.h"
SELFTEST = WORKSPACE / "android-port" / "src" / "barrel_yaw_tail_host_transaction_selftest_v1.cpp"
RESOLVER = WORKSPACE / "android-port" / "src" / "barrel_yaw_tail_payload_elf_resolver_v1.h"
RESOLVER_SELFTEST = WORKSPACE / "android-port" / "src" / "barrel_yaw_tail_payload_elf_resolver_selftest_v1.cpp"
MAPPING_MODEL = WORKSPACE / "android-port" / "tools" / "test_barrel_yaw_tail_resolver_mapping_model_v1.py"
PROTOCOL = WORKSPACE / "android-port" / "src" / "barrel_yaw_tail_payload_protocol_v1.h"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(*args: str) -> str:
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def pinned_hash(text: str) -> bytes:
    match = re.search(r"kExpectedSha256\[32\]\s*=\s*\{([^}]+)\}", text, re.S)
    require(match is not None, "resolver hash pin missing")
    values = [int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1))]
    require(len(values) == 32, "resolver hash pin is not 32 bytes")
    return bytes(values)


def validate_payload_program_headers(payload: pathlib.Path) -> None:
    data = payload.read_bytes()
    ehdr_format = "<16sHHIQQQIHHHHHH"
    phdr_format = "<IIQQQQQQ"
    require(len(data) >= struct.calcsize(ehdr_format), "payload ELF header truncated")
    header = struct.unpack_from(ehdr_format, data)
    ident, elf_type, machine, version = header[:4]
    phoff = header[5]
    ehsize, phentsize, phnum = header[8:11]
    require(ident[:4] == b"\x7fELF" and ident[4] == 2 and ident[5] == 1,
            "payload is not little-endian ELF64")
    require(elf_type == 3 and machine == 183 and version == 1,
            "payload ELF identity drift")
    require(ehsize == 64 and phentsize == struct.calcsize(phdr_format) and
            0 < phnum <= 64 and phoff <= len(data) and
            phnum * phentsize <= len(data) - phoff,
            "payload program-header table invalid")
    programs = [struct.unpack_from(phdr_format, data, phoff + i * phentsize)
                for i in range(phnum)]
    loads = [program for program in programs if program[0] == 1]
    expected_loads = [
        (1, 4, 0x0000, 0x0000, 0x0000, 0x0D64, 0x0D64, 0x1000),
        (1, 5, 0x0D70, 0x1D70, 0x1D70, 0x0F90, 0x0F90, 0x1000),
        (1, 6, 0x1D00, 0x3D00, 0x3D00, 0x0228, 0x0300, 0x1000),
        (1, 6, 0x1F40, 0x4F40, 0x4F40, 0x0380, 0x7EC90, 0x1000),
    ]
    require(loads == expected_loads, "payload exact PT_LOAD layout drift")
    final_rw = loads[-1]
    require(final_rw == (1, 6, 0x1F40, 0x4F40, 0x4F40,
                         0x380, 0x7EC90, 0x1000),
            "payload final RW PT_LOAD drift")
    require(final_rw[3] + final_rw[5] == 0x52C0 and
            final_rw[3] + final_rw[6] == 0x83BD0,
            "payload file/BSS boundary drift")
    boundary_rva = 0x1DD0
    boundary_size = 0x484
    require(any((program[1] & 5) == 5 and not (program[1] & 2) and
                boundary_rva >= program[3] and
                boundary_rva + boundary_size <= program[3] + program[5]
                for program in loads),
            "payload boundary is not file-backed RX")


def validate_locator_relocations(readelf: pathlib.Path,
                                 payload: pathlib.Path) -> None:
    expected = {
        0x50C0: 0x5100,
        0x50C8: 0x4F40,
        0x50D0: 0x75AC0,
        0x50D8: 0x52C0,
        0x50E0: 0x5000,
    }
    observed: dict[int, tuple[str, int]] = {}
    for line in run(str(readelf), "-r", str(payload)).splitlines():
        match = re.match(
            r"^([0-9a-fA-F]{16})\s+[0-9a-fA-F]{16}\s+"
            r"(R_AARCH64_[A-Z_]+)\s+([0-9a-fA-F]+)\s*$",
            line,
        )
        if match is None:
            continue
        offset = int(match.group(1), 16)
        if offset in expected:
            require(offset not in observed, f"duplicate locator relocation: {offset:#x}")
            observed[offset] = (match.group(2), int(match.group(3), 16))
    require(set(observed) == set(expected), "locator relocation set incomplete")
    for offset, addend in expected.items():
        require(observed[offset] == ("R_AARCH64_RELATIVE", addend),
                f"locator relocation drift at {offset:#x}")


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 4:
        print("usage: policy TRANSACTION RESOLVER PAYLOAD READELF", file=sys.stderr)
        return 2
    transaction, resolver_artifact, payload, readelf = map(pathlib.Path, args)
    for path in (transaction, resolver_artifact, payload, readelf, HEADER,
                 SELFTEST, RESOLVER, RESOLVER_SELFTEST, MAPPING_MODEL,
                 PROTOCOL):
        require(path.is_file(), f"missing required file: {path}")

    header = HEADER.read_text(encoding="utf-8")
    selftest = SELFTEST.read_text(encoding="utf-8")
    resolver = RESOLVER.read_text(encoding="utf-8")
    resolver_selftest = RESOLVER_SELFTEST.read_text(encoding="utf-8")
    combined = header + "\n" + selftest
    for token in (
        "original_table + protocol::kVptrPrefixSize",
        "protocol::kBoundarySlotOffset",
        "result.unpublished_control.flags = 0",
        "result.published_control.flags =",
        "protocol::kControlConfigured | protocol::kControlTargetsLoaded",
        "active_token == protocol::kDisarmedToken",
        "evidence.active_call_count == 0",
        "result.original.data()",
        "protocol::kPhysicsBackendVptrRva",
        "protocol::kOriginalBoundaryCallbackRva",
        "expected_first_caller_return",
        "expected_second_caller_return",
        "MarkPayloadStaged",
        "MarkConfigurationPublished",
        "MarkReplayComplete",
        "MarkRolledBack",
        "AllZero(recording_sha256, 32)",
    ):
        require(token in combined, f"missing host transaction contract: {token}")
    require(header.index("result.unpublished_control.flags = 0") <
            header.index("result.published_control.flags ="),
            "configuration is not represented as publish-last")

    for token in ("PTRACE_", "process_vm_", "/proc/", "pread(", "pwrite(",
                  "kill(", "waitpid(", "RemoteCall"):
        require(token not in combined, f"pure transaction gained runtime primitive: {token}")
    for token in ("O_RDWR", "pwrite(", "process_vm_write", "PTRACE_POKE"):
        require(token not in resolver, f"read-only resolver gained write primitive: {token}")
    for token in ("kBoundaryRva = 0x1DD0", "kShadowLocatorRva = 0x50C0",
                  "kEvidenceLocatorRva = 0x50E0", "detail::HashFile",
                  "kFinalRwOffset = 0x1F40",
                  "kFinalRwVaddr = 0x4F40",
                  "kFinalRwFileSize = 0x380",
                  "kFinalRwMemorySize = 0x7EC90",
                  "kFinalRwMemoryEndRva = 0x83BD0",
                  "kBoundarySize = 0x484",
                  "kControlStorageRva = 0x4F40",
                  "kEvidenceStorageRva = 0x5000",
                  "kShadowStorageRva = 0x5100",
                  "kAuditsStorageRva = 0x52C0",
                  "kTargetsStorageRva = 0x75AC0",
                  "BuildLoadPlan", "ReadLoadPlan",
                   "DeriveAndValidateLoadBias", "ValidateFinalRwSplit",
                   "PrivateAnonymousRw", "FileBackedRwRange",
                   "ExecutableRange", "WritableRange",
                   "ValidateStorageLayout",
                   "mapping.path.empty() || mapping.path == \"[anon:.bss]\"",
                   "mapping->perms[2] == '-' || mapping->perms[2] == 'x'",
                   "ResolveLocatorStorage", "all_zero || all_relocated",
                   "VerifyLiveFileBytes", "ReadProcessStartTime",
                   "SamePayloadCluster", "SameIdentity",
                   "HashFile(file.get(), final_hash)",
                   "S_ISREG(info.st_mode)", "major(info.st_dev)",
                   "minor(info.st_dev)"):
        require(token in resolver, f"missing resolver identity check: {token}")
    require(resolver.count("VerifyLiveFileBytes(file.get(), process_mem") == 2,
            "wrapper bytes must be checked before and after resolver reads")

    for token in (
        "7000004000-7000006000 rw-p 00001000 fd:01 424242",
        "7000006000-7000084000 rw-p 00000000 00:00 0 [anon:.bss]",
        "PositiveSplit", "RejectProgramHeaderDrift", "RejectMappingDrift",
        "RejectPathSpoof", "logical_end_83bd0",
        "exact_nonoverlap_storage", "boundary_full_484_guest_code",
        "native_rx", "houdini_ro", "locator_zero_or_relocated",
        "pid_start_time",
    ):
        require(token in resolver_selftest,
                f"missing resolver split-map selftest: {token}")

    require(hashlib.sha256(payload.read_bytes()).digest() == pinned_hash(resolver),
            "resolver SHA-256 pin does not match payload")
    validate_payload_program_headers(payload)
    validate_locator_relocations(readelf, payload)
    mapping_model_output = run(sys.executable, str(MAPPING_MODEL))
    require("BARREL_YAW_TAIL_RESOLVER_MAPPING_MODEL passed=1" in
            mapping_model_output,
            "host-executed resolver mapping model did not pass")
    payload_symbols = run(str(readelf), "-Ws", str(payload))
    require(re.search(
        r"\b0000000000001dd0\s+1156\s+FUNC\s+GLOBAL\s+DEFAULT\s+\d+\s+"
        r"a9tas_barrel_yaw_tail_boundary_v1\b", payload_symbols) is not None,
        "payload boundary symbol is not exact RVA 0x1dd0 size 0x484")
    for artifact in (transaction, resolver_artifact):
        elf = run(str(readelf), "-h", str(artifact))
        require("Machine:" in elf and "X86-64" in elf,
                f"unexpected build architecture: {artifact}")
    transaction_symbols = run(str(readelf), "-Ws", str(transaction))
    require("BARREL_YAW_HOST_TRANSACTION_SELFTEST" in
            run(str(readelf), "-p", ".rodata", str(transaction)),
            "transaction selftest receipt missing")
    require("BARREL_YAW_TAIL_ELF_RESOLVER_SELFTEST" in
            run(str(readelf), "-p", ".rodata", str(resolver_artifact)),
            "resolver split-map selftest receipt missing")
    for forbidden in ("ptrace", "pwrite", "process_vm_write", "kill", "waitpid"):
        require(forbidden not in transaction_symbols,
                f"pure transaction imports forbidden symbol: {forbidden}")

    print(
        "BARREL_YAW_HOST_TRANSACTION_POLICY passed=1 resolver_hash_pinned=1 "
        "resolver_phdr_pinned=1 locator_relocations_pinned=1 "
        "rw_file_anon_split=1 named_bss_exact=1 dev_inode_path=1 "
        "bss_contiguous_private_no_x=1 boundary_full_484_guest_code=1 "
        "native_rx_or_houdini_ro=1 locator_dual_mode=1 pid_identity_twice=1 "
        "live_wrapper_bytes_twice=1 exact_nonoverlap_storage=1 "
        "config_publish_last=1 transient_shadow=1 exact_build_vtable=1 "
        "zero_recording_hash_rejected=1 disarmed_restore=1 runtime=disabled "
        "device_access=0 game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"BARREL_YAW_HOST_TRANSACTION_POLICY passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
