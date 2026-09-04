#!/usr/bin/env python3
"""Strict parser for the observation-only NitroService thread-affinity probe."""

from __future__ import annotations

import argparse
import collections
import struct
from pathlib import Path


MAGIC = b"A9NTA1\0\0"
VERSION = 1
HEADER_SIZE = 224
EVENT_SIZE = 72
TARGET_VERIFIED = 1 << 0
CLEAN_DETACH = 1 << 1
CAPTURED_DELTA = 1 << 2
CAPTURED_NITRO = 1 << 3
HIT_DELTA = 1 << 0
HIT_ACTIVE = 1 << 1
HIT_MODE = 1 << 2
HIT_ENCRYPTED = 1 << 3
ALL_EVENT_FLAGS = 0xF

_SNAPSHOT = struct.Struct("<BB5sBII3I")
_HEADER_PREFIX = struct.Struct("<8s4I17Q2I")
_EVENT_PREFIX = struct.Struct("<QQiIQq")
assert _SNAPSHOT.size == 28
assert _HEADER_PREFIX.size + 2 * _SNAPSHOT.size == HEADER_SIZE
assert _EVENT_PREFIX.size + _SNAPSHOT.size + 4 == EVENT_SIZE


def _snapshot(blob: bytes, offset: int):
    values = _SNAPSHOT.unpack_from(blob, offset)
    optional, active, gates, reserved = values[:4]
    if optional not in (0, 1) or active not in (0, 1):
        raise ValueError("noncanonical Nitro bool state")
    if reserved != 0 or any(value not in (0, 1) for value in gates):
        raise ValueError("noncanonical Nitro gates/reserved state")
    return values


def decode_report(blob: bytes) -> dict[str, object]:
    if len(blob) < HEADER_SIZE:
        raise ValueError("report shorter than A9NTA1 header")
    header = _HEADER_PREFIX.unpack_from(blob)
    (
        magic,
        version,
        header_size,
        event_size,
        flags,
        pid,
        library_base,
        main_object,
        delta_address,
        final_owner,
        service_object,
        state_address,
        start_ns,
        event_count,
        delta_hits,
        active_hits,
        mode_hits,
        encrypted_hits,
        read_errors,
        ptrace_errors,
        thread_additions,
        unexpected_stops,
        initial_threads,
        final_threads,
    ) = header
    if (magic, version, header_size, event_size) != (
        MAGIC,
        VERSION,
        HEADER_SIZE,
        EVENT_SIZE,
    ):
        raise ValueError("unsupported A9NTA1 magic/version/ABI")
    if len(blob) != HEADER_SIZE + event_count * EVENT_SIZE:
        raise ValueError("A9NTA1 report length mismatch")
    if flags & 0xF != 0xF:
        raise ValueError("target/clean/delta/nitro capture flags missing")
    if not all((pid, library_base, main_object, delta_address,
                final_owner, service_object, state_address, start_ns)):
        raise ValueError("A9NTA1 identity field is zero")
    if read_errors or ptrace_errors or unexpected_stops:
        raise ValueError("A9NTA1 runtime error counters are nonzero")
    if initial_threads == 0 or final_threads != initial_threads + thread_additions:
        raise ValueError("A9NTA1 clean-detach thread accounting mismatch")
    _snapshot(blob, _HEADER_PREFIX.size)
    _snapshot(blob, _HEADER_PREFIX.size + _SNAPSHOT.size)

    counts = collections.Counter()
    delta_tids: collections.Counter[int] = collections.Counter()
    nitro_tids: collections.Counter[int] = collections.Counter()
    previous_ns = 0
    cursor = HEADER_SIZE
    for index in range(event_count):
        sequence, monotonic_ns, tid, event_flags, rip, delta_value = (
            _EVENT_PREFIX.unpack_from(blob, cursor)
        )
        if sequence != index or monotonic_ns < previous_ns or tid <= 0:
            raise ValueError("A9NTA1 event sequence/time/tid mismatch")
        if event_flags == 0 or event_flags & ~ALL_EVENT_FLAGS or rip == 0:
            raise ValueError("A9NTA1 event flags/rip mismatch")
        _snapshot(blob, cursor + _EVENT_PREFIX.size)
        (read_ok,) = struct.unpack_from("<I", blob, cursor + EVENT_SIZE - 4)
        if read_ok != 1:
            raise ValueError("A9NTA1 event read failed")
        if event_flags & HIT_DELTA:
            counts["delta"] += 1
            if delta_value != 0:
                delta_tids[tid] += 1
        if event_flags & HIT_ACTIVE:
            counts["active"] += 1
            nitro_tids[tid] += 1
        if event_flags & HIT_MODE:
            counts["mode"] += 1
            nitro_tids[tid] += 1
        if event_flags & HIT_ENCRYPTED:
            counts["encrypted"] += 1
            nitro_tids[tid] += 1
        previous_ns = monotonic_ns
        cursor += EVENT_SIZE
    if tuple(counts[key] for key in ("delta", "active", "mode", "encrypted")) != (
        delta_hits,
        active_hits,
        mode_hits,
        encrypted_hits,
    ):
        raise ValueError("A9NTA1 event counters mismatch")
    if not delta_tids:
        raise ValueError("no naturally running nonzero delta writer captured")
    if not nitro_tids:
        raise ValueError("no Nitro state writer captured")
    delta_tid_set = set(delta_tids)
    nitro_tid_set = set(nitro_tids)
    return {
        "events": event_count,
        "delta_hits": delta_hits,
        "nitro_hits": active_hits + mode_hits + encrypted_hits,
        "delta_tids": tuple(sorted(delta_tid_set)),
        "nitro_tids": tuple(sorted(nitro_tid_set)),
        "nitro_tids_are_delta_writers": nitro_tid_set <= delta_tid_set,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        summary = decode_report(args.report.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9nta1_error={error}")
        return 1
    print(
        "a9nta1_supported=1 "
        f"events={summary['events']} delta_hits={summary['delta_hits']} "
        f"nitro_hits={summary['nitro_hits']} "
        f"delta_tids={','.join(map(str, summary['delta_tids']))} "
        f"nitro_tids={','.join(map(str, summary['nitro_tids']))} "
        "nitro_tids_are_delta_writers="
        f"{int(summary['nitro_tids_are_delta_writers'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
