import struct
import unittest
from a9tas_recording_v1 import decode_archive, encode_archive, RecordingError, A9G4R2_HEADER, A9G4R2_FRAME
from test_a9tas_recording_v1 import make_recording, make_manifest
from dump_a9tas_frames import decode_frames


class FrameDumpTests(unittest.TestCase):
    def test_signed_android_session_and_wrong_identity(self):
        raw = bytearray(make_recording())
        unsigned = (1 << 63) + 123
        struct.pack_into('<Q', raw, 40, unsigned)
        manifest = make_manifest(bytes(raw))
        manifest['recording']['session_id'] = unsigned - (1 << 64)
        archive = decode_archive(encode_archive(manifest, bytes(raw)))
        self.assertEqual(archive.source.session_id, unsigned)
        manifest['recording']['session_id'] -= 1
        with self.assertRaises(RecordingError):
            encode_archive(manifest, bytes(raw))

    def test_all_frame_bytes_and_intervals(self):
        raw = make_recording()
        archive = decode_archive(encode_archive(make_manifest(raw), raw))
        frames = list(decode_frames(archive))
        self.assertEqual(len(frames), 3)
        rebuilt = b''.join(bytes.fromhex(f['raw_frame_hex']) for f in frames)
        self.assertEqual(rebuilt, raw[A9G4R2_HEADER.size:A9G4R2_HEADER.size + 3 * A9G4R2_FRAME.size])
        self.assertEqual(sum(len(f['physics_intervals']) for f in frames), 3)
        self.assertEqual(frames[1]['nitro_activation_count'], 1)
        self.assertEqual(frames[0]['float32_bits_le']['controls'][1], '0xbf800000')
        self.assertEqual(len(frames[0]['transform_float32']), 16)


if __name__ == '__main__':
    unittest.main()
