from __future__ import annotations

import unittest
from unittest import mock

import parse_unified_executor_report_v8 as parser
from parse_unified_executor_report_v2 import HEADER_SIZE, _HEADER
from parse_unified_executor_report_v5 import FRAME_SIZE, _FRAME


class A9uer8AdapterTest(unittest.TestCase):
    @staticmethod
    def fixture() -> bytes:
        values = [parser.MAGIC, parser.VERSION, HEADER_SIZE, FRAME_SIZE,
                  0, 1, 1, bytes(20), 0]
        values += [0] * 29
        values += [0, 0]
        return _HEADER.pack(*values) + bytes(FRAME_SIZE)

    def test_exact_abi_is_delegated_to_complete_v6_verifier(self) -> None:
        sentinel = object()
        with mock.patch.object(parser, "decode_report_v6",
                               return_value=sentinel) as delegated:
            self.assertIs(parser.decode_report(self.fixture()), sentinel)
        synthetic = delegated.call_args.args[0]
        header = _HEADER.unpack_from(synthetic)
        self.assertEqual((header[0], header[1]), (b"A9UER6\0\0", 6))
        self.assertEqual(synthetic[HEADER_SIZE:], bytes(FRAME_SIZE))

    def test_wrong_magic_or_length_fails_before_delegation(self) -> None:
        with self.assertRaises(ValueError):
            parser.decode_report(b"bad")
        fixture = bytearray(self.fixture())
        fixture[0] ^= 1
        with self.assertRaises(ValueError):
            parser.decode_report(bytes(fixture))

    def test_completion_write_certificate_allows_same_value_and_is_stripped(
            self) -> None:
        blob = bytearray(self.fixture())
        header = list(_HEADER.unpack_from(blob))
        header[4] = parser.COMPLETION_WRITE_HEADER
        frame = list(_FRAME.unpack_from(blob, HEADER_SIZE))
        frame[3] = parser.COMPLETION_WRITE_FRAME
        frame[13] = 0x1234
        frame[14] = 0x1234
        certified = _HEADER.pack(*header) + _FRAME.pack(*frame)
        sentinel = object()
        with mock.patch.object(parser, "decode_report_v6",
                               return_value=sentinel) as delegated:
            self.assertIs(parser.decode_report(certified), sentinel)
        synthetic = delegated.call_args.args[0]
        synthetic_header = _HEADER.unpack_from(synthetic)
        synthetic_frame = _FRAME.unpack_from(synthetic, HEADER_SIZE)
        self.assertEqual(synthetic_header[4] &
                         parser.COMPLETION_WRITE_HEADER, 0)
        self.assertEqual(synthetic_frame[3] &
                         parser.COMPLETION_WRITE_FRAME, 0)
        self.assertEqual(synthetic_frame[13], 0x1234)
        self.assertNotEqual(synthetic_frame[14], synthetic_frame[13])

    def test_header_certificate_requires_every_frame_certificate(self) -> None:
        blob = bytearray(self.fixture())
        header = list(_HEADER.unpack_from(blob))
        header[4] = parser.COMPLETION_WRITE_HEADER
        malformed = _HEADER.pack(*header) + bytes(FRAME_SIZE)
        with self.assertRaisesRegex(ValueError,
                                    "completion write certificate is missing"):
            parser.decode_report(malformed)


if __name__ == "__main__":
    unittest.main()
