#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Parse a9tas_keyboard_bridge_scan VALUE_WATCH raw text evidence.

Read-only offline parser. It segments per-channel press events from the
source_A/source_B/sink_A/sink_B diff log and computes event timing,
peak values, hold duration and source->sink lag.
"""
import re
import sys
from dataclasses import dataclass, field

WATCH_RE = re.compile(
    r"VALUE_WATCH t_ms=(?P<t>-?\d+) "
    r"source_A=(?P<sa>[^ ]+) source_B=(?P<sb>[^ ]+) "
    r"sink_A=(?P<ka>[^ ]+) sink_B=(?P<kb>[^ ]+)"
)

FLOAT_RE = re.compile(r"[-+]?\d+(?:\.\d+)?(?:e[-+]?\d+)?", re.I)


@dataclass
class Event:
    channel: str
    start_ms: int
    end_ms: int
    peak: float
    peak_ms: int
    min_val: float = 0.0
    max_val: float = 0.0
    samples: int = 0
    last_active_ms: int = 0
    sink_start_ms: int | None = None
    sink_peak: float = 0.0
    sink_peak_ms: int | None = None
    sink_end_ms: int | None = None
    sink_lag_ms: int | None = None


@dataclass
class WatchLog:
    rows: list[dict] = field(default_factory=list)
    meta: list[str] = field(default_factory=list)


def parse(path: str) -> WatchLog:
    log = WatchLog()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            s = line.strip()
            if s.startswith("VALUE_WATCH t_ms="):
                m = WATCH_RE.match(s)
                if m:
                    log.rows.append({
                        "t": int(m.group("t")),
                        "sa": float(m.group("sa")),
                        "sb": float(m.group("sb")),
                        "ka": float(m.group("ka")),
                        "kb": float(m.group("kb")),
                    })
            elif s.startswith(("VALUE_WATCH_BEGIN", "VALUE_WATCH_END",
                                "INPUT_SOURCE_FIELDS", "CONTROL_SINK_FIELDS",
                                "VALUE_C_BUFFER_FLOATS", "SUMMARY")):
                log.meta.append(s)
    return log


def is_active(value: float, threshold: float = 0.03) -> bool:
    return abs(value) >= threshold


CHANNEL_KEYS = {
    "source_A": "sa",
    "source_B": "sb",
    "sink_A": "ka",
    "sink_B": "kb",
}


def events_for(channel: str, rows: list[dict], threshold: float = 0.03,
               close_gap_ms: int = 200) -> list[Event]:
    events: list[Event] = []
    cur: Event | None = None
    key = CHANNEL_KEYS[channel]
    for r in rows:
        t = int(r["t"])
        v = float(r[key])
        active = abs(v) >= threshold
        if active:
            if cur is None:
                cur = Event(channel=channel, start_ms=t, end_ms=t,
                            peak=v, peak_ms=t, min_val=v, max_val=v,
                            samples=1, last_active_ms=t)
            else:
                cur.end_ms = t
                cur.last_active_ms = t
                cur.samples += 1
                cur.min_val = min(cur.min_val, v)
                cur.max_val = max(cur.max_val, v)
                if abs(v) > abs(cur.peak):
                    cur.peak = v
                    cur.peak_ms = t
        elif cur is not None:
            if t - cur.last_active_ms >= close_gap_ms:
                events.append(cur)
                cur = None
    if cur is not None:
        events.append(cur)
    return events


def annotate_sink(ev: Event, sink_channel: str, rows: list[dict],
                  threshold: float = 0.02) -> None:
    for r in rows:
        if r["t"] < ev.start_ms - 5:
            continue
        if r["t"] > ev.end_ms + 400:
            break
        v = float(r[CHANNEL_KEYS[sink_channel]])
        if abs(v) >= threshold:
            if ev.sink_start_ms is None:
                ev.sink_start_ms = r["t"]
                ev.sink_lag_ms = r["t"] - ev.start_ms
            ev.sink_end_ms = r["t"]
            if abs(v) > abs(ev.sink_peak):
                ev.sink_peak = v
                ev.sink_peak_ms = r["t"]


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: parse_value_watch_20260815.py <watch.txt>")
        return 2
    path = sys.argv[1]
    log = parse(path)
    print("== metadata ==")
    for m in log.meta:
        print(m)
    print(f"== parsed rows: {len(log.rows)} ==")
    if not log.rows:
        print("NO VALUE_WATCH ROWS")
        return 1

    all_events: list[Event] = []
    for ch in ("source_A", "source_B", "sink_A", "sink_B"):
        evs = events_for(ch, log.rows)
        if ch.startswith("source"):
            for ev in evs:
                annotate_sink(ev, ch.replace("source", "sink"), log.rows)
        for ev in evs:
            print(
                f"[{ev.channel}] start={ev.start_ms}ms end={ev.end_ms}ms "
                f"dur={ev.end_ms - ev.start_ms}ms samples={ev.samples} "
                f"peak={ev.peak:.9g}@{ev.peak_ms}ms min={ev.min_val:.9g} max={ev.max_val:.9g}"
            )
            if ev.channel.startswith("source") and ev.sink_start_ms is not None:
                print(f"    -> {ev.channel.replace('source','sink')} "
                      f"start={ev.sink_start_ms}ms lag={ev.sink_lag_ms}ms "
                      f"end={ev.sink_end_ms}ms peak={ev.sink_peak:.9g}@{ev.sink_peak_ms}ms")
        all_events.extend(evs)

    print("== source event summary (merged) ==")
    source_events = [e for e in all_events if e.channel.startswith("source")]
    for i, e in enumerate(source_events, 1):
        direction = "zero"
        if e.peak > 0.05:
            direction = "positive"
        elif e.peak < -0.05:
            direction = "negative"
        sink = e.channel.replace("source", "sink")
        sink_followed = "yes" if e.sink_start_ms is not None else "no"
        lag = f"{e.sink_lag_ms}ms" if e.sink_lag_ms is not None else "n/a"
        print(f"#{i} {e.channel} {direction} start={e.start_ms} end={e.end_ms} "
              f"peak={e.peak:.9g} -> {sink} followed={sink_followed} lag={lag} "
              f"sink_peak={e.sink_peak:.9g}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
