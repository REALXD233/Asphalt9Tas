import struct,unittest
from read_checkpoint_table_candidate import decode

class Tests(unittest.TestCase):
    def test_values_and_signed(self):
        keys=[123,456,789];address=0x100000
        for value in (0,10000000,11000000,-1):
            bits=value&((1<<64)-1)
            raw=struct.pack('<3Q',*(bits^(address+i*8)^k for i,k in enumerate(keys)))
            self.assertEqual(decode(raw,address,keys),value)
    def test_corruption_not_silently_accepted(self):
        with self.assertRaisesRegex(ValueError,'replicas'):decode(struct.pack('<3Q',1,2,3),0,[0,0,0])
    def test_address_is_part_of_encoding(self):
        keys=[3,5,7];address=0x1007
        raw=struct.pack('<3Q',*(99^(address+i*8)^k for i,k in enumerate(keys)))
        self.assertEqual(decode(raw,address,keys),99)
        with self.assertRaises(ValueError):decode(raw,address+1,keys)

if __name__=='__main__':unittest.main()
