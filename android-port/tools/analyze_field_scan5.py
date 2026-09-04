#!/usr/bin/env python3
"""analyze_field_scan5.py - locate transform/velocity via motion deltas.

For every aligned float column, compute menu-segment and race-segment
adjacent-sample delta stats (median abs delta, active ratio). A vehicle
motion field shows: menu nearly static OR small deltas; race continuous
moderate deltas with high active ratio. Velocity = 3 consecutive active
columns, transform = 16 consecutive active columns (translation rows
usually 12..14).

usage: python analyze_field_scan5.py <field_scan.bin>
"""
import struct
import sys
import math


def main():
    if len(sys.argv) != 2:
        print("usage: python analyze_field_scan5.py <field_scan.bin>")
        return 2
    data = open(sys.argv[1], "rb").read()
    if data[0:8] != b"A9FSCN1\x00":
        print("bad magic", data[0:8])
        return 2
    base, owner, startA, _ = struct.unpack_from("<QQQQ", data, 8)
    count = struct.unpack_from("<Q", data, 40)[0]
    window = struct.unpack_from("<I", data, 52)[0]
    ctrl = struct.unpack_from("<Q", data, 56)[0]
    stride = 8 + window
    rec = stride * 2 if ctrl else stride
    n = min(count, (len(data) - 64) // rec)
    menu_end = max(1, int(n * 0.15))
    race_start = max(menu_end, int(n * 0.25))
    print(f"samples={n} menu_end={menu_end} race_start={race_start} "
          f"owner=0x{owner:x} start=0x{startA:x}")

    cols = window // 4
    menu_med = [0.0] * cols
    race_med = [0.0] * cols
    race_active = [0.0] * cols
    for j in range(cols):
        menu_d = []
        race_d = []
        prev = None
        for i in range(n):
            off = 64 + i * rec + 8 + j * 4
            v = struct.unpack_from("<f", data, off)[0]
            if math.isnan(v) or math.isinf(v):
                prev = None
                continue
            if prev is not None:
                d = abs(v - prev)
                if i < menu_end:
                    menu_d.append(d)
                elif i > race_start:
                    race_d.append(d)
            prev = v
        if menu_d:
            menu_d.sort()
            menu_med[j] = menu_d[len(menu_d) // 2]
        if race_d:
            race_d.sort()
            race_med[j] = race_d[len(race_d) // 2]
            thr = 0.02
            race_active[j] = sum(1 for d in race_d if d > thr) / len(race_d)

    def is_active(j):
        m = race_med[j]
        return (0.0005 < m < 20.0 and race_active[j] > 0.3)

    # Velocity: 3 consecutive active columns with similar magnitudes.
    vel = []
    for off in range(0, cols - 2):
        if is_active(off) and is_active(off + 1) and is_active(off + 2):
            meds = [race_med[off], race_med[off + 1], race_med[off + 2]]
            mx = max(meds)
            if mx > 0 and min(meds) / mx > 0.05:
                vel.append(off)
    print("--- VELOCITY candidates (3 consecutive active columns) ---")
    for off in vel[:20]:
        print(f"  off=0x{off*4:x} abs=0x{startA+off*4:x} "
              f"med=({race_med[off]:.4f},{race_med[off+1]:.4f},{race_med[off+2]:.4f}) "
              f"act=({race_active[off]:.2f},{race_active[off+1]:.2f},{race_active[off+2]:.2f})")
    if not vel:
        print("  none")

    # Transform: 16+ consecutive active columns; report block starts where
    # columns 12..14 (translation) are active.
    active_flags = [is_active(j) for j in range(cols)]
    blocks = []
    run_start = None
    for j in range(cols):
        if active_flags[j]:
            if run_start is None:
                run_start = j
        else:
            if run_start is not None and j - run_start >= 16:
                blocks.append((run_start, j - 1))
            run_start = None
    if run_start is not None and cols - run_start >= 16:
        blocks.append((run_start, cols - 1))
    print("--- TRANSFORM block candidates (>=16 consecutive active columns) ---")
    for (s, e) in blocks[:20]:
        print(f"  off=0x{s*4:x}..0x{e*4:x} abs=0x{startA+s*4:x} len={e-s+1} "
              f"med12={race_med[s+12]:.4f} act12={race_active[s+12]:.2f}")
    if not blocks:
        print("  none")
    return 0


if __name__ == "__main__":
    sys.exit(main())
