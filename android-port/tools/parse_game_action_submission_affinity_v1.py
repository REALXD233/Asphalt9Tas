#!/usr/bin/env python3
"""Strict parser for the game-owned action submission-affinity transcript."""

from __future__ import annotations

import argparse
import collections
import re
from pathlib import Path


ARMED_RE = re.compile(
    r"^GAME_ACTION_SUBMISSION_AFFINITY_V1_ARMED "
    r"pid=(\d+) owner=0x([0-9a-f]+) queue_end=0x([0-9a-f]+) "
    r"active=0x([0-9a-f]+) mode=0x([0-9a-f]+) "
    r"delta=0x([0-9a-f]+) initial_count=(\d+) threads=(\d+) "
    r"duration_ms=(\d+) writes=debug-registers-only game_calls=0 "
    r"input_writes=0$"
)
EVENT_RE = re.compile(
    r"^GAME_ACTION_SUBMISSION_EVENT seq=(\d+) ns=(\d+) tid=(\d+) "
    r"name=(.+?) flags=([Q-][A-][M-][D-]) count=(\d+) direct=([01]) "
    r"token=0x([0-9a-f]+) completion=(\d+) completion_ok=([01]) "
    r"active=([01]) mode=(\d+) delta=(-?\d+) rip=0x([0-9a-f]+) "
    r"read_ok=([01]) regs_ok=([01])$"
)
DONE_RE = re.compile(
    r"^GAME_ACTION_SUBMISSION_AFFINITY_V1_DONE events=(\d+) "
    r"queue_hits=(\d+) appends=(\d+) cleanups=(\d+) active=(\d+) "
    r"mode=(\d+) delta=(\d+) read_errors=(\d+) ptrace_errors=(\d+) "
    r"unexpected_stops=(\d+) clean=([01]) game_calls=0 input_writes=0$"
)


