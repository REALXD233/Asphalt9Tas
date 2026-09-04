#!/usr/bin/env python3
"""Offline ABI and fail-closed tests for the source-proxy converter."""

from __future__ import annotations

import pathlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from analyze_hwbp_events_v5 import EVENT, HEADER  # noqa: E402
from make_source_proxy_replay_v1 import (  # noqa: E402
    BUILD_ID,
    FRAME,
    HEADER as REPLAY_HEADER,
    MAGIC,
    convert,
)


def float_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def make_trace(path: pathlib.Path, flags: int = 5) -> None:
    events = [
        EVENT.pack(0, 1_000, 10, 1, 0x100, float_bits(-1.0), 0, 1, 0),
        EVENT.pack(1, 1_100, 10, 2, 0x200, float_bits(-1.0), float_bits(0.5), 1, 0),
        EVENT.pack(2, 18_000, 10, 1, 0x100, 0, float_bits(0.5), 1, 0),
        EVENT.pack(3, 18_100, 10, 2, 0x200, 0, float_bits(-0.25), 1, 0),
    ]
    header = HEADER.pack(
        b"A9HEV5\0\0",
        5,
        HEADER.size,
        EVENT.size,
        flags,
        123,
        0x100000,
        0x200000,
        0x200C98,
        0x200C9C,
        900,
        len(events),
        2,
        2,
        0,
        0,
        0,
        0,
        1,
        1,
        bytes(24),
    )
    path.write_bytes(header + b"".join(events))


class ConverterTests(unittest.TestCase):
    def test_converts_complete_pairs_with_expected_axis_mapping(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            source = pathlib.Path(temp) / "trace.bin"
            output = pathlib.Path(temp) / "replay.bin"
            make_trace(source)
            self.assertEqual(convert(source, output), 2)
            data = output.read_bytes()
            header = REPLAY_HEADER.unpack_from(data)
            self.assertEqual(header[0], MAGIC)
            self.assertEqual(header[5], BUILD_ID)
            self.assertEqual(header[4], 2)
            first = FRAME.unpack_from(data, REPLAY_HEADER.size)
            second = FRAME.unpack_from(data, REPLAY_HEADER.size + FRAME.size)
            self.assertAlmostEqual(first[0], 0.5)
            self.assertAlmostEqual(first[1], -1.0)
            self.assertAlmostEqual(second[0], -0.25)
            self.assertAlmostEqual(second[1], 0.0)
            self.assertEqual(first[3], 7)

    def test_rejects_trace_without_build_signature_flag(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            source = pathlib.Path(temp) / "trace.bin"
            output = pathlib.Path(temp) / "replay.bin"
            make_trace(source, flags=1)
            with self.assertRaisesRegex(ValueError, "clean, complete"):
                convert(source, output)
            self.assertFalse(output.exists())

    def test_extracts_an_explicit_frame_range(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            source = pathlib.Path(temp) / "trace.bin"
            output = pathlib.Path(temp) / "replay.bin"
            make_trace(source)
            self.assertEqual(convert(source, output, start_frame=1, frame_count=1), 1)
            data = output.read_bytes()
            header = REPLAY_HEADER.unpack_from(data)
            self.assertEqual(header[4], 1)
            only = FRAME.unpack_from(data, REPLAY_HEADER.size)
            self.assertAlmostEqual(only[0], -0.25)
            self.assertAlmostEqual(only[1], 0.0)

    def test_rejects_out_of_bounds_frame_range(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            source = pathlib.Path(temp) / "trace.bin"
            output = pathlib.Path(temp) / "replay.bin"
            make_trace(source)
            with self.assertRaisesRegex(ValueError, "exceeds source trace"):
                convert(source, output, start_frame=1, frame_count=2)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
