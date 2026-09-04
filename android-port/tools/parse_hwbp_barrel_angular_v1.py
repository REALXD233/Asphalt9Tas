#!/usr/bin/env python3
"""Validate and classify a host-only barrel/angular HWBP trace."""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import asdict, dataclass
from pathlib import Path


MAGIC = b"A9BAV1\0\0"
VERSION = 1
CLEAN = 1 << 0
TARGET_VERIFIED = 1 << 1
HIT_HEAD = 1 << 0
HIT_TAIL = 1 << 1
HIT_AUX = 1 << 2
KNOWN_HITS = HIT_HEAD | HIT_TAIL | HIT_AUX
HEADER = struct.Struct("<8sIIII" + "Q" * 20 + "II")
EVENT = struct.Struct("<QQiIQQ4I2I32Q")


@dataclass(frozen=True)
class Summary:
    events: int
    head_hits: int
    tail_hits: int
    aux_hits: int
    candidate_events: int
    candidate_aux_events: int
    worker_events: int
    other_events: int
    tids: list[int]


def parse_trace(path: Path, require_candidate: bool = False) -> Summary:
    blob = path.read_bytes()
    if len(blob) < HEADER.size:
        raise ValueError("trace is shorter than the header")
    header = HEADER.unpack_from(blob)
    magic, version, header_size, event_size, flags = header[:5]
    if magic != MAGIC or version != VERSION:
        raise ValueError("unexpected trace magic/version")
    if header_size != HEADER.size or event_size != EVENT.size:
        raise ValueError("unexpected header/event size")
    if not flags & TARGET_VERIFIED:
        raise ValueError("target signatures were not verified")
    if not flags & CLEAN:
        raise ValueError("trace did not detach cleanly")

    library_base = header[6]
    native_body = header[7]
    angular = header[8]
    angular_tail = header[9]
    angular_aux = header[10]
    candidate_return = header[11]
    worker_returns = set(header[12:15])
    event_count = header[16]
    expected_head, expected_tail, expected_aux = header[17:20]
    value_errors, stack_errors, ptrace_errors = header[20:23]
    thread_additions, unexpected_stops = header[23:25]
    initial_threads, final_threads = header[25:27]
    if not library_base or not native_body or not angular:
        raise ValueError("trace contains a null identity/address")
    if angular_tail != angular + 8 or angular_aux != angular + 12:
        raise ValueError("angular watch addresses are inconsistent")
    if not candidate_return or 0 in worker_returns:
        raise ValueError("classification return addresses are invalid")
    if value_errors or stack_errors or ptrace_errors or unexpected_stops:
        raise ValueError("trace reports read/ptrace/unexpected-stop errors")
    if final_threads != initial_threads + thread_additions:
        raise ValueError("not every attached thread was accounted for")
    expected_size = HEADER.size + event_count * EVENT.size
    if event_count == 0 or len(blob) != expected_size:
        raise ValueError("trace length/event count mismatch")

    head_hits = tail_hits = aux_hits = 0
    candidate_events = candidate_aux_events = worker_events = other_events = 0
    tids: set[int] = set()
    previous_time = -1
    for index in range(event_count):
        event = EVENT.unpack_from(blob, HEADER.size + index * EVENT.size)
        sequence, monotonic_ns, tid, event_flags = event[:4]
        value_read_ok, stack_read_ok = event[10:12]
        stack_words = set(event[12:44])
        if sequence != index:
            raise ValueError(f"non-contiguous sequence at event {index}")
        if monotonic_ns < previous_time:
            raise ValueError(f"time moved backwards at event {index}")
        previous_time = monotonic_ns
        if event_flags == 0 or event_flags & ~KNOWN_HITS:
            raise ValueError(f"invalid hit flags at event {index}")
        if value_read_ok != 1 or stack_read_ok != 1:
            raise ValueError(f"event {index} is missing value/stack data")
        tids.add(tid)
        if event_flags & HIT_HEAD:
            head_hits += 1
        if event_flags & HIT_TAIL:
            tail_hits += 1
        if event_flags & HIT_AUX:
            aux_hits += 1
        is_candidate = candidate_return in stack_words
        is_worker = bool(worker_returns & stack_words)
        if is_candidate and is_worker:
            raise ValueError(f"ambiguous candidate/worker stack at event {index}")
        if is_candidate:
            candidate_events += 1
            if event_flags & HIT_AUX:
                candidate_aux_events += 1
        elif is_worker:
            worker_events += 1
        else:
            other_events += 1

    if (head_hits, tail_hits, aux_hits) != (
        expected_head,
        expected_tail,
        expected_aux,
    ):
        raise ValueError("event hit counts do not match the header")
    if require_candidate and (candidate_events == 0 or candidate_aux_events == 0):
        raise ValueError("no complete BarrelYaw-candidate setter sequence observed")
    return Summary(
        events=event_count,
        head_hits=head_hits,
        tail_hits=tail_hits,
        aux_hits=aux_hits,
        candidate_events=candidate_events,
        candidate_aux_events=candidate_aux_events,
        worker_events=worker_events,
        other_events=other_events,
        tids=sorted(tids),
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--require-candidate", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    try:
        summary = parse_trace(args.trace, args.require_candidate)
    except (OSError, ValueError) as error:
        print(f"REJECT: {error}")
        return 1
    if args.json:
        print(json.dumps(asdict(summary), indent=2))
    else:
        print(
            "PASS "
            f"events={summary.events} head={summary.head_hits} "
            f"tail={summary.tail_hits} aux={summary.aux_hits} "
            f"candidate={summary.candidate_events} "
            f"candidate_aux={summary.candidate_aux_events} "
            f"worker={summary.worker_events} other={summary.other_events} "
            f"tids={','.join(map(str, summary.tids))}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
