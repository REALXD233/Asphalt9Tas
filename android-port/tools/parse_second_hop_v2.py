#!/usr/bin/env python3
"""Parse passive second-hop keyboard-dispatch captures (A9SHP2)."""

from __future__ import annotations

import argparse
import collections
import json
import struct
from pathlib import Path


HEADER = struct.Struct("<8s20sIQQII")
RECORD_PREFIX = struct.Struct("<QQQQIIIIQQ")
SUBSCRIBER_PREFIX = struct.Struct("<QQQB7xQQQ")
SNAPSHOT_SIZE = 0x80
MAX_SUBSCRIBERS = 8
EXPECTED_RECORD_SIZE = 0xA80
MODULE_SIZE = 0xA5D8168


def qword(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def module_refs(data: bytes, base: int) -> list[dict[str, str | int]]:
    refs: list[dict[str, str | int]] = []
    for offset in range(0, len(data) - 7, 8):
        value = qword(data, offset)
        if base <= value < base + MODULE_SIZE:
            refs.append(
                {
                    "field_offset": f"0x{offset:x}",
                    "absolute": f"0x{value:x}",
                    "rva": f"0x{value - base:x}",
                }
            )
    return refs


def parse(path: Path) -> dict[str, object]:
    blob = path.read_bytes()
    if len(blob) < HEADER.size:
        raise ValueError("file is shorter than the A9SHP2 header")
    magic, build_id, record_size, base, events, count, _ = HEADER.unpack_from(blob)
    if not magic.startswith(b"A9SHP2"):
        raise ValueError(f"unexpected magic {magic!r}")
    if record_size != EXPECTED_RECORD_SIZE:
        raise ValueError(
            f"record size 0x{record_size:x}, expected 0x{EXPECTED_RECORD_SIZE:x}"
        )
    expected_size = HEADER.size + count * record_size
    if len(blob) != expected_size:
        raise ValueError(f"file size {len(blob)}, expected {expected_size}")

    records: list[dict[str, object]] = []
    callback_counts: collections.Counter[str] = collections.Counter()
    bound_target_counts: collections.Counter[str] = collections.Counter()
    for index in range(count):
        start = HEADER.size + index * record_size
        fields = RECORD_PREFIX.unpack_from(blob, start)
        (
            sequence,
            x0,
            x1,
            caller,
            tid,
            sub_count,
            eligible_count,
            _,
            head,
            count_candidate,
        ) = fields
        cursor = start + RECORD_PREFIX.size + SNAPSHOT_SIZE
        subscribers: list[dict[str, object]] = []
        for sub_index in range(MAX_SUBSCRIBERS):
            prefix = SUBSCRIBER_PREFIX.unpack_from(blob, cursor)
            node, obj, owner, removed, refcnt, vtable_rva, callback_rva = prefix
            object_start = cursor + SUBSCRIBER_PREFIX.size
            object_snapshot = blob[object_start : object_start + SNAPSHOT_SIZE]
            vtable_start = object_start + SNAPSHOT_SIZE
            vtable_snapshot = blob[vtable_start : vtable_start + SNAPSHOT_SIZE]
            cursor = vtable_start + SNAPSHOT_SIZE
            if sub_index >= min(sub_count, MAX_SUBSCRIBERS):
                continue

            callback_key = f"0x{callback_rva:x}"
            callback_counts[callback_key] += 1
            object_refs = module_refs(object_snapshot, base)
            vtable_refs = module_refs(vtable_snapshot, base)

            # The known callback-wrapper layout stores its actual target at
            # object+0x58. Report it independently; IDA can then distinguish
            # a direct handler from another this-adjust thunk.
            bound_target = qword(object_snapshot, 0x58)
            bound_target_rva = None
            if base <= bound_target < base + MODULE_SIZE:
                bound_target_rva = f"0x{bound_target - base:x}"
                bound_target_counts[bound_target_rva] += 1

            subscribers.append(
                {
                    "index": sub_index,
                    "node": f"0x{node:x}",
                    "object": f"0x{obj:x}",
                    "owner": f"0x{owner:x}",
                    "removed": removed,
                    "refcnt": refcnt,
                    "vtable_rva": f"0x{vtable_rva:x}",
                    "callback_rva": callback_key,
                    "bound_target_rva_at_object_0x58": bound_target_rva,
                    "object_module_refs": object_refs,
                    "vtable_module_refs": vtable_refs,
                }
            )

        records.append(
            {
                "index": index,
                "sequence": sequence,
                "x0": f"0x{x0:x}",
                "x1": f"0x{x1:x}",
                "caller_rva": f"0x{caller - base:x}" if base <= caller else None,
                "tid": tid,
                "sub_count": sub_count,
                "eligible_count": eligible_count,
                "head": f"0x{head:x}",
                "count_candidate": count_candidate,
                "subscribers": subscribers,
            }
        )

    return {
        "file": str(path),
        "build_id": build_id.hex(),
        "guest_base": f"0x{base:x}",
        "events": events,
        "record_count": count,
        "record_size": f"0x{record_size:x}",
        "callback_counts": dict(callback_counts.most_common()),
        "bound_target_counts": dict(bound_target_counts.most_common()),
        "records": records,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--summary", action="store_true", help="omit records")
    args = parser.parse_args()
    result = parse(args.capture)
    if args.summary:
        result.pop("records", None)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
