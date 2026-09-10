import struct
import unittest
from unittest.mock import patch
from capture_shape_attributes import capture

class ShapeAttributesTests(unittest.TestCase):
    def fixture(self, packed):
        base, vector, native, shape, prop = 0x10000000, 0x2000, 0x3000, 0x4000, 0x5000
        memory = {base+0xa5f6018: struct.pack('<Q', vector),
                  vector: struct.pack('<6Q', *range(0x6000, 0x6600, 0x100)),
                  native+0xd0: struct.pack('<Q', shape), prop: struct.pack('<I', packed)}
        for i, name in enumerate(['ramp', 'magnet', 'highjump', 'wreck', 'respawn', 'nochassis']):
            memory[0x6000+i*0x100] = struct.pack('<I',len(name))
            memory[0x6018+i*0x100] = name.encode()
        head = bytearray(0x48)
        struct.pack_into('<Q', head, 0x28, prop)
        memory[shape] = bytes(head)
        old = bytearray(0xd8)
        struct.pack_into('<Q',old,0xd0,shape)
        census = dict(pid=123,array_candidates=[dict(offset=0x160,heads=[dict(linked_heads=[
            dict(field=0x50,pointer=f'{native:x}',head_hex=old.hex())])])])
        def reader(pid,address,length):
            self.assertEqual(pid,123)
            value=memory[address]
            self.assertEqual(len(value),length)
            return value
        return base,memory,census,reader

    def test_bit_names_not_no_contact_inference(self):
        base,_,census,reader=self.fixture(13 | (2<<8) | (24<<16))
        with patch('capture_shape_attributes.read',reader):
            result=capture(census,base)['objects'][0]['shape']
        self.assertEqual(result['attributes'],['wreck','respawn'])
        self.assertEqual(result['category_raw'],13)
        self.assertEqual(result['secondary_raw'],2)

    def test_zero_flags_and_unknown_bits(self):
        for packed,unknown in [(13,0),(13 | (128<<16),128)]:
            base,_,census,reader=self.fixture(packed)
            with patch('capture_shape_attributes.read',reader):
                result=capture(census,base)['objects'][0]['shape']
            self.assertEqual(result['attributes'],[])
            self.assertEqual(result['unknown_attribute_bits'],unknown)

    def test_stale_native_shape(self):
        base,memory,census,reader=self.fixture(13)
        memory[0x30d0]=struct.pack('<Q',0x8000)
        with patch('capture_shape_attributes.read',reader):
            with self.assertRaisesRegex(ValueError,'stale census'):
                capture(census,base)

if __name__ == '__main__':
    unittest.main()
