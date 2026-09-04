#!/usr/bin/env python3
"""Tests for Gate 6 input/report cross-binding."""

from __future__ import annotations

import struct
import unittest

from make_unified_phase_only_gate_v1 import build_phase_only_gate
from native_physics_recording_v1 import NativePhysicsFrameV1, encode_recording as encode_a9nps1
from parse_unified_executor_report_v2 import (
    AUDIT_EXACT,
    BUILD_ID,
    COMMITTED,
    CORRECTION_SKIPPED,
    GATE2_COMPLETE,
    PREFIX_CERTIFIED,
    _HEADER,
)
from parse_unified_executor_report_v5 import (
    FRAME_SIZE,
    HEADER_SIZE,
    MAGIC,
    VERSION,
    _FRAME,
)
from unified_tick_recording_v1 import decode_recording
from verify_gate6_phase_only_result_v1 import verify_gate6_result


def floats(seed: float, count: int) -> bytes:
    return struct.pack(f"<{count}f", *(seed + index for index in range(count)))


def input_and_manifest() -> tuple[bytes, dict[str, object]]:
    source = encode_a9nps1([NativePhysicsFrameV1(7, 99, floats(1.0, 16), floats(30.0, 3))])
    return build_phase_only_gate(source, frame_index=0, fixed_interval_us=16667)


def report_for(input_blob: bytes) -> bytes:
    _, frames = decode_recording(input_blob)
    frame = frames[0]
    native = 0x77000000
    before = bytearray(804)
    before[0x10:0x50] = frame.transform
    before[0x150:0x15C] = frame.linear_velocity
    audit = _FRAME.pack(
        frame.tick,
        frame.monotonic_ns,
        300,
        CORRECTION_SKIPPED | AUDIT_EXACT | GATE2_COMPLETE | COMMITTED | PREFIX_CERTIFIED,
        16667,
        16667,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        100,
        101,
        1,
        3561,
        bytes(2),
        0,
        bytes(4),
        0,
        0,
        frame.transform,
        frame.linear_velocity,
        bytes(before),
        bytes(before),
    )
    addresses = (
        2668,
        0x70000000,
        0x71000000,
        0x72000000,
        0x73000000,
        0x730001D0,
        0x730001A0,
        0x74000F64,
        0x75000000,
        0x75000188,
        0x74000000,
        0x76000000,
        native,
        native + 0x10,
        native + 0x150,
    )
    counters = (7, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 4)
    header = _HEADER.pack(
        MAGIC,
        VERSION,
        HEADER_SIZE,
        FRAME_SIZE,
        0x1F,
        1,
        1,
        BUILD_ID,
        0,
        *addresses,
        *counters,
        10,
        14,
    )
    return header + audit


class Gate6ResultTests(unittest.TestCase):
    def test_accepts_bound_phase_only_result(self) -> None:
        input_blob, manifest = input_and_manifest()
        self.assertEqual(verify_gate6_result(report_for(input_blob), input_blob, manifest)[0], 7)

    def test_rejects_report_payload_not_bound_to_input(self) -> None:
        input_blob, manifest = input_and_manifest()
        report = bytearray(report_for(input_blob))
        report[HEADER_SIZE + 144] ^= 1
        with self.assertRaisesRegex(ValueError, "bound"):
            verify_gate6_result(bytes(report), input_blob, manifest)

    def test_rejects_active_control_write(self) -> None:
        input_blob, manifest = input_and_manifest()
        report = bytearray(report_for(input_blob))
        control_writes_offset = 8 + 24 + 20 + 4 + (15 + 2) * 8
        struct.pack_into("<Q", report, control_writes_offset, 2)
        with self.assertRaisesRegex(ValueError, "steering write count"):
            verify_gate6_result(bytes(report), input_blob, manifest)

    def test_rejects_non_skipped_correction_mode(self) -> None:
        input_blob, manifest = input_and_manifest()
        report = bytearray(report_for(input_blob))
        flags_offset = HEADER_SIZE + 20
        struct.pack_into("<I", report, flags_offset, 0x10 | 0x20 | 0x40 | 0x80 | 0x02)
        with self.assertRaises(ValueError):
            verify_gate6_result(bytes(report), input_blob, manifest)

    def test_rejects_manifest_mismatch(self) -> None:
        input_blob, manifest = input_and_manifest()
        manifest["fixed_interval_us"] = 20000
        with self.assertRaisesRegex(ValueError, "manifest"):
            verify_gate6_result(report_for(input_blob), input_blob, manifest)

    def test_rejects_more_than_one_fixed_delta_write(self) -> None:
        input_blob, manifest = input_and_manifest()
        report = bytearray(report_for(input_blob))
        delta_writes_offset = 8 + 24 + 20 + 4 + (15 + 1) * 8
        struct.pack_into("<Q", report, delta_writes_offset, 2)
        with self.assertRaisesRegex(ValueError, "fixed-delta"):
            verify_gate6_result(bytes(report), input_blob, manifest)

    def test_rejects_missing_deferred_callback_clear(self) -> None:
        input_blob, manifest = input_and_manifest()
        report = bytearray(report_for(input_blob))
        struct.pack_into("<Q", report, HEADER_SIZE + 80, 5)
        with self.assertRaisesRegex(ValueError, "deferred callback clear"):
            verify_gate6_result(bytes(report), input_blob, manifest)

    def test_rejects_commit_on_physics_owner(self) -> None:
        input_blob, manifest = input_and_manifest()
        report = bytearray(report_for(input_blob))
        struct.pack_into("<i", report, HEADER_SIZE + 114, 300)
        with self.assertRaisesRegex(ValueError, "independent commit tid"):
            verify_gate6_result(bytes(report), input_blob, manifest)


if __name__ == "__main__":
    unittest.main()
