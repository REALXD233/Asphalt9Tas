#!/usr/bin/env python3
"""Tests for Gate 7 steering-only input and A9UER5 cross-binding."""

from __future__ import annotations

import struct
import unittest

from make_unified_phase_only_gate_v1 import build_phase_only_gate
from make_unified_steering_gate_v1 import (
    STEERING_ONLY_SKIP_FLAGS,
    _A9SPR1_FRAME,
    _A9SPR1_HEADER,
    _BUILD_ID,
    build_steering_gate,
)
from native_physics_recording_v1 import NativePhysicsFrameV1, encode_recording as encode_a9nps1
from parse_unified_executor_report_v2 import (
    AUDIT_EXACT,
    BUILD_ID,
    COMMITTED,
    CORRECTION_SKIPPED,
    GATE2_COMPLETE,
    PREFIX_CERTIFIED,
    STEERING_APPLIED,
    _HEADER,
)
from parse_unified_executor_report_v5 import FRAME_SIZE, HEADER_SIZE, MAGIC, VERSION, _FRAME
from unified_tick_recording_v1 import decode_recording
from verify_gate7_steering_result_v1 import verify_gate7_result
from verify_unified_steering_gate_v1 import verify_steering_gate


def floats(seed: float, count: int) -> bytes:
    return struct.pack(f"<{count}f", *(seed + index for index in range(count)))


def phase_blob() -> bytes:
    source = encode_a9nps1(
        [NativePhysicsFrameV1(7, 99, floats(1.0, 16), floats(30.0, 3))]
    )
    return build_phase_only_gate(source, frame_index=0, fixed_interval_us=16667)[0]


def direction_blob(*, selected: float = 0.5, longitudinal: float = 0.0) -> bytes:
    count = 30
    output = bytearray(
        _A9SPR1_HEADER.pack(
            b"A9SPR1\0\0", 1, 64, 16, count, _BUILD_ID, bytes(20)
        )
    )
    for index in range(count):
        output += _A9SPR1_FRAME.pack(
            selected if index == 25 else 0.0,
            longitudinal if index == 25 else 0.0,
            0.0,
            7,
        )
    return bytes(output)


def input_and_manifest() -> tuple[bytes, dict[str, object]]:
    return build_steering_gate(
        phase_blob(), direction_blob(), direction_frame_index=25
    )


def report_for(input_blob: bytes) -> bytes:
    _, frames = decode_recording(input_blob)
    frame = frames[0]
    steering_bits = int.from_bytes(struct.pack("<f", frame.steering), "little")
    native = 0x77000000
    before = bytearray(804)
    before[0x10:0x50] = frame.transform
    before[0x150:0x15C] = frame.linear_velocity
    audit = _FRAME.pack(
        frame.tick,
        frame.monotonic_ns,
        300,
        STEERING_APPLIED
        | CORRECTION_SKIPPED
        | AUDIT_EXACT
        | GATE2_COMPLETE
        | COMMITTED
        | PREFIX_CERTIFIED,
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
        steering_bits,
        bytes(4),
        (steering_bits << 32) | 0x11111111,
        (steering_bits << 32) | 0x22222222,
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
    counters = (7, 1, 2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 4)
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


class SteeringGateTests(unittest.TestCase):
    def test_builds_and_verifies_exact_steering_only_input(self) -> None:
        blob, manifest = input_and_manifest()
        tick, interval, _, bits = verify_steering_gate(blob, manifest)
        self.assertEqual((tick, interval, bits), (7, 16667, "0000003f"))
        self.assertEqual(manifest["skip_flags"], STEERING_ONLY_SKIP_FLAGS)

    def test_rejects_weak_steering_source(self) -> None:
        with self.assertRaisesRegex(ValueError, "magnitude"):
            build_steering_gate(
                phase_blob(), direction_blob(selected=0.1), direction_frame_index=25
            )

    def test_rejects_active_longitudinal_source(self) -> None:
        with self.assertRaisesRegex(ValueError, "longitudinal"):
            build_steering_gate(
                phase_blob(), direction_blob(longitudinal=0.25), direction_frame_index=25
            )

    def test_rejects_manifest_steering_bit_mismatch(self) -> None:
        blob, manifest = input_and_manifest()
        manifest["steering_bits_hex"] = "0100003f"
        with self.assertRaisesRegex(ValueError, "steering_bits_hex"):
            verify_steering_gate(blob, manifest)

    def test_accepts_exact_a9uer5_cross_binding(self) -> None:
        blob, manifest = input_and_manifest()
        tick, _, bits = verify_gate7_result(report_for(blob), blob, manifest)
        self.assertEqual((tick, bits), (7, "0000003f"))

    def test_rejects_second_setter_wrong_steering_bits(self) -> None:
        blob, manifest = input_and_manifest()
        report = bytearray(report_for(blob))
        struct.pack_into("<I", report, HEADER_SIZE + 140, 0)
        with self.assertRaisesRegex(ValueError, "steering write audit"):
            verify_gate7_result(bytes(report), blob, manifest)

    def test_rejects_report_target_not_bound_to_input(self) -> None:
        blob, manifest = input_and_manifest()
        report = bytearray(report_for(blob))
        struct.pack_into("<I", report, HEADER_SIZE + 120, 0x3E800000)
        struct.pack_into("<I", report, HEADER_SIZE + 132, 0x3E800000)
        struct.pack_into("<I", report, HEADER_SIZE + 140, 0x3E800000)
        with self.assertRaisesRegex(ValueError, "not bound"):
            verify_gate7_result(bytes(report), blob, manifest)


if __name__ == "__main__":
    unittest.main()
