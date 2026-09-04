#!/usr/bin/env python3
"""Validate a finalized A9CSTR1 five-callback same-state receipt."""

from __future__ import annotations

import math
import pathlib
import struct
import sys


REPORT_SIZE = 320
FRAME_SIZE = 128
FRAME_COUNT = 5
FINAL_SIZE = REPORT_SIZE + FRAME_SIZE * FRAME_COUNT
FINAL_ACTION = 3
REQUIRED_REPORT_FLAGS = 0x3DF
REQUIRED_FRAME_FLAGS = 0xFF


def u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def i32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<i", data, offset)[0]


def u64(data: bytes, offset: int) -> int:
    return struct.unpack_from("<Q", data, offset)[0]


def validate(data: bytes) -> dict[str, int]:
    assert len(data) == FINAL_SIZE, (len(data), FINAL_SIZE)
    assert data[:8] == b"A9CSTR1\0"
    assert u32(data, 8) == 1
    assert u32(data, 12) == REPORT_SIZE
    assert u32(data, 16) == FINAL_ACTION
    flags = u32(data, 20)
    assert flags & REQUIRED_REPORT_FLAGS == REQUIRED_REPORT_FLAGS, hex(flags)
    assert u32(data, 144) == FRAME_COUNT
    assert u32(data, 148) == FRAME_COUNT
    assert u64(data, 152) == 0  # finalize does not reconfigure payload storage
    assert u64(data, 160) == 1  # one conditional callback-slot restore
    assert u64(data, 168) == 1
    assert u64(data, 176) == 0
    assert u64(data, 184) == 0

    evidence = data[192:REPORT_SIZE]
    assert evidence[:8] == b"A9CSE1\0\0"
    assert u32(evidence, 8) == 1
    assert u32(evidence, 12) == 128
    entries = u64(evidence, 16)
    original_calls = u64(evidence, 24)
    original_returns = u64(evidence, 32)
    combined_calls = u64(evidence, 40)
    combined_returns = u64(evidence, 48)
    fov_writes = u64(evidence, 56)
    recorded = u64(evidence, 64)
    idle = u64(evidence, 72)
    assert entries >= FRAME_COUNT
    assert original_calls == entries
    assert original_returns == original_calls
    assert combined_calls == FRAME_COUNT
    assert combined_returns == FRAME_COUNT
    assert fov_writes == FRAME_COUNT
    assert recorded == FRAME_COUNT
    assert idle == entries - FRAME_COUNT
    assert u64(evidence, 80) == 0
    assert u64(evidence, 88) == 0
    assert u32(evidence, 96) == FRAME_COUNT
    assert i32(evidence, 100) == 2
    assert u64(evidence, 104) == u64(data, 56)   # manager
    assert u64(evidence, 112) == u64(data, 72)   # node
    assert u64(evidence, 120) == u64(data, 64)   # shape

    producer_tid = 0
    for index in range(FRAME_COUNT):
        start = REPORT_SIZE + index * FRAME_SIZE
        frame = data[start:start + FRAME_SIZE]
        assert u32(frame, 0) == index
        tid = u32(frame, 4)
        assert tid > 0
        if producer_tid == 0:
            producer_tid = tid
        assert tid == producer_tid
        assert u32(frame, 8) == REQUIRED_FRAME_FLAGS, hex(u32(frame, 8))
        values = struct.unpack_from("<23f", frame, 16)
        assert all(math.isfinite(value) for value in values)
        assert frame[100:104] == frame[104:108]  # exact same-bit FOV write
        assert frame[108:120] == bytes(12)

    return {
        "frames": FRAME_COUNT,
        "producer_tid": producer_tid,
        "wrapper_entries": entries,
        "idle_entries": idle,
        "flags": flags,
    }


def make_selftest_blob() -> bytes:
    data = bytearray(FINAL_SIZE)
    data[:8] = b"A9CSTR1\0"
    struct.pack_into("<IIII", data, 8, 1, REPORT_SIZE, FINAL_ACTION,
                     REQUIRED_REPORT_FLAGS)
    addresses = [101, 202, 303, 404, 0x1000, 0x2000, 0x3000, 0x4000,
                 0x5000, 0x6000, 0x7000, 0x8000, 0x9000, 0xA000, 0xB000]
    struct.pack_into("<15Q", data, 24, *addresses)
    struct.pack_into("<II5Q", data, 144, FRAME_COUNT, FRAME_COUNT,
                     0, 1, 1, 0, 0)
    evidence = 192
    data[evidence:evidence + 8] = b"A9CSE1\0\0"
    struct.pack_into("<II10QIi3Q", data, evidence + 8,
                     1, 128,
                     7, 7, 7, 5, 5, 5, 5, 2, 0, 0,
                     FRAME_COUNT, 2,
                     addresses[4], addresses[6], addresses[5])
    for index in range(FRAME_COUNT):
        start = REPORT_SIZE + index * FRAME_SIZE
        struct.pack_into("<IIII", data, start, index, 1234,
                         REQUIRED_FRAME_FLAGS, 0)
        struct.pack_into("<23f", data, start + 16,
                         *[float(index + 1)] * 21, 1.25, 1.25)
    return bytes(data)


def main() -> int:
    if len(sys.argv) == 1 or sys.argv[1:] == ["--selftest"]:
        blob = make_selftest_blob()
        result = validate(blob)
        damaged = bytearray(blob)
        damaged[REPORT_SIZE + 104] ^= 1
        try:
            validate(bytes(damaged))
        except AssertionError:
            pass
        else:
            raise AssertionError("same-bit FOV corruption was accepted")
        print(
            "CAMERA_RACEVIEW_SAME_STATE_REPORT_VALIDATOR_SELFTEST "
            f"passed=1 frames={result['frames']} corruption_rejected=1 "
            "device_access=0"
        )
        return 0
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} [--selftest|FINAL_REPORT]")
    path = pathlib.Path(sys.argv[1])
    result = validate(path.read_bytes())
    print(
        "CAMERA_RACEVIEW_SAME_STATE_REPORT_VALID "
        f"frames={result['frames']} producer_tid={result['producer_tid']} "
        f"wrapper_entries={result['wrapper_entries']} "
        f"idle_entries={result['idle_entries']} flags=0x{result['flags']:x}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
