import unittest
from export_track_difference import classify

V=[(0,0,0),(1,0,0),(0,1,0)]
class DifferenceTests(unittest.TestCase):
    def test_same(self):
        self.assertEqual(classify([(V,1)],[(list(reversed(V)),1)]),([],[],0))
    def test_material_change(self):
        a,b,n=classify([(V,1)],[(V,2)])
        self.assertEqual((len(a),len(b),n),(1,1,1))
    def test_duplicates(self):
        a,b,n=classify([(V,1),(V,1)],[(V,1)])
        self.assertEqual((len(a),len(b),n),(1,0,0))
    def test_geometry_change(self):
        a,b,n=classify([(V,1)],[([(x+10,y,z) for x,y,z in V],1)])
        self.assertEqual((len(a),len(b),n),(1,1,0))
if __name__=='__main__': unittest.main()
