import unittest
from collections import Counter
from compare_track_objects import match_objects


def item(i, world, local, values=('a',)):
    return dict(index=i, world_hash=world, local_hash=local,
                counter=Counter(values), triangles=len(values))


class ObjectComparisonTests(unittest.TestCase):
    def test_world_match_ignores_object_order(self):
        matches, rest = match_objects([item(7,'w','l')], [item(2,'w','l')])
        self.assertEqual(matches[0]['b'], 2)
        self.assertEqual(matches[0]['classification'], 'same_world_geometry_and_material')
        self.assertEqual(rest, [])

    def test_local_match_is_not_world_match(self):
        matches, _ = match_objects([item(0,'w1','l')], [item(4,'w2','l')])
        self.assertEqual(matches[0]['classification'], 'same_local_geometry_world_differs')

    def test_duplicate_identity_remains_ambiguous(self):
        matches, _ = match_objects([item(0,'w','l'),item(1,'w','l')], [item(4,'w','l')])
        self.assertEqual(matches, [])

    def test_overlap_preserves_multiplicity(self):
        _, candidates = match_objects([item(0,'w1','l1',('a','a','b'))],
                                       [item(2,'w2','l2',('a','c'))])
        self.assertEqual(candidates[0]['common'], 1)
        self.assertEqual(candidates[0]['unmatched_a'], 2)


if __name__ == '__main__': unittest.main()
