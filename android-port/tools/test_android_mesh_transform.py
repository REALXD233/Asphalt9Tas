import struct
import unittest
from export_android_world_meshes import transform_for_bcol, Capture
from track_collision_export import point

class TransformTests(unittest.TestCase):
    def shape_head(self,kind):
        head=bytearray(128)
        struct.pack_into('<I',head,36,kind)
        return head

    def test_box_implicit_dimensions_not_scaled_twice(self):
        head=self.shape_head(0)
        struct.pack_into('<3f',head,0x30,2,3,4)
        struct.pack_into('<3f',head,0x40,5,6,7)
        struct.pack_into('<f',head,0x50,.04)
        c=Capture(0);c.span=lambda a,n:bytes(head)
        s=c.shapes[c.shape(100)]
        self.assertEqual(s['ImplicitHalfExtents'],[5,6,7])
        for actual,expected in zip(s['HalfExtents'],[5.04,6.04,7.04]):
            self.assertAlmostEqual(actual,expected)
        self.assertAlmostEqual(s['CollisionMargin'],.04)

    def test_thin_box_negative_implicit_dimension(self):
        head=self.shape_head(0)
        struct.pack_into('<3f',head,0x40,22.55,-.039886,19.45)
        struct.pack_into('<f',head,0x50,.04)
        c=Capture(0);c.span=lambda a,n:bytes(head)
        s=c.shapes[c.shape(100)]
        self.assertGreater(s['HalfExtents'][1],0)
        self.assertLess(s['HalfExtents'][1],.001)

    def test_sphere_effective_radius(self):
        head=self.shape_head(8)
        struct.pack_into('<f',head,0x30,2)
        struct.pack_into('<f',head,0x40,1.5)
        c=Capture(0);c.span=lambda a,n:bytes(head)
        self.assertEqual(c.shapes[c.shape(100)]['HalfExtents'],[3]*3)

    def test_capsule_axis(self):
        head=self.shape_head(10)
        struct.pack_into('<3f',head,0x40,.5,.5,.25)
        struct.pack_into('<I',head,0x58,2)
        c=Capture(0);c.span=lambda a,n:bytes(head)
        s=c.shapes[c.shape(100)]
        self.assertEqual(s['UpAxys'],2)
        self.assertEqual(s['HalfExtents'],[.5,.5,.25])

    def test_compound_child_transform_and_pointer(self):
        parent=self.shape_head(31);box=self.shape_head(0)
        struct.pack_into('<I',parent,0x34,1)
        struct.pack_into('<Q',parent,0x40,200)
        struct.pack_into('<3f',box,0x40,1,2,3)
        child=bytearray(88)
        struct.pack_into('<16f',child,0,*[0,-1,0,0,1,0,0,0,0,0,1,0,10,20,30,0])
        struct.pack_into('<Q',child,64,300)
        c=Capture(0);c.span=lambda a,n:bytes({100:parent,200:child,300:box}[a])
        s=c.shapes[c.shape(100)]
        self.assertEqual(c.shapes[s['Children'][0]['ChildIndex']]['Type'],0)
        self.assertEqual(point(s['Children'][0]['LocalTrans'],[2,3,4]),(7,22,34))

    def test_android_rows_become_bcol_columns(self):
        raw = [0,-1,0,0, 1,0,0,0, 0,0,1,0, 10,20,30,0]
        matrix = transform_for_bcol(struct.pack('<16f',*raw))
        self.assertEqual(point(matrix,[2,3,4]),(7,22,34))

    def test_nonfinite_rejected(self):
        with self.assertRaises(ValueError):
            transform_for_bcol(struct.pack('<16f',*([float('nan')]*16)))

if __name__ == '__main__':
    unittest.main()
