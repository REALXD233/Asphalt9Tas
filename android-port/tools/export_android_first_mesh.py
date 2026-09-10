"""Export the live, statically verified Huawei mesh layout. No target writes/calls.

Explicit addresses are session-specific, not a general BuildProfile locator.
Mesh descriptor layout is proven by native RVA 0x5e56954 (48-byte elements).
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
from read_collision_span import read
from track_collision_export import export

def capture(pid, shape, native, destination):
    head = read(pid, shape, 128)
    if struct.unpack_from('<I', head, 0x24)[0] != 26:
        raise ValueError('not verified mesh type 26')
    interface = struct.unpack_from('<Q', head, 0x58)[0]
    iface = read(pid, interface, 64)
    count = struct.unpack_from('<I', iface, 0x1c)[0]
    if count != 1:
        raise ValueError('first-mesh exporter expects one subpart')
    descriptor = struct.unpack_from('<Q', iface, 0x28)[0]
    desc = read(pid, descriptor, 48)
    faces = struct.unpack_from('<I', desc)[0]
    indices_addr = struct.unpack_from('<Q', desc, 8)[0]
    index_stride, vertices_count = struct.unpack_from('<II', desc, 16)
    vertices_addr = struct.unpack_from('<Q', desc, 24)[0]
    vertex_stride, index_type, vertex_type = struct.unpack_from('<III', desc, 32)
    if (index_stride, vertex_stride, index_type, vertex_type) != (6, 12, 3, 0):
        raise ValueError('unsupported descriptor layout')
    if not (0 < faces*6 <= 1048576 and 0 < vertices_count*12 <= 1048576):
        raise ValueError('outside bounded first-mesh capture')
    transform_bytes = read(pid, native+16, 64)
    flags_blob = read(pid, native+0xe8, 4)
    flags = struct.unpack('<I', flags_blob)[0]
    transform = list(struct.unpack('<16f', transform_bytes))
    vertices_blob = read(pid, vertices_addr, vertices_count*12)
    indices_blob = read(pid, indices_addr, faces*6)
    vertices = list(struct.iter_unpack('<3f', vertices_blob))
    indices = list(struct.iter_unpack('<3H', indices_blob))
    if not all(math.isfinite(v) for p in vertices for v in p):
        raise ValueError('nonfinite vertex')
    if max(max(t) for t in indices) >= len(vertices):
        raise ValueError('index out of bounds')
    for addr, value in [(shape, head), (interface, iface), (descriptor, desc),
                        (native+16, transform_bytes), (native+0xe8, flags_blob), (vertices_addr, vertices_blob),
                        (indices_addr, indices_blob)]:
        if read(pid, addr, len(value)) != value:
            raise ValueError('capture changed; no output published')
    destination.mkdir(parents=True, exist_ok=False)
    (destination/'vertices.f32').write_bytes(vertices_blob)
    (destination/'indices.u16').write_bytes(indices_blob)
    payload = b''.join(struct.pack('<9fi', *(v for i in t for v in vertices[i]), -1) for t in indices)
    manifest = {'Meta': {'Version': 1, 'Source': 'Android live read-only; first mesh only'},
                'Shapes': [{'Type': 26, 'ByteOffset': 0, 'ByteLength': len(payload)}],
                'Objects': [{'ShapeIndex': 0, 'Trans': transform, 'ColFlags': flags}]}
    raw = json.dumps(manifest).encode()
    track = destination/'android-first-mesh.TRACK'
    track.write_bytes(b'BCOL'+struct.pack('<I', len(raw))+raw+struct.pack('<I', len(payload))+payload)
    provenance = dict(pid=pid, shape=hex(shape), native=hex(native), interface=hex(interface),
        descriptor=hex(descriptor), vertices=vertices_count, triangles=faces,
        indices_sha256=hashlib.sha256(indices_blob).hexdigest(),
        vertices_sha256=hashlib.sha256(vertices_blob).hexdigest(), full_buffer_repeat_equal=True,
        aabb=[[min(p[k] for p in vertices) for k in range(3)],
              [max(p[k] for p in vertices) for k in range(3)]],
        source='emulator-5554 PID live /proc/mem O_RDONLY, not upstream TRACK',
        limitation='one mesh, material ids unknown (-1); no reset-wall classification; no textures')
    (destination/'provenance.json').write_text(json.dumps(provenance, indent=2), encoding='utf-8')
    result = export(track, destination/'viewer')
    page = destination/'viewer'/'viewer.html'
    page.write_text(page.read_text(encoding='utf-8').replace(
        '上游历史样本 · 非安卓实时地图', '安卓游戏实读 · 首份碰撞网格快照'), encoding='utf-8')
    print(json.dumps(dict(provenance, exported_triangles=result)))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('shape', type=lambda x: int(x, 16))
    parser.add_argument('native', type=lambda x: int(x, 16))
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    capture(args.pid, args.shape, args.native, args.destination)
