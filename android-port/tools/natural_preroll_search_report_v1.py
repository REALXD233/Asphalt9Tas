#!/usr/bin/env python3
"""Strict A9NPR1 zero-write natural pre-roll search verifier."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

from natural_preroll_anchor_v1 import (
    NaturalPrerollAnchorV1,
    anchor_matches,
    decode_anchor,
)
from parse_unified_executor_report_v2 import (
    AUDIT_EXACT,
    COMMITTED,
    GATE2_COMPLETE,
    PREFIX_CERTIFIED,
    TRANSFORM_OFFSET,
    TRANSFORM_SIZE,
    LINEAR_OFFSET,
    LINEAR_SIZE,
)
from parse_unified_executor_report_v5 import FRAME_SIZE, _FRAME
from unified_tick_recording_v1 import SUPPORTED_BUILD_ID


MAGIC = b"A9NPR1\0\0"
VERSION = 1
HEADER_SIZE = 200
REQUIRED_FLAGS = 0x3F
ANCHOR_MATCHED = 1 << 8
SEARCH_AUDIT_FLAGS = AUDIT_EXACT | GATE2_COMPLETE | COMMITTED | PREFIX_CERTIFIED
_HEADER = struct.Struct("<8s6I20sI8Q4I32s32s")
assert _HEADER.size == HEADER_SIZE


@dataclass(frozen=True)
class NaturalSearchResultV1:
    cycles: int
    matched_cycle: int
    first_event: int
    last_event: int


def _snapshot_payload(snapshot: bytes) -> bytes:
    if len(snapshot) != 804:
        raise ValueError("A9NPR1 snapshot size mismatch")
    return (
        snapshot[TRANSFORM_OFFSET : TRANSFORM_OFFSET + TRANSFORM_SIZE]
        + snapshot[LINEAR_OFFSET : LINEAR_OFFSET + LINEAR_SIZE]
    )


def verify_search_report(
    report_blob: bytes, anchor: NaturalPrerollAnchorV1
) -> NaturalSearchResultV1:
    if len(report_blob) < HEADER_SIZE:
        raise ValueError("A9NPR1 is shorter than its header")
    header = _HEADER.unpack_from(report_blob)
    if header[0] != MAGIC or header[1] != VERSION:
        raise ValueError("unsupported A9NPR1 magic/version")
    if header[2] != HEADER_SIZE or header[3] != FRAME_SIZE:
        raise ValueError("A9NPR1 ABI size mismatch")
    if header[4] != REQUIRED_FLAGS:
        raise ValueError("A9NPR1 success flags mismatch")
    cycles, matched = header[5], header[6]
    if cycles < 1 or matched != cycles - 1:
        raise ValueError("A9NPR1 must stop on its first committed match")
    if header[7] != SUPPORTED_BUILD_ID or header[8] != 0:
        raise ValueError("A9NPR1 build/reserved field mismatch")
    identities = header[9:15]
    if any(value == 0 for value in identities):
        raise ValueError("A9NPR1 contains a zero runtime identity")
    event_count, thread_additions = header[15], header[16]
    initial_threads, final_threads = header[17], header[18]
    if initial_threads < 1 or final_threads != initial_threads + thread_additions:
        raise ValueError("A9NPR1 clean-detach thread accounting mismatch")
    if header[19] != anchor.fixed_interval_us or header[20] != anchor.frame_count:
        raise ValueError("A9NPR1 source interval/frame count mismatch")
    if header[21] != anchor.recording_sha256 or header[22] != anchor.report_sha256:
        raise ValueError("A9NPR1 source hash binding mismatch")
    expected_size = HEADER_SIZE + cycles * FRAME_SIZE
    if len(report_blob) != expected_size:
        raise ValueError(f"A9NPR1 length must be exactly {expected_size} bytes")

    previous_event = 0
    first_event = 0
    for index in range(cycles):
        frame = _FRAME.unpack_from(report_blob, HEADER_SIZE + index * FRAME_SIZE)
        flags = frame[3]
        expected = SEARCH_AUDIT_FLAGS | (ANCHOR_MATCHED if index == matched else 0)
        if flags != expected:
            raise ValueError(f"search cycle {index}: audit flags mismatch")
        if frame[0] != 0 or frame[1] != 0 or frame[2] <= 0 or frame[16] <= 0:
            raise ValueError(f"search cycle {index}: tick/thread field mismatch")
        if frame[2] == frame[16] or not 1 <= frame[4] <= 1_000_000 or frame[5] != 0:
            raise ValueError(f"search cycle {index}: delta/owner field mismatch")
        events = frame[6:13]
        if not all(left < right for left, right in zip(events, events[1:])):
            raise ValueError(f"search cycle {index}: event order mismatch")
        if previous_event and events[0] <= previous_event:
            raise ValueError(f"search cycle {index}: event overlap")
        if frame[13] == frame[14] or frame[15] & 0xFF != 1:
            raise ValueError(f"search cycle {index}: callback certificate mismatch")
        if frame[17] != bytes(2) or frame[18] != 0 or frame[19] != bytes(4):
            raise ValueError(f"search cycle {index}: reserved/control field mismatch")
        if frame[22] != bytes(64) or frame[23] != bytes(12):
            raise ValueError(f"search cycle {index}: recorded payload must be empty")
        if frame[24] != frame[25]:
            raise ValueError(f"search cycle {index}: search changed audited state")
        matched_now = anchor_matches(_snapshot_payload(frame[24]), anchor)
        if matched_now != (index == matched):
            raise ValueError(f"search cycle {index}: anchor match decision mismatch")
        if index == 0:
            first_event = events[0]
        previous_event = events[-1]
    if event_count < previous_event:
        raise ValueError("A9NPR1 event count precedes its final commit")
    return NaturalSearchResultV1(cycles, matched, first_event, previous_event)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("anchor", type=Path)
    args = parser.parse_args()
    try:
        anchor = decode_anchor(args.anchor.read_bytes())
        result = verify_search_report(args.report.read_bytes(), anchor)
    except (OSError, ValueError, struct.error) as error:
        print(f"a9npr1_error={error}")
        return 1
    print(
        f"a9npr1_supported=1 cycles={result.cycles} "
        f"matched={result.matched_cycle} "
        f"events={result.first_event}..{result.last_event} zero_writes=1"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
