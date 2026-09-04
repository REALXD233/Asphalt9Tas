#!/usr/bin/env python3
"""Strict parser for a stable P1 physics-executor affinity report snapshot."""

from __future__ import annotations

import argparse
import collections
import struct
from pathlib import Path


MAGIC = b"A9PEA1\0\0"
VERSION = 1
HEADER_SIZE = 128
EVENT_SIZE = 56
PASSIVE_BUILD = 1 << 0
RUNTIME_ARMING_COMPILED = 1 << 1
INSTALLED = 1 << 2
ATOMIC_BRANCH_PATCH = 1 << 3
KNOWN_REPORT_FLAGS = (
    PASSIVE_BUILD | RUNTIME_ARMING_COMPILED | INSTALLED | ATOMIC_BRANCH_PATCH
)
TOKEN_NULL = 1 << 0
TOKEN_ZERO = 1 << 1
KNOWN_EVENT_FLAGS = TOKEN_NULL | TOKEN_ZERO

HEADER = struct.Struct("<8s6I4Q4I6Q")
EVENT = struct.Struct("<5Q2IQ")
assert HEADER.size == HEADER_SIZE
assert EVENT.size == EVENT_SIZE


def decode_report(blob: bytes, require_installed: bool = False) -> dict[str, object]:
    if len(blob) < HEADER_SIZE:
        raise ValueError("report shorter than header")
    values = HEADER.unpack_from(blob)
    (magic, version, header_size, event_size, capacity, flags, reserved,
     events, dropped, first_ns, last_ns, first_tid, last_tid, tid_changes,
     null_tokens, zero_tokens, nonzero_tokens, first_token, last_token,
     guest_base, target) = values
    if (magic, version, header_size, event_size) != (
            MAGIC, VERSION, HEADER_SIZE, EVENT_SIZE):
        raise ValueError("unsupported report identity/ABI")
    if capacity == 0 or capacity > 1_000_000:
        raise ValueError("invalid report capacity")
    if len(blob) != HEADER_SIZE + capacity * EVENT_SIZE:
        raise ValueError("report length does not match fixed capacity")
    if flags & ~KNOWN_REPORT_FLAGS or reserved:
        raise ValueError("unknown report flags/reserved bits")
    passive = bool(flags & PASSIVE_BUILD)
    runtime = bool(flags & RUNTIME_ARMING_COMPILED)
    installed = bool(flags & INSTALLED)
    atomic_patch = bool(flags & ATOMIC_BRANCH_PATCH)
    if passive == runtime:
        raise ValueError("report must identify exactly one build mode")
    if installed and not runtime:
        raise ValueError("installed report is not a runtime build")
    if atomic_patch and not installed:
        raise ValueError("atomic-patch proof present without install")
    if require_installed and not flags & INSTALLED:
        raise ValueError("installed live-capture flag missing")
    if require_installed and not atomic_patch:
        raise ValueError("installed report lacks atomic-branch patch proof")
    if events == 0:
        raise ValueError("report contains no executor events")
    committed = min(events, capacity)
    if dropped != events - committed:
        raise ValueError("dropped counter mismatch")
    if null_tokens + zero_tokens + nonzero_tokens != events:
        raise ValueError("token counters do not sum to events")

    tids: collections.Counter[int] = collections.Counter()
    null_count = zero_count = nonzero_count = 0
    records: list[tuple[int, int, int]] = []
    for index in range(committed):
        offset = HEADER_SIZE + index * EVENT_SIZE
        (sequence, monotonic_ns, token_us, context, output, tid,
         event_flags, commit_sequence) = EVENT.unpack_from(blob, offset)
        if sequence != index or commit_sequence != index + 1:
            raise ValueError("event sequence/commit mismatch")
        if monotonic_ns == 0 or context == 0 or output == 0 or tid == 0:
            raise ValueError("zero event identity field")
        if event_flags & ~KNOWN_EVENT_FLAGS:
            raise ValueError("unknown event flag")
        is_null = bool(event_flags & TOKEN_NULL)
        is_zero = bool(event_flags & TOKEN_ZERO)
        if is_null and is_zero:
            raise ValueError("null token also marked zero")
        if is_null:
            if token_us != 0:
                raise ValueError("null token has nonzero value")
            null_count += 1
        elif is_zero:
            if token_us != 0:
                raise ValueError("zero token flag has nonzero value")
            zero_count += 1
        else:
            if token_us == 0:
                raise ValueError("zero token missing flag")
            nonzero_count += 1
        tids[tid] += 1
        records.append((monotonic_ns, tid, token_us))

    # When capacity is not exceeded, all header summaries must match exact
    # committed data. P1 uses a bounded short capture and must satisfy this.
    if events <= capacity:
        if (null_count, zero_count, nonzero_count) != (
                null_tokens, zero_tokens, nonzero_tokens):
            raise ValueError("token summary mismatch")
        if (first_ns, first_tid, first_token) != (
                records[0][0], records[0][1], records[0][2]):
            raise ValueError("first summary mismatch")
    if len(tids) == 1:
        if (last_ns, last_tid, last_token) != (
                records[-1][0], records[-1][1], records[-1][2]):
            raise ValueError("last summary mismatch")
        if tid_changes != 0:
            raise ValueError("single-TID report has TID-change counter")
        if any(records[i][0] > records[i + 1][0]
               for i in range(len(records) - 1)):
            raise ValueError("single-TID timestamps are not monotonic")
    if require_installed and (guest_base == 0 or target == 0):
        raise ValueError("installed report lacks game target identity")

    return {
        "events": events,
        "committed": committed,
        "dropped": dropped,
        "tids": tuple(sorted(tids)),
        "unique_tid": len(tids) == 1,
        "zero_tokens": zero_tokens,
        "nonzero_tokens": nonzero_tokens,
        "null_tokens": null_tokens,
        "guest_base": guest_base,
        "target": target,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--require-installed", action="store_true")
    args = parser.parse_args()
    try:
        summary = decode_report(args.report.read_bytes(), args.require_installed)
    except (OSError, ValueError, struct.error) as error:
        print(f"a9pea1_error={error}")
        return 1
    print(
        "a9pea1_supported=1 "
        f"events={summary['events']} committed={summary['committed']} "
        f"dropped={summary['dropped']} "
        f"tids={','.join(map(str, summary['tids']))} "
        f"unique_tid={int(summary['unique_tid'])} "
        f"zero_tokens={summary['zero_tokens']} "
        f"nonzero_tokens={summary['nonzero_tokens']} "
        f"null_tokens={summary['null_tokens']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
