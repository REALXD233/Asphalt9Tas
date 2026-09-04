#!/usr/bin/env python3
"""Report callback-cycle owner versus world-accumulator writer TIDs."""

from __future__ import annotations

import argparse
from pathlib import Path

from parse_hwbp_pipeline_order_v1 import (
    HIT_ACCUMULATOR,
    HIT_CALLBACK_FLAGS,
    HIT_COMPLETION,
    HIT_F64,
    read_trace,
)


def analyze(path: Path) -> tuple[int, int, int, list[tuple[int, int, int]]]:
    _, events = read_trace(path)
    state = "waiting"
    owner_tid: int | None = None
    cycles = same_owner = different_owner = 0
    examples: list[tuple[int, int, int]] = []
    for event in events:
        event_types: list[str] = []
        if event.flags & HIT_COMPLETION:
            event_types.append("completion")
        if event.flags & HIT_CALLBACK_FLAGS:
            event_types.append("callback")
        if event.flags & HIT_F64:
            event_types.append("f64")
        if event.flags & HIT_ACCUMULATOR:
            event_types.append("accumulator")
        for event_type in event_types:
            if state == "waiting":
                if event_type == "completion":
                    state = "completed"
                    owner_tid = event.tid
                continue
            if event_type == "completion":
                state = "completed"
                owner_tid = event.tid
            elif event_type == "callback":
                dispatching = event.callback_flags & 0xFF
                if state == "completed" and dispatching == 1:
                    state = "callback_open"
                elif state == "published" and dispatching == 0:
                    state = "callback_closed"
            elif event_type == "f64" and state == "callback_open":
                state = "published"
            elif event_type == "accumulator" and state == "callback_closed":
                assert owner_tid is not None
                cycles += 1
                if event.tid == owner_tid:
                    same_owner += 1
                else:
                    different_owner += 1
                    if len(examples) < 5:
                        examples.append((event.sequence, owner_tid, event.tid))
                state = "waiting"
                owner_tid = None
    return cycles, same_owner, different_owner, examples


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, nargs="+")
    args = parser.parse_args()
    for path in args.trace:
        cycles, same, different, examples = analyze(path)
        print(
            f"trace={path.name} cycles={cycles} accumulator_same_owner={same} "
            f"accumulator_different_owner={different} examples={examples}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
