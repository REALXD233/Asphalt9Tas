#!/usr/bin/env python3
"""Validate and summarize A9TAS v5 passive HWBP event traces."""

from __future__ import annotations

import argparse
import collections
import math
import pathlib
import statistics
import struct
import sys
from dataclasses import dataclass


HEADER = struct.Struct("<8sIIII13QII24s")
EVENT = struct.Struct("<QQiIQIIII")
MAGIC = b"A9HEV5\0\0"


@dataclass(frozen=True)
class EventRecord:
    sequence: int
    monotonic_ns: int
    tid: int
    flags: int
    rip: int
    c98_bits: int
    c9c_bits: int
    read_ok: int

    @property
    def kind(self) -> str:
        return {1: "C98", 2: "C9C", 3: "BOTH"}.get(self.flags & 3, "NONE")


def bits_float(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def infer_tick_pairs(
    events: list[EventRecord],
) -> tuple[list[tuple[EventRecord, EventRecord]], tuple[str, str] | None, list[int], bool]:
    """Infer the within-tick writer order from adjacent transition timings.

    HWBP attachment can begin between the two writes of a tick. A stream may
    therefore alternate perfectly while index-based (0,1)/(2,3) pairing is one
    event out of phase. The within-tick transition must have a materially
    smaller median gap than the opposite, between-tick transition.
    """
    directions = (("C98", "C9C"), ("C9C", "C98"))
    gaps: dict[tuple[str, str], list[int]] = {direction: [] for direction in directions}
    for left, right in zip(events, events[1:]):
        direction = (left.kind, right.kind)
        if direction in gaps:
            gaps[direction].append(right.monotonic_ns - left.monotonic_ns)
    if not gaps[directions[0]] or not gaps[directions[1]]:
        return [], None, list(range(len(events))), False
    medians = {direction: statistics.median(values) for direction, values in gaps.items()}
    order = min(directions, key=lambda direction: medians[direction])
    opposite = directions[1] if order == directions[0] else directions[0]
    confident = medians[opposite] >= max(1, medians[order]) * 2

    pairs: list[tuple[EventRecord, EventRecord]] = []
    unpaired: list[int] = []
    index = 0
    while index < len(events):
        if (
            index + 1 < len(events)
            and events[index].kind == order[0]
            and events[index + 1].kind == order[1]
        ):
            pairs.append((events[index], events[index + 1]))
            index += 2
        else:
            unpaired.append(index)
            index += 1
    return pairs, order, unpaired, confident


def parse_trace(path: pathlib.Path) -> tuple[dict[str, int], list[EventRecord]]:
    data = path.read_bytes()
    if len(data) < HEADER.size:
        raise ValueError(f"short file: {len(data)} bytes")
    raw = HEADER.unpack_from(data)
    if raw[0] != MAGIC:
        raise ValueError(f"bad magic: {raw[0]!r}")
    header = {
        "version": raw[1],
        "header_size": raw[2],
        "event_size": raw[3],
        "flags": raw[4],
        "pid": raw[5],
        "library_base": raw[6],
        "final_owner": raw[7],
        "c98_address": raw[8],
        "c9c_address": raw[9],
        "start_ns": raw[10],
        "event_count": raw[11],
        "c98_hits": raw[12],
        "c9c_hits": raw[13],
        "read_errors": raw[14],
        "ptrace_errors": raw[15],
        "thread_additions": raw[16],
        "unexpected_stops": raw[17],
        "initial_threads": raw[18],
        "final_threads": raw[19],
    }
    if header["version"] != 5:
        raise ValueError(f"unsupported version: {header['version']}")
    if header["header_size"] != HEADER.size or header["event_size"] != EVENT.size:
        raise ValueError(
            f"ABI mismatch: header={header['header_size']}/{HEADER.size} "
            f"event={header['event_size']}/{EVENT.size}"
        )
    expected = HEADER.size + header["event_count"] * EVENT.size
    if len(data) != expected:
        raise ValueError(f"length mismatch: actual={len(data)} expected={expected}")

    records: list[EventRecord] = []
    offset = HEADER.size
    for _ in range(header["event_count"]):
        row = EVENT.unpack_from(data, offset)
        records.append(EventRecord(*row[:8]))
        offset += EVENT.size
    return header, records


def summarize(header: dict[str, int], events: list[EventRecord]) -> dict[str, object]:
    kinds = collections.Counter(event.kind for event in events)
    tids = collections.Counter(event.tid for event in events)
    rips = collections.Counter(event.rip for event in events)
    transitions = collections.Counter(
        (left.kind, right.kind) for left, right in zip(events, events[1:])
    )
    sequence_errors = [
        (index, event.sequence)
        for index, event in enumerate(events)
        if event.sequence != index
    ]
    invalid_reads = [event.sequence for event in events if event.read_ok != 1]
    invalid_floats = [
        event.sequence
        for event in events
        if event.read_ok == 1
        and (
            not math.isfinite(bits_float(event.c98_bits))
            or not math.isfinite(bits_float(event.c9c_bits))
            or abs(bits_float(event.c98_bits)) > 8.0
            or abs(bits_float(event.c9c_bits)) > 8.0
        )
    ]
    invalid_flags = [
        event.sequence for event in events if (event.flags & 3) not in (1, 2, 3)
    ]
    timestamp_errors = [
        right.sequence
        for left, right in zip(events, events[1:])
        if right.monotonic_ns < left.monotonic_ns
    ]
    header_count_errors = []
    if header["c98_hits"] != sum(bool(event.flags & 1) for event in events):
        header_count_errors.append("c98_hits")
    if header["c9c_hits"] != sum(bool(event.flags & 2) for event in events):
        header_count_errors.append("c9c_hits")

    strict_pairs = 0
    pair_orders: collections.Counter[tuple[str, str]] = collections.Counter()
    pair_intervals_ms: list[float] = []
    for index in range(0, len(events) - 1, 2):
        first, second = events[index], events[index + 1]
        pair_orders[(first.kind, second.kind)] += 1
        if {first.kind, second.kind} == {"C98", "C9C"}:
            strict_pairs += 1
            if index >= 2:
                pair_intervals_ms.append(
                    (first.monotonic_ns - events[index - 2].monotonic_ns) / 1e6
                )

    dominant_pair = pair_orders.most_common(1)[0] if pair_orders else (None, 0)
    inferred_pairs, inferred_order, unpaired_events, pairing_confident = (
        infer_tick_pairs(events)
    )
    return {
        "kinds": kinds,
        "tids": tids,
        "rips": rips,
        "transitions": transitions,
        "sequence_errors": sequence_errors,
        "invalid_reads": invalid_reads,
        "invalid_floats": invalid_floats,
        "invalid_flags": invalid_flags,
        "timestamp_errors": timestamp_errors,
        "header_count_errors": header_count_errors,
        "pair_orders": pair_orders,
        "strict_pairs": strict_pairs,
        "pair_count": len(events) // 2,
        "dominant_pair": dominant_pair,
        "pair_interval_median_ms": (
            statistics.median(pair_intervals_ms) if pair_intervals_ms else None
        ),
        "pair_interval_mean_ms": (
            statistics.fmean(pair_intervals_ms) if pair_intervals_ms else None
        ),
        "header_clean": bool(header["flags"] & 1),
        "target_signature_verified": bool(header["flags"] & 4),
        "inferred_tick_pairs": inferred_pairs,
        "inferred_order": inferred_order,
        "unpaired_events": unpaired_events,
        "pairing_confident": pairing_confident,
    }


def format_counter(counter: collections.Counter, limit: int = 12) -> str:
    if not counter:
        return "none"
    return ", ".join(f"{key}={value}" for key, value in counter.most_common(limit))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=pathlib.Path)
    parser.add_argument(
        "--strict-pairs",
        action="store_true",
        help="fail unless the complete stream consists of alternating C98/C9C pairs",
    )
    args = parser.parse_args(argv)
    try:
        header, events = parse_trace(args.trace)
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    summary = summarize(header, events)

    print(
        f"magic=A9HEV5 version={header['version']} events={len(events)} "
        f"clean={int(summary['header_clean'])} "
        f"target_verified={int(summary['target_signature_verified'])} "
        f"pid={header['pid']}"
    )
    print(
        f"owner=0x{header['final_owner']:x} c98=0x{header['c98_address']:x} "
        f"c9c=0x{header['c9c_address']:x}"
    )
    print(
        "errors: "
        f"read={header['read_errors']} ptrace={header['ptrace_errors']} "
        f"unexpected_stops={header['unexpected_stops']} "
        f"thread_additions={header['thread_additions']}"
    )
    print(f"kinds: {format_counter(summary['kinds'])}")
    print(f"tids: {format_counter(summary['tids'])}")
    print(f"rips: {format_counter(summary['rips'])}")
    print(f"transitions: {format_counter(summary['transitions'])}")
    print(f"pair orders: {format_counter(summary['pair_orders'])}")
    print(
        f"strict complete pairs={summary['strict_pairs']}/{summary['pair_count']} "
        f"dominant={summary['dominant_pair']}"
    )
    print(
        f"timing-inferred pairs={len(summary['inferred_tick_pairs'])} "
        f"order={summary['inferred_order']} confident={int(summary['pairing_confident'])} "
        f"unpaired={summary['unpaired_events'][:12]}"
    )
    if summary["pair_interval_median_ms"] is not None:
        print(
            "pair-start interval ms: "
            f"median={summary['pair_interval_median_ms']:.3f} "
            f"mean={summary['pair_interval_mean_ms']:.3f}"
        )
    print(
        f"integrity: seq_errors={len(summary['sequence_errors'])} "
        f"invalid_reads={len(summary['invalid_reads'])} "
        f"invalid_floats={len(summary['invalid_floats'])} "
        f"invalid_flags={len(summary['invalid_flags'])} "
        f"timestamp_errors={len(summary['timestamp_errors'])} "
        f"header_count_errors={summary['header_count_errors']}"
    )
    print("first events:")
    for event in events[:20]:
        print(
            f"  seq={event.sequence} t_ms={(event.monotonic_ns-header['start_ns'])/1e6:.3f} "
            f"kind={event.kind} tid={event.tid} rip=0x{event.rip:x} "
            f"c98={bits_float(event.c98_bits):+.7g} "
            f"c9c={bits_float(event.c9c_bits):+.7g} read_ok={event.read_ok}"
        )

    integrity_ok = (
        summary["header_clean"]
        and summary["target_signature_verified"]
        and not summary["sequence_errors"]
        and not summary["invalid_reads"]
        and not summary["invalid_floats"]
        and not summary["invalid_flags"]
        and not summary["timestamp_errors"]
        and not summary["header_count_errors"]
    )
    if args.strict_pairs:
        integrity_ok = (
            integrity_ok
            and summary["pairing_confident"]
            and not summary["unpaired_events"]
            and len(summary["inferred_tick_pairs"]) * 2 == len(events)
        )
    return 0 if integrity_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
