#!/usr/bin/env python3
"""Parse A9PGTR2 transaction receipts and optionally extract replay bits."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import sys


REPORT_SIZE = 448
CONTROL_OFFSET = 192
EVIDENCE_OFFSET = 256
HASH_OFFSET = 384
CONTROL = struct.Struct("<8s8I3Q")
EVIDENCE = struct.Struct("<8sIIiI12Q")
EVENT = struct.Struct("<4Q6IQ")


def parse(path: Path) -> tuple[dict, bytes]:
    data = path.read_bytes()
    if len(data) < REPORT_SIZE or data[:8] != b"A9PGTR2\0":
        raise ValueError("not an A9PGTR2 receipt")
    version, report_size, action, flags = struct.unpack_from("<4I", data, 8)
    if version != 2 or report_size != REPORT_SIZE:
        raise ValueError("unsupported A9PGTR2 report layout")
    qwords = struct.unpack_from("<13Q", data, 24)
    (pid, start_ticks, game_base, step_options, original_vptr, shadow_vptr,
     original_getter, wrapper, payload_base, control_address,
     evidence_address, intervals_address, events_address) = qwords
    mode, limit = struct.unpack_from("<2I", data, 128)
    payload_writes, game_writes, rollbacks, read_errors = struct.unpack_from(
        "<4Q", data, 136)

    control_raw = CONTROL.unpack_from(data, CONTROL_OFFSET)
    evidence_raw = EVIDENCE.unpack_from(data, EVIDENCE_OFFSET)
    control = {
        "magic": control_raw[0].rstrip(b"\0").decode("ascii", "replace"),
        "version": control_raw[1], "size": control_raw[2],
        "mode": control_raw[3], "enabled": control_raw[4],
        "limit": control_raw[5], "cursor": control_raw[6],
        "completed": control_raw[7], "active_calls": control_raw[8],
        "expected_object": hex(control_raw[9]),
        "expected_vptr": hex(control_raw[10]),
        "original_getter": hex(control_raw[11]),
    }
    evidence_names = (
        "calls", "record_calls", "replay_calls", "overrides",
        "semantic_errors", "object_mismatches", "first_tid", "last_tid",
        "tid_changes", "reserved0", "reserved1", "reserved2",
    )
    evidence = {
        "magic": evidence_raw[0].rstrip(b"\0").decode("ascii", "replace"),
        "version": evidence_raw[1], "size": evidence_raw[2],
        "status": evidence_raw[3], "reserved32": evidence_raw[4],
    }
    evidence.update(dict(zip(evidence_names, evidence_raw[5:])))

    count = min(control["cursor"], limit)
    expected_size = REPORT_SIZE
    include_data = action == 3
    if include_data:
        expected_size += count * 4 + count * EVENT.size
    if len(data) != expected_size:
        raise ValueError(
            f"receipt size mismatch: expected {expected_size}, got {len(data)}")

    interval_bytes = b""
    intervals: list[dict] = []
    events: list[dict] = []
    if include_data:
        interval_start = REPORT_SIZE
        interval_bytes = data[interval_start:interval_start + count * 4]
        bits_values = struct.unpack(f"<{count}I", interval_bytes) if count else ()
        for index, bits in enumerate(bits_values):
            value = struct.unpack("<f", struct.pack("<I", bits))[0]
            intervals.append({
                "index": index, "bits": f"0x{bits:08x}", "seconds": value,
                "valid": math.isfinite(value) and 0.001 <= value <= 0.1,
            })
        event_start = interval_start + count * 4
        for index in range(count):
            raw = EVENT.unpack_from(data, event_start + index * EVENT.size)
            events.append({
                "sequence": raw[0], "object": hex(raw[1]),
                "output": hex(raw[2]), "monotonic_ns": raw[3],
                "tid": raw[4], "original_bits": f"0x{raw[5]:08x}",
                "requested_bits": f"0x{raw[6]:08x}",
                "final_bits": f"0x{raw[7]:08x}", "flags": hex(raw[8]),
                "commit_sequence": raw[9], "reserved": raw[10],
            })

    complete = (
        control["completed"] == 1 and control["enabled"] == 0 and
        control["active_calls"] == 0 and control["cursor"] == limit and
        evidence["status"] == 2 and evidence["calls"] == limit and
        evidence["semantic_errors"] == 0 and
        evidence["object_mismatches"] == 0
    )
    result = {
        "format": "A9PGTR2", "version": version, "action": action,
        "flags": hex(flags), "pid": pid, "start_ticks": start_ticks,
        "game_base": hex(game_base), "step_options": hex(step_options),
        "original_vptr": hex(original_vptr), "shadow_vptr": hex(shadow_vptr),
        "original_getter": hex(original_getter), "wrapper": hex(wrapper),
        "payload_base": hex(payload_base),
        "control_address": hex(control_address),
        "evidence_address": hex(evidence_address),
        "intervals_address": hex(intervals_address),
        "events_address": hex(events_address), "mode": mode, "limit": limit,
        "payload_write_attempts": payload_writes,
        "game_write_attempts": game_writes,
        "rollback_attempts": rollbacks, "read_errors": read_errors,
        "payload_sha256": data[HASH_OFFSET:HASH_OFFSET + 32].hex(),
        "complete": complete, "control": control, "evidence": evidence,
        "intervals": intervals, "events": events,
    }
    return result, interval_bytes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("receipt", type=Path)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--extract-intervals", type=Path)
    args = parser.parse_args()
    try:
        result, interval_bytes = parse(args.receipt)
    except (OSError, ValueError) as exc:
        print(f"A9PGTR2 parse failed: {exc}", file=sys.stderr)
        return 2
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    if args.json:
        args.json.write_text(rendered + "\n", encoding="utf-8")
    else:
        print(rendered)
    if args.extract_intervals:
        if result["action"] != 3:
            print("interval extraction requires a finalize receipt", file=sys.stderr)
            return 2
        args.extract_intervals.write_bytes(interval_bytes)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
