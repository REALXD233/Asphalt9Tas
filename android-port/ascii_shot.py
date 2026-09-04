#!/usr/bin/env python3
"""ASCII-art renderer for game screenshots (works without PIL)."""
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


def render(path, cols=100, rows=42, y0=0, y1=None):
    w, h, bpp, px = read_png(path)
    if y1 is None:
        y1 = h
    chars = ' .:-=+*#%@'
    for r in range(y0, y1, max(1, (y1 - y0) // rows)):
        line = ''
        for c in range(0, w, max(1, w // cols)):
            idx = (r * w + c) * bpp
            rr = px[idx]
            gg = px[idx + 1]
            bb = px[idx + 2]
            lum = (rr * 299 + gg * 587 + bb * 114) // 1000
            line += chars[lum * 9 // 256]
        print(f'{r:4d} {line}')


if __name__ == '__main__':
    path = sys.argv[1]
    cols = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    rows = int(sys.argv[3]) if len(sys.argv) > 3 else 42
    y0 = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    y1 = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    render(path, cols, rows, y0, y1 or None)
