#!/usr/bin/env python3
"""Parse and strictly classify A9SCV1 read-only successor traces."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence


MAGIC = b"A9SCV1\x00\x00"
VERSION = 1
STACK_WORDS = 64
FLAG_CLEAN = 1 << 0
FLAG_TARGET_VERIFIED = 1 << 1
FLAG_READ_ONLY = 1 << 2
REQUIRED_HEADER_FLAGS = FLAG_CLEAN | FLAG_TARGET_VERIFIED | FLAG_READ_ONLY

HIT_RBX = 1 << 0
HIT_ANGULAR_AUX = 1 << 1
HIT_C9C = 1 << 2
HIT_F64 = 1 << 3
KNOWN_HITS = HIT_RBX | HIT_ANGULAR_AUX | HIT_C9C | HIT_F64

MARKER_RVAS = {
    "rbx_caller": 0x369CC08,
    "yaw_candidate": 0x369E45C,
    "rbx_internal_angular": 0x369E044,
}

GPR_NAMES = (
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9",
    "r10", "r11", "r12", "r13", "r14", "r15",
)

HEADER_STRUCT = struct.Struct("<8s6I26Q2I")
EVENT_STRUCT = struct.Struct("<QQiI20Q12I64Q")
assert HEADER_STRUCT.size == 248
assert EVENT_STRUCT.size == 744


class TraceError(ValueError):
    """Trace is malformed or does not satisfy the classifier contract."""


@dataclass(frozen=True)
class Header:
    magic: bytes
    version: int
    header_size: int
    event_size: int
    flags: int
    stack_word_count: int
    reserved: int
    pid: int
    library_base: int
    final_owner: int
    rbx_owner: int
    physics_base: int
    native_body: int
    native_angular: int
    rbx_watch_address: int
    angular_aux_watch_address: int
    c9c_watch_address: int
    f64_watch_address: int
    rbx_caller_return: int
    yaw_candidate_return: int
    rbx_internal_angular_return: int
    start_ns: int
    event_count: int
    slot_hits: tuple[int, int, int, int]
    value_read_errors: int
    stack_read_errors: int
    register_read_errors: int
    ptrace_errors: int
    thread_additions: int
    unexpected_stops: int
    initial_threads: int
    final_threads: int


@dataclass(frozen=True)
class Event:
    sequence: int
    monotonic_ns: int
    tid: int
    flags: int
    dr6: int
    rip: int
    rsp: int
    rbp: int
    rflags: int
    orig_rax: int
    gpr: dict[str, int]
    rbx_words: tuple[int, int]
    angular_words: tuple[int, int, int, int]
    c9c_bits: int
    f64_bits: int
    value_read_ok: int
    stack_read_ok: int
    register_read_ok: int
    reserved: int
    stack_words: tuple[int, ...]


@dataclass(frozen=True)
class MarkerHit:
    event_sequence: int
    tid: int
    event_flags: int
    marker: str
    address: int
    stack_index: int
    neighbor_start_index: int
    neighbor_words: tuple[int, ...]
    neighbor_fingerprint_sha256: str


@dataclass(frozen=True)
class PhaseWindow:
    tid: int
    c9c_sequence: int
    f64_sequence: int
    candidate_sequences: tuple[int, ...]
    candidate_markers: tuple[str, ...]
    marker_hits: tuple[MarkerHit, ...]

    @property
    def has_rbx(self) -> bool:
        return "rbx_caller" in self.candidate_markers

    @property
    def has_angular(self) -> bool:
        # 0x369E044 is deliberately a negative/non-Yaw control: it is the
        # angular setter inside RBX.  Only 0x369E45C certifies BarrelYaw.
        return "yaw_candidate" in self.candidate_markers

    @property
    def has_positive_candidate(self) -> bool:
        # The RBX-internal angular return is retained as a required negative
        # control, but it must never make an otherwise empty phase window pass
        # the default successor Gate.
        return self.has_rbx or self.has_angular

    @property
    def has_all_markers(self) -> bool:
        return set(self.candidate_markers) >= set(MARKER_RVAS)


@dataclass(frozen=True)
class Analysis:
    header: Header
    events: tuple[Event, ...]
    marker_hits: tuple[MarkerHit, ...]
    windows: tuple[PhaseWindow, ...]


def _unpack_header(values: Sequence[int | bytes]) -> Header:
    q = list(values[7:33])
    return Header(
        magic=values[0],
        version=int(values[1]),
        header_size=int(values[2]),
        event_size=int(values[3]),
        flags=int(values[4]),
        stack_word_count=int(values[5]),
        reserved=int(values[6]),
        pid=q[0],
        library_base=q[1],
        final_owner=q[2],
        rbx_owner=q[3],
        physics_base=q[4],
        native_body=q[5],
        native_angular=q[6],
        rbx_watch_address=q[7],
        angular_aux_watch_address=q[8],
        c9c_watch_address=q[9],
        f64_watch_address=q[10],
        rbx_caller_return=q[11],
        yaw_candidate_return=q[12],
        rbx_internal_angular_return=q[13],
        start_ns=q[14],
        event_count=q[15],
        slot_hits=tuple(q[16:20]),
        value_read_errors=q[20],
        stack_read_errors=q[21],
        register_read_errors=q[22],
        ptrace_errors=q[23],
        thread_additions=q[24],
        unexpected_stops=q[25],
        initial_threads=int(values[33]),
        final_threads=int(values[34]),
    )


def _unpack_event(values: Sequence[int]) -> Event:
    # Prefix: QQ i I, then dr6 + five fixed registers + fourteen GPRs.
    register_values = values[4:24]
    integer_values = values[24:36]
    return Event(
        sequence=values[0],
        monotonic_ns=values[1],
        tid=values[2],
        flags=values[3],
        dr6=register_values[0],
        rip=register_values[1],
        rsp=register_values[2],
        rbp=register_values[3],
        rflags=register_values[4],
        orig_rax=register_values[5],
        gpr=dict(zip(GPR_NAMES, register_values[6:20], strict=True)),
        rbx_words=tuple(integer_values[0:2]),
        angular_words=tuple(integer_values[2:6]),
        c9c_bits=integer_values[6],
        f64_bits=integer_values[7],
        value_read_ok=integer_values[8],
        stack_read_ok=integer_values[9],
        register_read_ok=integer_values[10],
        reserved=integer_values[11],
        stack_words=tuple(values[36:100]),
    )


def read_trace(path: Path | str) -> tuple[Header, tuple[Event, ...]]:
    data = Path(path).read_bytes()
    if len(data) < HEADER_STRUCT.size:
        raise TraceError("trace is shorter than the A9SCV1 header")
    header = _unpack_header(HEADER_STRUCT.unpack_from(data, 0))
    _validate_header(header)
    expected_size = header.header_size + header.event_count * header.event_size
    if len(data) != expected_size:
        raise TraceError(
            f"trace size mismatch: actual={len(data)} expected={expected_size}"
        )
    events = tuple(
        _unpack_event(EVENT_STRUCT.unpack_from(data, header.header_size +
                                               index * header.event_size))
        for index in range(header.event_count)
    )
    _validate_events(header, events)
    return header, events


def _validate_header(header: Header) -> None:
    if header.magic != MAGIC:
        raise TraceError(f"bad magic: {header.magic!r}")
    if header.version != VERSION:
        raise TraceError(f"unsupported version: {header.version}")
    if header.header_size != HEADER_STRUCT.size:
        raise TraceError(f"bad header size: {header.header_size}")
    if header.event_size != EVENT_STRUCT.size:
        raise TraceError(f"bad event size: {header.event_size}")
    if header.stack_word_count != STACK_WORDS:
        raise TraceError(f"bad stack word count: {header.stack_word_count}")
    if header.reserved != 0:
        raise TraceError("nonzero header reserved field")
    if header.flags & REQUIRED_HEADER_FLAGS != REQUIRED_HEADER_FLAGS:
        raise TraceError(
            "trace is not clean, target-verified, and read-only: "
            f"flags=0x{header.flags:x}"
        )
    error_fields = {
        "value_read_errors": header.value_read_errors,
        "stack_read_errors": header.stack_read_errors,
        "register_read_errors": header.register_read_errors,
        "ptrace_errors": header.ptrace_errors,
        "unexpected_stops": header.unexpected_stops,
    }
    nonzero = {name: value for name, value in error_fields.items() if value}
    if nonzero:
        raise TraceError(f"nonzero capture error counters: {nonzero}")
    if header.event_count == 0:
        raise TraceError("trace contains no events")
    if header.initial_threads == 0:
        raise TraceError("no initially attached threads")
    if header.final_threads != header.initial_threads + header.thread_additions:
        raise TraceError("thread lifecycle accounting mismatch")
    if header.angular_aux_watch_address != header.native_angular + 0x0C:
        raise TraceError("DR1 does not equal native angular + 0x0C")
    if header.rbx_watch_address != header.rbx_owner + 0x1968:
        raise TraceError("DR0 does not equal RBX owner + 0x1968")
    if header.c9c_watch_address != header.final_owner + 0x0C9C:
        raise TraceError("DR2 does not equal final owner + 0x0C9C")
    if header.f64_watch_address != header.physics_base + 0x0F64:
        raise TraceError("DR3 does not equal physics base + 0x0F64")
    expected_markers = {
        "rbx_caller_return": header.library_base + MARKER_RVAS["rbx_caller"],
        "yaw_candidate_return":
            header.library_base + MARKER_RVAS["yaw_candidate"],
        "rbx_internal_angular_return":
            header.library_base + MARKER_RVAS["rbx_internal_angular"],
    }
    for field, expected in expected_markers.items():
        if getattr(header, field) != expected:
            raise TraceError(f"marker address mismatch for {field}")


def _validate_events(header: Header, events: Sequence[Event]) -> None:
    observed_hits = [0, 0, 0, 0]
    previous_ns = 0
    for index, event in enumerate(events):
        if event.sequence != index:
            raise TraceError(
                f"event sequence mismatch at index {index}: {event.sequence}"
            )
        if event.monotonic_ns < header.start_ns or event.monotonic_ns < previous_ns:
            raise TraceError(f"non-monotonic event timestamp at sequence {index}")
        previous_ns = event.monotonic_ns
        if event.tid <= 0:
            raise TraceError(f"invalid tid at sequence {index}")
        if event.flags == 0 or event.flags & ~KNOWN_HITS:
            raise TraceError(f"invalid event flags at sequence {index}")
        if event.flags & (event.flags - 1):
            raise TraceError(f"ambiguous multi-slot hit at sequence {index}")
        if event.dr6 & KNOWN_HITS != event.flags:
            raise TraceError(f"DR6/flags mismatch at sequence {index}")
        if (event.value_read_ok, event.stack_read_ok, event.register_read_ok) != (
            1, 1, 1
        ):
            raise TraceError(f"incomplete read at sequence {index}")
        if event.reserved != 0:
            raise TraceError(f"nonzero event reserved field at sequence {index}")
        for slot in range(4):
            if event.flags == 1 << slot:
                observed_hits[slot] += 1
    if tuple(observed_hits) != header.slot_hits:
        raise TraceError(
            f"slot count mismatch: events={tuple(observed_hits)} "
            f"header={header.slot_hits}"
        )


def _fingerprint(words: Sequence[int]) -> str:
    packed = struct.pack(f"<{len(words)}Q", *words)
    return hashlib.sha256(packed).hexdigest()


def find_marker_hits(header: Header, events: Iterable[Event]) -> tuple[MarkerHit, ...]:
    marker_addresses = {
        "rbx_caller": header.rbx_caller_return,
        "yaw_candidate": header.yaw_candidate_return,
        "rbx_internal_angular": header.rbx_internal_angular_return,
    }
    hits: list[MarkerHit] = []
    for event in events:
        for marker, address in marker_addresses.items():
            indices = [
                index for index, value in enumerate(event.stack_words)
                if value == address
            ]
            if len(indices) > 1:
                raise TraceError(
                    f"duplicate {marker} marker in event {event.sequence}: {indices}"
                )
            if not indices:
                continue
            index = indices[0]
            start = max(0, index - 2)
            end = min(len(event.stack_words), index + 3)
            neighbors = event.stack_words[start:end]
            hits.append(MarkerHit(
                event_sequence=event.sequence,
                tid=event.tid,
                event_flags=event.flags,
                marker=marker,
                address=address,
                stack_index=index,
                neighbor_start_index=start,
                neighbor_words=neighbors,
                neighbor_fingerprint_sha256=_fingerprint(neighbors),
            ))
    return tuple(hits)


def classify_windows(
    events: Sequence[Event], marker_hits: Sequence[MarkerHit]
) -> tuple[PhaseWindow, ...]:
    hits_by_sequence: dict[int, list[MarkerHit]] = {}
    for hit in marker_hits:
        hits_by_sequence.setdefault(hit.event_sequence, []).append(hit)

    # Candidate markers are slot-specific.  This prevents an unrelated marker
    # present deeper in a C9C/F64 stack from being promoted to a successor.
    def candidate_hits(event: Event) -> list[MarkerHit]:
        allowed: set[str]
        if event.flags == HIT_RBX:
            allowed = {"rbx_caller"}
        elif event.flags == HIT_ANGULAR_AUX:
            allowed = {"yaw_candidate", "rbx_internal_angular"}
        else:
            return []
        return [
            hit for hit in hits_by_sequence.get(event.sequence, [])
            if hit.marker in allowed
        ]

    open_by_tid: dict[int, tuple[int, list[MarkerHit]]] = {}
    windows: list[PhaseWindow] = []
    for event in events:
        if event.flags == HIT_C9C:
            if event.tid in open_by_tid:
                previous, _ = open_by_tid[event.tid]
                raise TraceError(
                    f"tid {event.tid} has C9C {event.sequence} before F64 "
                    f"closed C9C {previous}"
                )
            open_by_tid[event.tid] = (event.sequence, [])
            continue
        current = open_by_tid.get(event.tid)
        if current is None:
            continue
        if event.flags in (HIT_RBX, HIT_ANGULAR_AUX):
            current[1].extend(candidate_hits(event))
            continue
        if event.flags == HIT_F64:
            c9c_sequence, candidates = current
            if not c9c_sequence < event.sequence:
                raise TraceError("non-increasing C9C/F64 phase sequence")
            windows.append(PhaseWindow(
                tid=event.tid,
                c9c_sequence=c9c_sequence,
                f64_sequence=event.sequence,
                candidate_sequences=tuple(
                    sorted({hit.event_sequence for hit in candidates})
                ),
                candidate_markers=tuple(hit.marker for hit in candidates),
                marker_hits=tuple(candidates),
            ))
            del open_by_tid[event.tid]
    return tuple(windows)


def analyze(path: Path | str) -> Analysis:
    header, events = read_trace(path)
    marker_hits = find_marker_hits(header, events)
    windows = classify_windows(events, marker_hits)
    return Analysis(header, events, marker_hits, windows)


def _hex(value: int) -> str:
    return f"0x{value:016x}"


def event_dict(event: Event) -> dict[str, object]:
    return {
        "sequence": event.sequence,
        "monotonic_ns": event.monotonic_ns,
        "tid": event.tid,
        "flags": f"0x{event.flags:x}",
        "dr6": _hex(event.dr6),
        "rip": _hex(event.rip),
        "rsp": _hex(event.rsp),
        "rbp": _hex(event.rbp),
        "rflags": _hex(event.rflags),
        "orig_rax": _hex(event.orig_rax),
        "gpr": {name: _hex(value) for name, value in event.gpr.items()},
        "rbx_words": [f"0x{value:08x}" for value in event.rbx_words],
        "angular_words": [f"0x{value:08x}" for value in event.angular_words],
        "c9c_bits": f"0x{event.c9c_bits:08x}",
        "f64_bits": f"0x{event.f64_bits:08x}",
        "stack_words": [_hex(value) for value in event.stack_words],
    }


def _json_default(value: object) -> object:
    if isinstance(value, bytes):
        return value.decode("ascii", "replace").rstrip("\x00")
    if isinstance(value, tuple):
        return list(value)
    raise TypeError(type(value).__name__)


def analysis_dict(analysis: Analysis) -> dict[str, object]:
    marker_hits = []
    for hit in analysis.marker_hits:
        item = asdict(hit)
        item["address"] = _hex(hit.address)
        item["neighbor_words"] = [_hex(value) for value in hit.neighbor_words]
        marker_hits.append(item)
    windows = []
    for window in analysis.windows:
        item = asdict(window)
        item["has_rbx"] = window.has_rbx
        item["has_angular"] = window.has_angular
        item["has_all_markers"] = window.has_all_markers
        windows.append(item)
    return {
        "header": asdict(analysis.header),
        "events": [event_dict(event) for event in analysis.events],
        "marker_hits": marker_hits,
        "windows": windows,
    }


def require_gate(
    analysis: Analysis, *, require_window: bool,
    require_dual_candidates: bool, require_all_markers: bool,
    require_stable_classifier: bool = False,
) -> None:
    if require_stable_classifier:
        require_all_markers = True
    positive_windows = [
        window for window in analysis.windows if window.has_positive_candidate
    ]
    marker_windows = [
        window for window in analysis.windows if window.marker_hits
    ]
    if require_window and not positive_windows:
        raise TraceError(
            "no same-tid C9C -> positive successor candidate -> F64 window"
        )
    if require_dual_candidates and not any(
        window.has_rbx and window.has_angular for window in positive_windows
    ):
        raise TraceError(
            "no same-tid C9C -> (RBX and angular candidates) -> F64 window"
        )
    validated_hits = [
        hit for window in marker_windows for hit in window.marker_hits
    ]
    validated_markers = {hit.marker for hit in validated_hits}
    if require_all_markers and validated_markers < set(MARKER_RVAS):
        raise TraceError(
            "not all three exact markers were observed inside same-tid "
            "C9C -> candidate -> F64 windows"
        )
    if require_all_markers:
        for marker in MARKER_RVAS:
            marker_hits = [
                hit for hit in validated_hits if hit.marker == marker
            ]
            indices = {
                hit.stack_index for hit in marker_hits
            }
            if len(indices) != 1:
                raise TraceError(
                    f"{marker} does not have one stable stack index: "
                    f"{sorted(indices)}"
                )
            if require_stable_classifier:
                if len(marker_hits) < 2:
                    raise TraceError(
                        f"{marker} needs at least two qualified observations: "
                        f"observed={len(marker_hits)}"
                    )
                tids = {hit.tid for hit in marker_hits}
                neighbor_starts = {
                    hit.neighbor_start_index for hit in marker_hits
                }
                fingerprints = {
                    hit.neighbor_fingerprint_sha256 for hit in marker_hits
                }
                if len(tids) != 1:
                    raise TraceError(
                        f"{marker} does not have one stable owner tid: "
                        f"{sorted(tids)}"
                    )
                if len(neighbor_starts) != 1 or len(fingerprints) != 1:
                    raise TraceError(
                        f"{marker} does not have one stable neighbor "
                        "fingerprint"
                    )


def print_text(analysis: Analysis) -> None:
    header = analysis.header
    print(
        "SUCCESSOR_CLASSIFIER_V1_OK "
        f"events={len(analysis.events)} slot_hits={header.slot_hits} "
        f"markers={len(analysis.marker_hits)} windows={len(analysis.windows)}"
    )
    for event in analysis.events:
        print(json.dumps(event_dict(event), separators=(",", ":")))
    for hit in analysis.marker_hits:
        neighbors = ",".join(_hex(value) for value in hit.neighbor_words)
        print(
            f"MARKER seq={hit.event_sequence} tid={hit.tid} "
            f"slot=0x{hit.event_flags:x} name={hit.marker} "
            f"index={hit.stack_index} neighbor_start={hit.neighbor_start_index} "
            f"neighbor=[{neighbors}] sha256={hit.neighbor_fingerprint_sha256}"
        )
    for window in analysis.windows:
        print(
            f"WINDOW tid={window.tid} c9c={window.c9c_sequence} "
            f"candidates={window.candidate_sequences} f64={window.f64_sequence} "
            f"markers={window.candidate_markers} dual="
            f"{int(window.has_rbx and window.has_angular)}"
        )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", type=Path)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--require-window", action="store_true")
    parser.add_argument("--require-dual-candidates", action="store_true")
    parser.add_argument("--require-all-markers", action="store_true")
    parser.add_argument("--require-stable-classifier", action="store_true")
    args = parser.parse_args(argv)
    try:
        analysis = analyze(args.trace)
        require_gate(
            analysis,
            require_window=(args.require_window or
                             args.require_dual_candidates or
                             args.require_all_markers or
                             args.require_stable_classifier),
            require_dual_candidates=args.require_dual_candidates,
            require_all_markers=args.require_all_markers,
            require_stable_classifier=args.require_stable_classifier,
        )
    except (OSError, TraceError) as error:
        print(f"SUCCESSOR_CLASSIFIER_V1_REJECT: {error}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(analysis_dict(analysis), indent=2,
                         default=_json_default))
    else:
        print_text(analysis)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
