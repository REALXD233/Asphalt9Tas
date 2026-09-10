"""Read upstream BCOL v1 .TRACK collision geometry; no game access required.

Format reference: AsphaltTool/shared/src/BulletSerializer.h and BulletTypes.h.
Exports world-space triangle meshes to OBJ; unsupported primitives are reported,
never silently represented as exact collision meshes. Does not classify reset walls.
"""
import argparse
import collections
import hashlib
import json
import math
from pathlib import Path
import struct

IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0]


def vector(value, size):
    if not isinstance(value, list) or len(value) != size or not all(
            type(x) in (float, int) and math.isfinite(x) for x in value):
        raise ValueError('invalid finite vector')
    return value


def point(matrix, p):
    # Upstream ComposeBulletTransforms uses basis columns and origin at 12.
    return tuple(matrix[r] * p[0] + matrix[4+r] * p[1] +
                 matrix[8+r] * p[2] + matrix[12+r] for r in range(3))


def primitive(s):
    """Display tessellation; implicit dimensions, not expanded collision margins."""
    h = vector(s['HalfExtents'], 3)
    if min(h) < 0:
        raise ValueError('negative primitive extent')
    if s['Type'] == 0:
        v = [(x*h[0], y*h[1], z*h[2]) for x,y,z in
             [(-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1),(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]]
        for a,b,c,d in [(0,3,2,1),(4,5,6,7),(0,1,5,4),(2,3,7,6),(0,4,7,3),(1,2,6,5)]:
            yield [v[a],v[b],v[c]]
            yield [v[a],v[c],v[d]]
        return
    axis = s.get('UpAxys', 1) if s['Type'] == 10 else 1
    if axis not in (0,1,2):
        raise ValueError('invalid capsule axis')
    radius = h[(axis+2)%3] if s['Type'] == 10 else h[0]
    half = h[axis] if s['Type'] == 10 else 0
    rings = []
    # Separate hemisphere equators provide a real cylindrical middle section.
    for sign in (-1,1):
        for k in range(9):
            angle = -math.pi/2 + k*math.pi/16 if sign < 0 else k*math.pi/16
            ring = []
            for j in range(24):
                p = [0.,0.,0.]
                p[axis] = radius*math.sin(angle) + sign*half
                p[(axis+1)%3] = radius*math.cos(angle)*math.cos(j*math.tau/24)
                p[(axis+2)%3] = radius*math.cos(angle)*math.sin(j*math.tau/24)
                ring.append(p)
            rings.append(ring)
    for lower,upper in zip(rings,rings[1:]):
        for j in range(24):
            n = (j+1)%24
            yield [lower[j],lower[n],upper[n]]
            yield [lower[j],upper[n],upper[j]]


class Track:
    def __init__(self, blob):
        if len(blob) < 12 or blob[:4] != b'BCOL':
            raise ValueError('not BCOL')
        size, = struct.unpack_from('<I', blob, 4)
        if size > 64 * 1024 * 1024 or 12 + size > len(blob):
            raise ValueError('truncated/oversized manifest')
        self.manifest = json.loads(blob[8:8+size])
        length, = struct.unpack_from('<I', blob, 8+size)
        if 12 + size + length != len(blob):
            raise ValueError('payload length mismatch')
        if self.manifest['Meta']['Version'] != 1:
            raise ValueError('unsupported BCOL version')
        self.payload = memoryview(blob)[12+size:]
        self.shapes = self.manifest['Shapes']
        self.objects = self.manifest['Objects']
        if not isinstance(self.shapes, list) or not isinstance(self.objects, list):
            raise ValueError('invalid tables')
        self.unsupported = collections.Counter()

    def triangles(self, index, chain=(), transforms=()):
        if type(index) is not int or not 0 <= index < len(self.shapes):
            raise ValueError('shape index out of bounds')
        if index in chain or len(chain) >= 64:
            raise ValueError('cyclic/deep compound')
        s = self.shapes[index]
        if not isinstance(s, dict) or 'Type' not in s:
            self.unsupported['empty_shape'] += 1
            return
        kind = s['Type']
        chain = (*chain, index)
        if kind == 31:
            for child in s['Children']:
                transform = vector(child['LocalTrans'], 16)
                yield from self.triangles(child['ChildIndex'], chain, (*transforms, transform))
        elif kind == 22:
            scale = vector(s['LocalScale'], 3)
            transform = IDENTITY.copy()
            transform[0], transform[5], transform[10] = scale
            yield from self.triangles(s['ChildIndex'], chain, (*transforms, transform))
        elif kind == 26:
            offset, length = s['ByteOffset'], s['ByteLength']
            if type(offset) is not int or type(length) is not int or offset < 0 or length < 0 or length % 40 or offset + length > len(self.payload):
                raise ValueError('invalid triangle span')
            for data in struct.iter_unpack('<9fi', self.payload[offset:offset+length]):
                vertices = []
                for j in (0, 3, 6):
                    p = vector(list(data[j:j+3]), 3)
                    for transform in reversed(transforms):
                        p = point(transform, p)
                    if not all(math.isfinite(x) for x in p):
                        raise ValueError('world coordinate overflow')
                    vertices.append(tuple(p))
                yield vertices, data[9]
        elif kind in (0, 8, 10):
            for triangle in primitive(s):
                vertices = []
                for p in triangle:
                    for transform in reversed(transforms):
                        p = point(transform, p)
                    vertices.append(tuple(p))
                yield vertices, -1
        else:
            self.unsupported[str(kind)] += 1


def export(source, destination):
    blob = source.read_bytes()
    track = Track(blob)
    # New directory only: interrupted exports never overwrite previous results.
    destination.mkdir(parents=True, exist_ok=False)
    count = 0
    materials = collections.Counter()
    scene = []
    with (destination / 'collision.obj').open('w', encoding='utf-8') as output:
        output.write('# Collision triangles; original game axes/units; not visual scenery\n')
        for i, obj in enumerate(track.objects):
            flags = obj.get('ColFlags', 0)
            native_attributes = obj.get('NativeAttributes', {})
            # Root attributes only: a tagged compound child must not color its siblings.
            tags = native_attributes.get('attributes', [])
            item = dict(id=i, flags=flags, vertices=[], attributes=tags,
                        attributeStatus=native_attributes.get('status', 'unknown'))
            scene.append(item)
            output.write(f'o object_{i}_flags_{flags}\n')
            for vertices, material in track.triangles(obj['ShapeIndex'], transforms=(vector(obj['Trans'], 16),)):
                output.write(f'g object_{i}_material_{material}\n')
                for vertex in vertices:
                    item['vertices'].extend(vertex)
                    output.write('v ' + ' '.join(format(x, '.9g') for x in vertex) + '\n')
                start = count * 3 + 1
                output.write(f'f {start} {start+1} {start+2}\n')
                count += 1
                materials[material] += 1
    report = dict(source=str(source.resolve()), sha256=hashlib.sha256(blob).hexdigest(),
                  objects=len(track.objects), triangles=count, materials=dict(materials),
                  unsupported_shape_instances=dict(track.unsupported),
                  wall_classification='native attributes where supplied; no-contact-response alone does not prove reset; gameplay effects unverified',
                  geometry='shape-defined surfaces; sphere/capsule tessellated; see shape_surface_notes for margin conventions',
                  shape_surface_notes=[dict(index=i, surface=s['Surface']) for i,s in enumerate(track.shapes) if 'Surface' in s],
                  object_metadata=track.objects)
    (destination / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    template = Path(__file__).with_name('track_viewer_template.html').read_text(encoding='utf-8')
    if 'AttributeEvidence' in track.manifest.get('Meta', {}):
        template = template.replace('上游历史样本 · 非安卓实时地图', '安卓实读快照 · 原生属性已关联')
    data = json.dumps(scene, separators=(',', ':'), allow_nan=False)
    (destination / 'viewer.html').write_text(template.replace('/*SCENE_DATA*/[]', data), encoding='utf-8')
    return count


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('track', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print('exported_triangles=', export(args.track, args.output))
