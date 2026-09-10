import json
import struct
import unittest
from track_collision_export import Track, IDENTITY, primitive


def fixture(shapes=None):
    meta = dict(Meta=dict(Version=1), Objects=[], Shapes=shapes or [dict(Type=26, ByteOffset=0, ByteLength=40)])
    manifest = json.dumps(meta).encode()
    return b'BCOL' + struct.pack('<I', len(manifest)) + manifest + struct.pack('<I', 40) + struct.pack('<9fi', 1, 2, 3, 0, 0, 0, 2, 3, 4, 7)


class Tests(unittest.TestCase):
    def test_box(self):
        triangles = list(primitive(dict(Type=0, HalfExtents=[1,2,3])))
        self.assertEqual(len(triangles), 12)
        self.assertEqual(max(v[2] for t in triangles for v in t), 3)

    def test_capsule_axes(self):
        for axis in range(3):
            h = [2,2,2]
            h[axis] = 5
            vertices = [v for t in primitive(dict(Type=10, HalfExtents=h, UpAxys=axis)) for v in t]
            self.assertAlmostEqual(max(v[axis] for v in vertices), 7)
            self.assertAlmostEqual(min(v[axis] for v in vertices), -7)
            self.assertAlmostEqual(max(v[(axis+1)%3] for v in vertices), 2)

    def test_sphere(self):
        for t in primitive(dict(Type=8, HalfExtents=[3,0,0])):
            for v in t:
                self.assertAlmostEqual(sum(x*x for x in v), 9)

    def test_mesh(self):
        vertices, material = next(Track(fixture()).triangles(0))
        self.assertEqual(vertices[0], (1, 2, 3))
        self.assertEqual(material, 7)

    def test_nested_transform(self):
        transform = IDENTITY.copy()
        transform[12] = 10
        t = Track(fixture([dict(Type=31, Children=[dict(ChildIndex=1, LocalTrans=transform)]),
                           dict(Type=22, ChildIndex=2, LocalScale=[2, 3, 4]),
                           dict(Type=26, ByteOffset=0, ByteLength=40)]))
        self.assertEqual(next(t.triangles(0))[0][0], (12, 6, 12))

    def test_rotation(self):
        rotation = [0,1,0,0,-1,0,0,0,0,0,1,0,0,0,0,0]
        self.assertEqual(next(Track(fixture()).triangles(0, transforms=(rotation,)))[0][0], (-2,1,3))

    def test_cycle(self):
        t = Track(fixture([dict(Type=22, ChildIndex=0, LocalScale=[1,1,1])]))
        with self.assertRaises(ValueError): list(t.triangles(0))

    def test_truncated(self):
        with self.assertRaises(ValueError): Track(fixture()[:-1])

    def test_invalid_span(self):
        t = Track(fixture([dict(Type=26, ByteOffset=-1, ByteLength=40)]))
        with self.assertRaises(ValueError): list(t.triangles(0))

    def test_unknown_not_silent(self):
        t = Track(fixture([dict(Type=999)]))
        self.assertEqual(list(t.triangles(0)), [])
        self.assertEqual(t.unsupported, {'999': 1})


if __name__ == '__main__': unittest.main()
