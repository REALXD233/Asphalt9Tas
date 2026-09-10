import unittest
from analyze_material_components import components

class Tests(unittest.TestCase):
    def test_shared_edge(self):
        result=components([[(0,0,0),(1,0,0),(0,1,0)],[(1,0,0),(1,1,0),(0,1,0)]])
        self.assertEqual(len(result),1);self.assertEqual(result[0]['area'],1)
        self.assertEqual(result[0]['bounds'],[[0,0,0],[1,1,0]])
    def test_point_contact_not_edge_contact(self):
        self.assertEqual(len(components([[(0,0,0),(1,0,0),(0,1,0)],[(0,0,0),(-1,0,0),(0,-1,0)]])),2)
    def test_degenerate_and_empty(self):
        self.assertEqual(components([]),[])
        self.assertEqual(len(components([[(0,0,0)]*3,[(0,0,0)]*3])),2)

if __name__=='__main__':unittest.main()
