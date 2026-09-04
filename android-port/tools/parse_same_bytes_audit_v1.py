#!/usr/bin/env python3
"""Fail-closed parser for A9SBT1 same-bytes transport audit reports."""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"A9SBT1\0\0"
VERSION = 1
SUPPORTED_BUILD_ID = bytes.fromhex("e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b")
REQUIRED_FLAGS = 0x1F
NATIVE_BODY_SIZE = 0x290
WRAPPER_AUDIT_SIZE = 0x48
SNAPSHOT_SIZE = 804
HEADER = struct.Struct("<8sIIII20sI15QII")

assert HEADER.size == 176


@dataclass(frozen=True)
class AuditHeader:
    flags: int
    pid: int
    library_base: int
    physics_context: int
    car_physics_state: int
    wrapper: int
    native_body: int
    transform_address: int
    linear_address: int
    event_count: int
    read_errors: int
    ptrace_errors: int
    semantic_errors: int
    write_attempts: int
    write_failures: int
    thread_additions: int
    initial_threads: int
    final_threads: int


def _finite_payload(snapshot: bytes) -> bool:
    native = snapshot[:NATIVE_BODY_SIZE]
    raw = native[0x10:0x50] + native[0x150:0x15C]
    return all(
        math.isfinite(value) and abs(value) <= 1_000_000.0
        for (value,) in struct.iter_unpack("<f", raw)
    )


def read_audit(path: Path) -> tuple[AuditHeader, bytes, bytes, bytes]:
    blob = path.read_bytes()
    expected = HEADER.size + 3 * SNAPSHOT_SIZE
    if len(blob) != expected:
        raise ValueError(f"audit length must be exactly {expected} bytes")
    values = HEADER.unpack_from(blob)
    magic, version, header_size, snapshot_size, flags, build_id, _reserved = values[:7]
    if magic != MAGIC or version != VERSION:
        raise ValueError("unsupported A9SBT1 magic/version")
    if header_size != HEADER.size or snapshot_size != SNAPSHOT_SIZE:
        raise ValueError("A9SBT1 ABI size mismatch")
    if build_id != SUPPORTED_BUILD_ID:
        raise ValueError("A9SBT1 build ID mismatch")
    header = AuditHeader(flags, *values[7:])
    before_start = HEADER.size
    immediate_start = before_start + SNAPSHOT_SIZE
    next_start = immediate_start + SNAPSHOT_SIZE
    return (
        header,
        blob[before_start:immediate_start],
        blob[immediate_start:next_start],
        blob[next_start:],
    )


def assess(
    header: AuditHeader, before: bytes, immediate: bytes, next_cycle: bytes
) -> list[str]:
    problems: list[str] = []
    if header.flags != REQUIRED_FLAGS:
        problems.append(f"flags 0x{header.flags:x} != required 0x{REQUIRED_FLAGS:x}")
    if header.pid <= 0 or header.library_base == 0 or header.physics_context == 0:
        problems.append("invalid runtime identity fields")
    if header.native_body == 0 or header.wrapper == 0:
        problems.append("invalid wrapper/native identity")
    if header.transform_address != header.native_body + 0x10:
        problems.append("transform address is not native+0x10")
    if header.linear_address != header.native_body + 0x150:
        problems.append("linear address is not native+0x150")
    if header.event_count == 0:
        problems.append("no certified events were observed")
    if header.read_errors or header.ptrace_errors or header.semantic_errors:
        problems.append(
            "nonzero errors: "
            f"read={header.read_errors} ptrace={header.ptrace_errors} "
            f"semantic={header.semantic_errors}"
        )
    if header.write_attempts != 1 or header.write_failures != 0:
        problems.append(
            f"write counters attempts={header.write_attempts} "
            f"failures={header.write_failures}"
        )
    if header.final_threads != header.initial_threads + header.thread_additions:
        problems.append("not every attached thread detached cleanly")
    if before != immediate:
        first = next(
            index
            for index, (left, right) in enumerate(
                zip(before, immediate, strict=True)
            )
            if left != right
        )
        problems.append(f"immediate audit changed at snapshot byte {first}")
    if not _finite_payload(before):
        problems.append("before payload is non-finite/out-of-range")
    if not _finite_payload(next_cycle):
        problems.append("next-cycle payload is non-finite/out-of-range")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("audit", type=Path)
    parser.add_argument("--require-supported", action="store_true")
    args = parser.parse_args()
    try:
        header, before, immediate, next_cycle = read_audit(args.audit)
        problems = assess(header, before, immediate, next_cycle)
    except (OSError, ValueError, struct.error) as error:
        print(f"same_bytes_audit_error={error}")
        return 2
    supported = not problems
    print(
        f"events={header.event_count} writes={header.write_attempts} "
        f"write_failures={header.write_failures} "
        f"threads={header.initial_threads}+{header.thread_additions}"
        f"->{header.final_threads} supported={int(supported)}"
    )
    print(
        f"native=0x{header.native_body:x} transform=0x{header.transform_address:x} "
        f"linear=0x{header.linear_address:x} exact_pre_post={int(before == immediate)}"
    )
    for problem in problems:
        print(f"reject={problem}")
    return 0 if supported or not args.require_supported else 1


if __name__ == "__main__":
    raise SystemExit(main())
