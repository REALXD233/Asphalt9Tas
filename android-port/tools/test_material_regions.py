import json,struct,tempfile,unittest
from pathlib import Path
from export_material_regions import build
from track_collision_export import Track,IDENTITY
from compare_collision_tracks import key

class MaterialRegionsTests(unittest.TestCase):
    def test_split_preserves_geometry_and_decodes_flags(self):
        table=bytearray(32);struct.pack_into('<I',table,8,0x8030b);struct.pack_into('<I',table,24,0x10030b)
        payload=struct.pack('<9fi',0,0,0,1,0,0,0,1,0,0)+struct.pack('<9fi',2,0,0,3,0,0,2,1,0,1)
        matrix=IDENTITY.copy();matrix[12]=10
        m=dict(Meta=dict(Version=1),Shapes=[dict(Type=26,ByteOffset=0,ByteLength=80,
            NativeMaterialTables=[dict(Stride=16,RawTable=table.hex())])],
            Objects=[dict(ShapeIndex=0,Trans=matrix,ColFlags=1)])
        raw=json.dumps(m).encode();blob=b'BCOL'+struct.pack('<I',len(raw))+raw+struct.pack('<I',len(payload))+payload
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'test.TRACK';path.write_bytes(blob);out,counts=build(path)
        t=Track(out)
        self.assertEqual(counts,dict(wreck=1,respawn=1))
        self.assertEqual(len(t.objects),2)
        self.assertEqual(list(t.triangles(0))[0][0][0],(10,0,0))
        self.assertEqual(t.objects[1]['NativeAttributes']['attributes'],['respawn'])

    def test_comparison_ignores_winding_not_position(self):
        v=[(0,0,0),(1,0,0),(0,1,0)]
        self.assertEqual(key(v,3),key(v[::-1],3))
        self.assertNotEqual(key(v,3),key([(x+1,y,z) for x,y,z in v],3))

if __name__=='__main__':unittest.main()
