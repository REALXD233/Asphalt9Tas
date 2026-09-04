#!/usr/bin/env python3
"""Find yellow-green button regions in a game screenshot (no PIL needed)."""
import zlib
import struct
import sys


def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    pos = 8
    width = height = None
    idat = b''
    color_type = 0
    while pos < len(d):
        ln = struct.unpack('>I', d[pos:pos + 4])[0]
        typ = d[pos + 4:pos + 8]
        data = d[pos + 8:pos + 8 + ln]
        if typ == b'IHDR':
            width, height, _, color_type = struct.unpack('>IIBB', data[:10])
        elif typ == b'IDAT':
            idat += data
        pos += 12 + ln
    raw = zlib.decompress(idat)
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}[color_type]
    stride = width * bpp
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _y in range(height):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa = abs(b - c)
                pb = abs(a - c)
                pc = abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out += line
        prev = line
    return width, height, bpp, out


def is_yellow_green(r, g, b):
    # yellow-green: high red+green, low blue
    return g > 140 and r > 90 and g > b + 60 and r > b + 60


def find_regions(path, min_area=200):
    w, h, bpp, px = read_png(path)
    mask = [0] * (w * h)
    for y in range(h):
        for x in range(w):
            idx = (y * w + x) * bpp
            if is_yellow_green(px[idx], px[idx + 1], px[idx + 2]):
                mask[y * w + x] = 1
    seen = [False] * (w * h)
    regions = []
    for y in range(h):
        for x in range(w):
            if mask[y * w + x] and not seen[y * w + x]:
                # BFS
                stack = [(x, y)]
                seen[y * w + x] = True
                xs = [x]
                ys = [y]
                while stack:
                    cx, cy = stack.pop()
                    for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                        if 0 <= nx < w and 0 <= ny < h and mask[ny * w + nx] and not seen[ny * w + nx]:
                            seen[ny * w + nx] = True
                            stack.append((nx, ny))
                            xs.append(nx)
                            ys.append(ny)
                area = len(xs)
                if area >= min_area:
                    regions.append((area, min(xs), min(ys), max(xs), max(ys)))
    regions.sort(reverse=True)
    return regions


if __name__ == '__main__':
    path = sys.argv[1]
    min_area = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    regions = find_regions(path, min_area)
    print(f'{len(regions)} yellow-green regions (min_area={min_area}):')
    for area, x0, y0, x1, y1 in regions[:20]:
        cx = (x0 + x1) // 2
        cy = (y0 + y1) // 2
        print(f'  area={area:6d} box=({x0},{y0})-({x1},{y1}) center=({cx},{cy})')
