"""Read proven primitive shape user attributes; no target writes or calls.

Layout is specific to Huawei core 439fd7f9, not a universal ABI.
Compound children are traversed; mesh attributes are deliberately unclassified.
"""
import argparse
import collections
import json
import struct
from pathlib import Path
from read_collision_span import read

def capture(census, base):
    pid = census['pid']
    spans = {}
    def stable(address, length):
        key = (address, length)
        if key not in spans:
            a = read(pid, address, length)
            if a != read(pid, address, length):
                raise ValueError(f'changed span {address:x}')
            spans[key] = a
        return spans[key]
    def q(data, off=0):
        return struct.unpack_from('<Q', data, off)[0]
    # 59714fc: indexed name vector, string length at +0, bytes at +0x18.
    vector = q(stable(base + 0xa5f6018, 8))
    names = []
    for ptr in struct.unpack('<6Q', stable(vector, 48)):
        length = struct.unpack('<I', stable(ptr, 4))[0]
        if not 0 < length <= 64:
            raise ValueError('unexpected attribute name length')
        names.append(stable(ptr + 24, length).decode('ascii'))
    if names != ['ramp', 'magnet', 'highjump', 'wreck', 'respawn', 'nochassis']:
        raise ValueError(f'unexpected live enum {names}')
    def shape(address, depth=0):
        if depth > 8:
            raise ValueError('compound depth exceeded')
        head = stable(address, 0x48)
        typ = struct.unpack_from('<I', head, 0x24)[0]
        result = dict(address=hex(address), type=typ)
        if typ in (0, 8, 10):
            ptr = q(head, 0x28)
            if not ptr:
                return dict(result, status='no_attribute_pointer')
            packed = struct.unpack('<I', stable(ptr, 4))[0]
            result.update(status='decoded', packed=packed,
                category_raw=packed & 255, secondary_raw=(packed >> 8) & 255,
                attribute_mask=packed >> 16,
                attributes=[name for bit, name in enumerate(names) if packed & (1 << (16 + bit))],
                unknown_attribute_bits=(packed >> 16) & ~63)
        elif typ == 31:
            count = struct.unpack_from('<i', head, 0x34)[0]
            if not 0 <= count <= 256:
                raise ValueError('unexpected compound count')
            children = stable(q(head, 0x40), count * 88) if count else b''
            result.update(status='compound', children=[shape(q(children, i*88+64), depth+1) for i in range(count)])
        else:
            result['status'] = 'not_proven_for_this_shape_type'
        return result
    rows = []
    for group in census['array_candidates']:
        if group['offset'] not in (0x148, 0x160):
            continue
        field = 0x90 if group['offset'] == 0x148 else 0x50
        for i, item in enumerate(group['heads']):
            link = next(x for x in item['linked_heads'] if x['field'] == field)
            native = int(link['pointer'], 16)
            old = bytes.fromhex(link['head_hex'])
            shape_ptr = q(old, 0xd0)
            if q(stable(native+0xd0, 8)) != shape_ptr:
                raise ValueError('stale census shape')
            rows.append(dict(group=hex(group['offset']), member=i, native=hex(native), shape=shape(shape_ptr)))
    leaves = []
    def collect(s):
        if s['status'] == 'decoded':
            leaves.append(s)
        for child in s.get('children', []):
            collect(child)
    for row in rows:
        collect(row['shape'])
    return dict(pid=pid, base=hex(base), attribute_names=names, objects=rows,
        summary=dict(objects=len(rows), decoded_primitive_occurrences=len(leaves),
            packed_counts=dict(collections.Counter(hex(x['packed']) for x in leaves)),
            attribute_counts=dict(collections.Counter(n for x in leaves for n in x['attributes']))),
        limitation='Attributes describe shapes, not proof of gameplay contact outcome; mesh/scaled-mesh attributes unresolved. Repeated reads are not an atomic snapshot.',
        reads=[dict(address=hex(a), length=n, hex=b.hex()) for (a,n),b in spans.items()])

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('census', type=Path)
    p.add_argument('base', type=lambda x: int(x,16))
    p.add_argument('output', type=Path)
    a = p.parse_args()
    result = capture(json.loads(a.census.read_text(encoding='utf-8-sig')), a.base)
    with a.output.open('x', encoding='utf-8') as f:
        json.dump(result, f, indent=2)
    print(json.dumps(result['summary']))