def decode_transcript(text: str) -> dict[str, object]:
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if len(lines) < 3:
        raise ValueError("submission-affinity transcript is incomplete")
    armed = ARMED_RE.fullmatch(lines[0])
    done = DONE_RE.fullmatch(lines[-1])
    if not armed or not done:
        raise ValueError("submission-affinity transcript boundary mismatch")

    (
        pid,
        owner_hex,
        queue_end_hex,
        active_hex,
        mode_hex,
        delta_hex,
        initial_count,
        initial_threads,
        duration_ms,
    ) = armed.groups()
    pid_i = int(pid)
    owner = int(owner_hex, 16)
    queue_end = int(queue_end_hex, 16)
    identities = (owner, queue_end, int(active_hex, 16), int(mode_hex, 16),
                  int(delta_hex, 16))
    if pid_i <= 0 or not all(identities):
        raise ValueError("submission-affinity identity is zero")
    if queue_end != owner + 0x1368:
        raise ValueError("completion-vector end address mismatch")
    if int(initial_threads) <= 0 or not 1000 <= int(duration_ms) <= 60000:
        raise ValueError("submission-affinity arm parameters invalid")

    event_lines = lines[1:-1]
    counters: collections.Counter[str] = collections.Counter()
    append_tids: collections.Counter[int] = collections.Counter()
    cleanup_tids: collections.Counter[int] = collections.Counter()
    nitro_tids: collections.Counter[int] = collections.Counter()
    delta_tids: collections.Counter[int] = collections.Counter()
    previous_count = int(initial_count)
    previous_ns = -1
    first_append_seq: int | None = None
    first_nitro_seq: int | None = None
    last_append_seq: int | None = None
    last_append_ns: int | None = None
    nitro_submission_delay_ns: int | None = None
    completion_at_append: list[int] = []

    for index, line in enumerate(event_lines):
        match = EVENT_RE.fullmatch(line)
        if not match:
            raise ValueError(f"malformed submission event {index}")
        (
            sequence,
            monotonic_ns,
            tid,
            _name,
            flags,
            count,
            _direct,
            token_hex,
            completion,
            completion_ok,
            _active,
            _mode,
            delta_value,
            rip_hex,
            read_ok,
            regs_ok,
        ) = match.groups()
        sequence_i = int(sequence)
        ns_i = int(monotonic_ns)
        tid_i = int(tid)
        count_i = int(count)
        if sequence_i != index or ns_i < previous_ns or tid_i <= 0:
            raise ValueError("submission event sequence/time/tid mismatch")
        if flags == "----" or int(rip_hex, 16) == 0:
            raise ValueError("submission event flags/rip mismatch")
        if read_ok != "1" or regs_ok != "1":
            raise ValueError("submission event read/register failure")

        if flags[0] == "Q":
            counters["queue"] += 1
            if count_i == previous_count + 1:
                counters["append"] += 1
                append_tids[tid_i] += 1
                if first_append_seq is None:
                    first_append_seq = sequence_i
                last_append_seq = sequence_i
                last_append_ns = ns_i
                if int(token_hex, 16) == 0 or completion_ok != "1":
                    raise ValueError("appended completion token is unreadable")
                completion_i = int(completion)
                if completion_i not in (0, 1):
                    raise ValueError("noncanonical completion byte")
                completion_at_append.append(completion_i)
            elif count_i < previous_count:
                counters["cleanup"] += 1
                cleanup_tids[tid_i] += 1
            previous_count = count_i
        if flags[1] == "A":
            counters["active"] += 1
            nitro_tids[tid_i] += 1
            if first_nitro_seq is None:
                first_nitro_seq = sequence_i
                if (last_append_seq is None or last_append_ns is None or
                        count_i == 0 or
                        sequence_i - last_append_seq > 8 or
                        ns_i - last_append_ns > 100_000_000):
                    raise ValueError(
                        "Nitro transition is not bound to a live submission"
                    )
                nitro_submission_delay_ns = ns_i - last_append_ns
        if flags[2] == "M":
            counters["mode"] += 1
            nitro_tids[tid_i] += 1
            if first_nitro_seq is None:
                first_nitro_seq = sequence_i
                if (last_append_seq is None or last_append_ns is None or
                        count_i == 0 or
                        sequence_i - last_append_seq > 8 or
                        ns_i - last_append_ns > 100_000_000):
                    raise ValueError(
                        "Nitro transition is not bound to a live submission"
                    )
                nitro_submission_delay_ns = ns_i - last_append_ns
        if flags[3] == "D":
            counters["delta"] += 1
            if int(delta_value) != 0:
                delta_tids[tid_i] += 1
        previous_ns = ns_i

    (
        done_events,
        done_queue,
        done_appends,
        done_cleanups,
        done_active,
        done_mode,
        done_delta,
        read_errors,
        ptrace_errors,
        unexpected_stops,
        clean,
    ) = map(int, done.groups())
    expected = (
        len(event_lines),
        counters["queue"],
        counters["append"],
        counters["cleanup"],
        counters["active"],
        counters["mode"],
        counters["delta"],
    )
    if expected != (
        done_events,
        done_queue,
        done_appends,
        done_cleanups,
        done_active,
        done_mode,
        done_delta,
    ):
        raise ValueError("submission-affinity counters mismatch")
    if read_errors or ptrace_errors or unexpected_stops or clean != 1:
        raise ValueError("submission-affinity capture did not detach cleanly")
    if not append_tids or not cleanup_tids:
        raise ValueError("no complete token submission/cleanup lifecycle")
    if counters["active"] == 0 or counters["mode"] == 0 or not nitro_tids:
        raise ValueError("no Nitro active/mode transition captured")
    if not delta_tids:
        raise ValueError("no naturally running nonzero delta captured")
    assert first_append_seq is not None and first_nitro_seq is not None
    assert nitro_submission_delay_ns is not None
    return {
        "events": len(event_lines),
        "append_tids": tuple(sorted(append_tids)),
        "cleanup_tids": tuple(sorted(cleanup_tids)),
        "nitro_tids": tuple(sorted(nitro_tids)),
        "delta_tids": tuple(sorted(delta_tids)),
        "nitro_after_first_append": first_nitro_seq > first_append_seq,
        "nitro_submission_delay_ns": nitro_submission_delay_ns,
        "completion_at_append": tuple(completion_at_append),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("transcript", type=Path)
    args = parser.parse_args()
    try:
        summary = decode_transcript(args.transcript.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as error:
        print(f"a9asa1_error={error}")
        return 1
    print(
        "a9asa1_supported=1 "
        f"events={summary['events']} "
        f"append_tids={','.join(map(str, summary['append_tids']))} "
        f"cleanup_tids={','.join(map(str, summary['cleanup_tids']))} "
        f"nitro_tids={','.join(map(str, summary['nitro_tids']))} "
        f"delta_tids={','.join(map(str, summary['delta_tids']))} "
        f"nitro_after_first_append={int(summary['nitro_after_first_append'])} "
        f"nitro_submission_delay_ns={summary['nitro_submission_delay_ns']} "
        "completion_at_append="
        f"{','.join(map(str, summary['completion_at_append']))}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
