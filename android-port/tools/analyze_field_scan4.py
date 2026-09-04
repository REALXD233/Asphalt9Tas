#!/usr/bin/env python3
"""analyze_field_scan4.py - locate racer transform/velocity in a wide field scan.

Signatures:
  VELOCITY: 3 consecutive floats near 0 in the menu segment, growing to large
            values during the race segment.
  TRANSFORM: 16 consecutive floats whose translation (elements 12..14) moves
             continuously during the race (menu may also move - garage
             preview), with matrix-like structure (rows 0..11 moderate values).

usage: python analyze_field_scan4.py <field_scan.bin>
"""
import struct
import sys

def main():
    if len(sys.argv) != 2:
        print("usage: python analyze_field_scan4.py <field_scan.bin>")
        return 2
    data = open(sys.argv[1], "rb").read()
    magic = data[0:8]
    if magic != b"A9FSCN1\x00":
        print("bad magic", magic)
        return 2
    base, owner, startA, startMs = struct.unpack_from("<QQQQ", data, 8)
    count = struct.unpack_from("<Q", data, 40)[0]
    window = struct.unpack_from("<I", data, 52)[0]
    ctrl = struct.unpack_from("<Q", data, 56)[0]
    stride = 8 + window
    rec = stride * 2 if ctrl else stride
    print(f"count={count} window=0x{window:x} owner=0x{owner:x} start=0x{startA:x} hasCtrl={ctrl!=0}")

    n = min(count, (len(data) - 64) // rec)
    menu_end = max(1, int(n * 0.15))
    race_start = max(menu_end, int(n * 0.25))
    print(f"samples={n} menu_end={menu_end} race_start={race_start}")

    # Column stats for every aligned float.
    menu_absmax = [0.0] * (window // 4)
    race_absmax = [0.0] * (window // 4)
    for i in range(n):
        base_off = 64 + i * rec + 8
        seg = 0 if i < menu_end else (1 if i > race_start else -1)
        if seg < 0:
            continue
        arr = menu_absmax if seg == 0 else race_absmax
        for j in range(window // 4):
            v = abs(struct.unpack_from("<f", data, base_off + j * 4)[0])
            if v > arr[j]:
                arr[j] = v

    # VELOCITY: triplets of floats menu<0.1, race>5.0
    vel = []
    for off in range(0, window // 4 - 2):
        if (menu_absmax[off] < 0.1 and menu_absmax[off + 1] < 0.1 and
                menu_absmax[off + 2] < 0.1 and
                race_absmax[off] > 5.0 and race_absmax[off + 1] > 5.0 and
                race_absmax[off + 2] > 5.0):
            vel.append(off * 4)
    print("--- VELOCITY candidates (menu~0, race>5) ---")
    for off in vel[:20]:
        print(f"  off=0x{off:x} abs=0x{startA + off:x} "
              f"race=({race_absmax[off//4]:.1f},{race_absmax[off//4+1]:.1f},{race_absmax[off//4+2]:.1f})")
    if not vel:
        print("  none")

    # TRANSFORM: blocks of >=16 consecutive floats with race translation motion.
    # Translation element k of a block: race_absmax varies AND per-sample delta large.
    # Simplify: any 16-consecutive-float window where translation (12..14)
    # race_absmax > 1.0 and menu_absmax also moderate (garage preview moves).
    blocks = []
    for off in range(0, window // 4 - 16):
        t12 = race_absmax[off + 12]
        t13 = race_absmax[off + 13]
        t14 = race_absmax[off + 14]
        if t12 > 1.0 and t13 > 1.0 and t14 > 1.0:
            blocks.append(off * 4)
    # Merge consecutive block starts.
    merged = []
    for off in blocks:
        if merged and off - merged[-1] <= 16:
            continue
        merged.append(off)
    print("--- TRANSFORM block candidates (translation>1.0) ---")
    for off in merged[:20]:
        print(f"  start_off=0x{off:x} abs=0x{startA + off:x} "
              f"trans=({race_absmax[off//4+12]:.1f},{race_absmax[off//4+13]:.1f},{race_absmax[off//4+14]:.1f})")
    if not merged:
        print("  none")
    return 0

if __name__ == "__main__":
    sys.exit(main())
