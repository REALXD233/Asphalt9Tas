#!/usr/bin/env python3
"""Parse a9tas-tick-observer-v1.bin produced by payload_tick_observer_v1.cpp."""
import argparse
import struct
import sys

KINDS = {
    1: "tick",
    2: "getterA",
    3: "getterB",
    4: "sinkB",
    5: "sinkA",
    6: "finalC98",
    7: "finalC9C",
}


def parse(path):
    with open(path, "rb") as f:
        data = f.read()
    # C++ DumpHeader layout: 8 + 20 + 4 + 4 + (4 pad) + 8 + 8 + 8 + 4*8
    header_size = 8 + 20 + 4 + 4 + 4 + 8 + 8 + 8 + 4 * 8
    header_fmt = "<8s20sII4xQQQ4Q"
    hdr = struct.unpack_from(header_fmt, data, 0)
    magic, build_id, record_size, record_count, guest_base, events, dropped, *_ = hdr
    print(f"magic={magic.decode('ascii', 'replace')}")
    print(f"build_id={build_id.hex()}")
    print(f"record_size={record_size} count={record_count} events={events} dropped={dropped}")
    print(f"guest_base=0x{guest_base:x}")

    rec_fmt = "<QQQQQIHHfII4x"
    expected = 8 * 5 + 4 + 2 + 2 + 4 + 4 + 4 + 4
    if record_size < expected:
        print(f"ERROR: record_size={record_size} < {expected}", file=sys.stderr)
        return 1
    records = []
    off = header_size
    for i in range(record_count):
        if off + record_size > len(data):
            print(f"ERROR: truncated at record {i}", file=sys.stderr)
            return 1
        vals = struct.unpack_from(rec_fmt, data, off)
        records.append(vals)
        off += record_size

    from collections import Counter, defaultdict
    kind_count = Counter()
    tids = Counter()
    first_ns = last_ns = None
    tick_ns = []
    order = Counter()
    prev_tick_seq = None
    for seq, ns, x0, x1, caller, tid, kind, flags, value, x0_word, _res in records:
        kind_count[kind] += 1
        tids[tid] += 1
        if first_ns is None:
            first_ns = ns
        last_ns = ns
        if kind == 1:
            tick_ns.append(ns)
            prev_tick_seq = seq
        elif prev_tick_seq is not None:
            order[(prev_tick_seq, kind)] += 1

    print("\nkind counts:")
    for k in sorted(kind_count):
        print(f"  {str(KINDS.get(k, k)):10s} ({k}) = {kind_count[k]}")
    print("\ntids:")
    for t, c in tids.most_common(12):
        print(f"  tid={t} events={c}")
    if first_ns is not None:
        print(f"\nspan_ms={(last_ns - first_ns) / 1e6:.3f}")
    if len(tick_ns) >= 2:
        deltas = [(tick_ns[i + 1] - tick_ns[i]) / 1e6 for i in range(len(tick_ns) - 1)]
        deltas.sort()
        avg = sum(deltas) / len(deltas)
        print(f"tick_count={len(tick_ns)} delta_ms min={deltas[0]:.3f} "
              f"median={deltas[len(deltas) // 2]:.3f} max={deltas[-1]:.3f} avg={avg:.3f}")
        if deltas:
            print(f"tick_hz_avg={1000.0 / avg:.2f}")

    print("\ntick-local event order (sequence key -> next-kind counts):")
    for (s, k), c in sorted(order.items())[:40]:
        print(f"  seq={s} -> {KINDS.get(k, k)} x{c}")

    print("\nsetter value ranges:")
    for kind in (4, 5, 6, 7):
        vals = [r[8] for r in records if r[6] == kind and (r[7] & 1)]
        if vals:
            print(f"  {KINDS[kind]:10s} n={len(vals)} min={min(vals):+.6f} "
                  f"max={max(vals):+.6f}")

    print("\nfirst 30 records:")
    for r in records[:30]:
        seq, ns, x0, x1, caller, tid, kind, flags, value, x0_word, _res = r
        print(f"  seq={seq} ns={ns} kind={KINDS.get(kind, kind)} tid={tid} "
              f"x0=0x{x0:x} x1=0x{x1:x} caller=0x{caller:x} "
              f"flags=0x{flags:x} value={value:+.4f} x0_word=0x{x0_word:x}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    args = ap.parse_args()
    return parse(args.path)


if __name__ == "__main__":
    sys.exit(main())
