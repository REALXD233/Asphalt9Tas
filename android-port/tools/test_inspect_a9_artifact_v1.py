#!/usr/bin/env python3
"""Offline tests for the read-only A9 artifact inspector (B4/N1).

Every fixture is small and synthesized in a temporary directory.  No device,
no live runner, no copying of large evidence files.  The input files are
asserted byte-identical (SHA-256) before and after inspection.
"""

from __future__ import annotations

import hashlib
import io
import json
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_DIR))

import inspect_a9_artifact_v1 as inspector
from inspect_a9_artifact_v1 import inspect_bytes, main

# Authoritative codecs used to build fixtures (the same modules the inspector
# delegates to).
from native_physics_recording_v1 import (
    NativePhysicsFrameV1,
    encode_recording as encode_nps1,
)
from unified_tick_recording_v1 import (
    FRAME_SIZE as UTK1_FRAME_SIZE,
    HEADER_SIZE as UTK1_HEADER_SIZE,
    SUPPORTED_SKIP_MASK as UTK1_SKIP_FLAGS,
    UnifiedTickFrameV1,
    encode_recording as encode_utk1,
)
from synchronized_tick_recording_v1 import (
    FRAME_AUDIT_SIZE as USR_FRAME_SIZE,
    HEADER_SIZE as USR_HEADER_SIZE,
    SUPPORTED_BUILD_ID as USR_BUILD_ID,
    _FRAME as USR_FRAME,
    _HEADER as USR_HEADER,
)
from test_synchronized_tick_recording_v1 import make_capture, physics
from test_synchronized_brake_recording_v1 import make_brake_source
from test_synchronized_action_window_recording_v1 import make_action_window
from test_synchronized_action_until_release_recording_v1 import make_action_until_release
from test_parse_unified_executor_report_v1 import make_report as make_uer1
from test_parse_unified_executor_report_v2 import (
    AUDIT_EXACT,
    COMMITTED,
    CORRECTION_CORRECTED,
    CORRECTION_EQUAL,
    CORRECTION_SKIPPED,
    GATE2_COMPLETE,
    PREFIX_CERTIFIED,
    STEERING_APPLIED,
    _HEADER as UER_HEADER,
    make_report as make_uer2,
    payload,
    snapshot,
)
from parse_unified_executor_report_v2 import FRAME_SIZE as UER2_FRAME_SIZE
from parse_unified_executor_report_v3 import _FRAME as UER3_FRAME
from parse_unified_executor_report_v4 import _FRAME as UER4_FRAME
from parse_unified_executor_report_v5 import _FRAME as UER5_FRAME
from parse_unified_nitro_observe_report_v7 import (
    NITRO_RPC_OBSERVED,
    NITRO_RPC_USED,
    _FRAME as UER7_FRAME,
    _HEADER as UER7_HEADER,
    _RESPONSE,
)
from validate_fc1_report_v1 import REPORT_SIZE

SUPPORTED = (
    "A9NPS1", "A9UTK1", "A9USR1", "A9USR2", "A9USR3", "A9USR4",
    "A9UER1", "A9UER2", "A9UER3", "A9UER4", "A9UER5", "A9UER6", "A9UER7",
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_fixture(directory: Path, name: str, data: bytes) -> Path:
    path = directory / name
    path.write_bytes(data)
    return path


# ---------------------------------------------------------------------------
# Fixture builders (all synthetic).
# ---------------------------------------------------------------------------


def make_nps1(frames: int = 3, *, finite: bool = True, tick_step: int = 1) -> bytes:
    built = []
    for index in range(frames):
        transform = struct.pack(
            "<16f", *((float(index) + value) for value in range(16))
        )
        linear = struct.pack("<3f", index + 20.0, index + 21.0, index + 22.0)
        if not finite:
            transform = struct.pack("<16f", *([float("nan")] + [0.0] * 15))
        built.append(
            NativePhysicsFrameV1(
                tick=index * tick_step,
                monotonic_ns=1_000_000 + index,
                transform=transform,
                linear_velocity=linear,
            )
        )
    return encode_nps1(built)


def make_utk1(frames: int = 3) -> bytes:
    built = []
    for index in range(frames):
        transform = struct.pack(
            "<16f", *((float(index) + value) for value in range(16))
        )
        linear = struct.pack("<3f", index + 20.0, index + 21.0, index + 22.0)
        built.append(
            UnifiedTickFrameV1(
                tick=index,
                monotonic_ns=1_000_000 + index,
                steering=0.25 + index * 0.1,
                brake=0.0,
                accelerator=0.0,
                nitro_activations=0,
                skip_flags=UTK1_SKIP_FLAGS,
                respawn=False,
                barrel_angular=(0.0, 0.0, 0.0),
                barrel_rbx=(0.0, 0.0),
                transform=transform,
                linear_velocity=linear,
            )
        )
    return encode_utk1(built, fixed_interval_us=16667)


def make_usr1(frames: int = 3) -> bytes:
    """Synthetic A9USR1 report with N frames (steering stays within 1.05)."""
    audits: list[bytes] = []
    for index in range(frames):
        transform, linear = physics(float(index))
        steering = 0.25 + (index % 4) * 0.1  # 0.25..0.55, always <= 1.05
        steering_bits = struct.unpack("<I", struct.pack("<f", steering))[0]
        first_event = index * 7 + 1
        audits.append(
            USR_FRAME.pack(
                index,
                1_000_000 + index,
                2767,
                12624,
                16000 + index,
                16667,
                *range(first_event, first_event + 7),
                100 + index,
                101 + index,
                1,
                bytes(6),
                0x12345678,
                steering_bits << 32 | 0x12345678,
                transform,
                linear,
            )
        )
    qwords = (
        2668,
        0x100000,
        0x200000,
        0x300000,
        0x400000,
        0x400120,
        0x400130,
        0x500064,
        0x600000,
        0x600028,
        0x500000,
        0x700000,
        0x800000,
        0x800010,
        0x800150,
        frames * 7,
        frames,
        0,
        0,
        0,
        0,
        0,
    )
    header = USR_HEADER.pack(
        b"A9USR1\0\0",
        1,
        USR_HEADER_SIZE,
        USR_FRAME_SIZE,
        0x3F,
        frames,
        frames,
        USR_BUILD_ID,
        0,
        *qwords,
        268,
        268,
    )
    return header + b"".join(audits)


def make_usr2(frames: int = 3) -> bytes:
    return _retag_usr(make_usr1(frames), b"A9USR2\0\0", 2, 0x7F)


def make_usr3(frames: int = 3) -> bytes:
    return _retag_usr(make_usr1(frames), b"A9USR3\0\0", 3, 0xFF)


def make_usr4(frames: int = 30) -> bytes:
    header = list(USR_HEADER.unpack_from(make_usr1(frames)))
    header[0], header[1], header[4] = b"A9USR4\0\0", 4, 0x1FF
    header[5] = frames  # maximum_frames == captured_frames
    report = make_usr1(frames)
    return USR_HEADER.pack(*header) + report[USR_HEADER_SIZE:]


def _retag_usr(report: bytes, magic: bytes, version: int, flags: int) -> bytes:
    header = list(USR_HEADER.unpack_from(report))
    header[0], header[1], header[4] = magic, version, flags
    return USR_HEADER.pack(*header) + report[USR_HEADER_SIZE:]


# --- A9UER3..A9UER7 builders ------------------------------------------------

def _uer2_frame_fields(modes: tuple[int, ...]) -> tuple[list[list], tuple[int, ...]]:
    """A9UER2-shaped frame field lists with spaced phase events.

    Phase events use a 10-step per frame so that later versions (deferred
    clear, commit tid, steering/brake audits, nitro fields) can insert their
    extra fields without breaking the certified event order.
    """
    frames: list[list] = []
    equal = corrected = skipped = 0
    event = 0
    for index, mode in enumerate(modes):
        base = index * 10
        events = (base + 1, base + 2, base + 3, base + 4, base + 5, base + 7)
        event = events[-1]
        if mode == CORRECTION_EQUAL:
            equal += 1
        elif mode == CORRECTION_CORRECTED:
            corrected += 1
        else:
            skipped += 1
        current = payload(1.0 + index * 40)
        recorded = current if mode == CORRECTION_EQUAL else payload(20.0 + index * 40)
        before = snapshot(current)
        immediate = before if mode != CORRECTION_CORRECTED else snapshot(recorded)
        frames.append([
            100 + index,          # tick
            1000 + index,         # monotonic_ns
            300 + index,          # cycle_tid
            mode | AUDIT_EXACT | GATE2_COMPLETE | COMMITTED | PREFIX_CERTIFIED
            | STEERING_APPLIED,
            16667,                # original_delta_us
            16667,                # applied_delta_us
            *events,              # delta, c98, c9c_prefix, f64, close, commit
            1000 + index,         # completion_before
            1001 + index,         # completion_after
            1,                    # callback_flags_at_c9c
            bytes(6),             # reserved
            recorded[:64],        # recorded_transform
            recorded[64:],        # recorded_linear
            before,
            immediate,
        ])
    counters = (
        event,                  # event_count
        len(frames),            # delta_writes
        len(frames) * 2,        # control_writes (steering applied per frame)
        equal,
        corrected,
        skipped,
        0, 0, 0,                # read/ptrace/semantic errors
        corrected,              # write_attempts
        0, 0, 0,                # write_failures/rollbacks
        4,                      # thread_additions
    )
    return frames, counters


def _uer_header(modes: tuple[int, ...], counters: tuple[int, ...]) -> list:
    native = 0x77000000
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
    header = [
        b"A9UER2\0\0",
        2,
        296,
        UER2_FRAME_SIZE,
        0x1F,
        len(modes),
        len(modes),
        bytes.fromhex("e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b"),
        0,
        *addresses,
        *counters,
        10,
        14,
    ]
    return header


def _uer_chain_fields(modes: tuple[int, ...]) -> tuple[list[list], tuple[int, ...]]:
    """Upgrade the A9UER2 fields to A9UER5 shape (v5/v6 layout)."""
    frames, counters = _uer2_frame_fields(modes)
    steering_bits = struct.unpack("<I", struct.pack("<f", 1.0))[0]
    brake_bits = struct.unpack("<I", struct.pack("<f", -1.0))[0]
    upgraded = []
    for index, fields in enumerate(frames):
        base = index * 10
        v3 = fields[:11] + [base + 6] + fields[11:]          # deferred clear
        v4 = v3[:16] + [400 + index, bytes(2)] + v3[17:]     # commit tid
        steer = (steering_bits << 32) | brake_bits
        v5 = v4[:18] + [steering_bits, bytes(4), steer, steer] + v4[18:]
        upgraded.append(v5)
    return upgraded, counters


def make_uer3(modes: tuple[int, ...] = (CORRECTION_EQUAL, CORRECTION_CORRECTED)) -> bytes:
    frames, counters = _uer2_frame_fields(modes)
    header = _uer_header(modes, counters)
    header[0], header[1], header[3] = b"A9UER3\0\0", 3, 1804
    packed = []
    for index, fields in enumerate(frames):
        base = index * 10
        v3 = fields[:11] + [base + 6] + fields[11:]
        packed.append(UER3_FRAME.pack(*v3))
    return UER_HEADER.pack(*header) + b"".join(packed)


def make_uer4(modes: tuple[int, ...] = (CORRECTION_EQUAL, CORRECTION_CORRECTED)) -> bytes:
    frames, counters = _uer2_frame_fields(modes)
    header = _uer_header(modes, counters)
    header[0], header[1], header[3] = b"A9UER4\0\0", 4, 1804
    packed = []
    for index, fields in enumerate(frames):
        base = index * 10
        v3 = fields[:11] + [base + 6] + fields[11:]
        v4 = v3[:16] + [400 + index, bytes(2)] + v3[17:]
        packed.append(UER4_FRAME.pack(*v4))
    return UER_HEADER.pack(*header) + b"".join(packed)


def make_uer5(modes: tuple[int, ...] = (CORRECTION_EQUAL, CORRECTION_CORRECTED)) -> bytes:
    frames, counters = _uer_chain_fields(modes)
    header = _uer_header(modes, counters)
    header[0], header[1], header[3] = b"A9UER5\0\0", 5, 1828
    return UER_HEADER.pack(*header) + b"".join(UER5_FRAME.pack(*f) for f in frames)


def make_uer6(modes: tuple[int, ...] = (CORRECTION_EQUAL, CORRECTION_CORRECTED)) -> bytes:
    frames, counters = _uer_chain_fields(modes)
    header = _uer_header(modes, counters)
    header[0], header[1], header[3] = b"A9UER6\0\0", 6, 1828
    packed = []
    for fields in frames:
        v6 = list(fields)
        v6[3] |= 1 << 8  # BRAKE_APPLIED
        packed.append(UER5_FRAME.pack(*v6))
    return UER_HEADER.pack(*header) + b"".join(packed)


def make_uer7() -> bytes:
    modes = (CORRECTION_EQUAL,)
    frames, counters = _uer_chain_fields(modes)
    header = _uer_header(modes, counters)
    header[0], header[1], header[2], header[3] = b"A9UER7\0\0", 7, 320, 1940
    header[4] |= NITRO_RPC_USED
    state = (0, 0, b"\x01\x01\x01\x01\x01", 0, 0, 0)
    response = _RESPONSE.pack(
        b"A9NRS1\0\0",
        1,
        96,
        123456,           # sequence
        0,                # result
        0,                # calls
        header[10],       # guest_base == library_base
        header[12],       # vehicle_owner == main_object
        0x700000001000,
        header[10] + 0x3674E50,
        *state,
        *state,
    )
    fields = frames[0]
    v7 = fields[:24] + [fields[6], 0, 0, response] + fields[24:]
    v7[3] |= NITRO_RPC_OBSERVED
    new_header = header + [1, 0, 0]
    return UER7_HEADER.pack(*new_header) + UER7_FRAME.pack(*v7)


def make_fc1(pid: int = 1234, base: int = 0x70000000) -> bytes:
    data = bytearray(REPORT_SIZE)
    data[:8] = b"A9FC1R1\0"
    struct.pack_into("<IIII", data, 8, 1, REPORT_SIZE, 0x7FF, 7)
    struct.pack_into("<QQ", data, 24, pid, base)
    context = 0x71000000
    car = 0x72000000
    payload_shadow = 0x73000000
    original_vptr = base + 0x7EE8D18
    shadow_vptr = payload_shadow + 0x58
    struct.pack_into("<QQQ", data, 40, context, context + 0x180,
                     context + 0x1A0)
    struct.pack_into("<QQQ", data, 64, car, original_vptr, shadow_vptr)
    struct.pack_into("<QQQQ", data, 88, payload_shadow, 0x73001000,
                     0x73002000, 0x74000000)
    struct.pack_into("<iII", data, 120, 1240, 20, 20)
    struct.pack_into("<QQ", data, 136, 100, 200)
    struct.pack_into("<QQQQQQQ", data, 152, 1, 0, 0, 0, 0, 0, 0)
    evidence = 208
    data[evidence:evidence + 8] = b"A9FC0E1\0"
    struct.pack_into("<II", data, evidence + 8, 1, 128)
    struct.pack_into("<QQQQQ", data, evidence + 16, 1, 1, 1, 0, 0)
    struct.pack_into("<QQQQ", data, evidence + 56, car, 0x74000000,
                     shadow_vptr, original_vptr)
    struct.pack_into("<i", data, evidence + 96, 0)
    return bytes(data)


def valid_fixture(format_name: str) -> bytes:
    return {
        "A9NPS1": make_nps1(),
        "A9UTK1": make_utk1(),
        "A9USR1": make_usr1(),
        "A9USR2": make_usr2(),
        "A9USR3": make_usr3(),
        "A9USR4": make_usr4(),
        "A9UER1": make_uer1(),
        "A9UER2": make_uer2(),
        "A9UER3": make_uer3(),
        "A9UER4": make_uer4(),
        "A9UER5": make_uer5(),
        "A9UER6": make_uer6(),
        "A9UER7": make_uer7(),
    }[format_name]


class ValidFixtureTests(unittest.TestCase):
    def test_every_advertised_supported_format_has_a_valid_fixture(self) -> None:
        for format_name in SUPPORTED:
            with self.subTest(format=format_name):
                report = inspect_bytes(valid_fixture(format_name))
                self.assertEqual(report["status"], "VALID", report["error"])
                self.assertEqual(report["format"], format_name)
                self.assertTrue(report["read_only"])
                self.assertEqual(report["device_access"], 0)
                self.assertTrue(report["checks"])

    def test_fc1_valid_with_explicit_pid_and_base(self) -> None:
        report = inspect_bytes(make_fc1(1234, 0x70000000),
                               format_name="FC1", pid=1234, base=0x70000000)
        self.assertEqual(report["status"], "VALID")
        self.assertEqual(report["format"], "FC1")
        self.assertEqual(report["size"], REPORT_SIZE)
        self.assertIsNone(report["frames"])

    def test_input_files_are_byte_identical_after_inspection(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            for format_name in SUPPORTED + ("FC1",):
                with self.subTest(format=format_name):
                    blob = make_fc1() if format_name == "FC1" else valid_fixture(format_name)
                    path = write_fixture(directory, f"{format_name}.bin", blob)
                    before = sha256(path.read_bytes())
                    if format_name == "FC1":
                        inspect_bytes(blob, format_name="FC1", pid=1234, base=0x70000000)
                    else:
                        inspect_bytes(blob)
                    after = sha256(path.read_bytes())
                    self.assertEqual(before, after)

    def test_a9usr2_input_cycle_anchor_profile_is_supported(self) -> None:
        anchored = _retag_usr(make_usr2(), b"A9USR2\0\0", 2, 0x27F)
        report = inspect_bytes(anchored)
        self.assertEqual(report["status"], "VALID", report["error"])
        self.assertEqual(report["format"], "A9USR2")

    def test_a9usr2_unregistered_flag_profile_fails_closed(self) -> None:
        unknown = _retag_usr(make_usr2(), b"A9USR2\0\0", 2, 0x17F)
        report = inspect_bytes(unknown)
        self.assertEqual(report["status"], "INVALID")
        self.assertIn("not a registered profile", report["error"])


class NegativeFixtureTests(unittest.TestCase):
    def test_unknown_magic(self) -> None:
        report = inspect_bytes(b"XXXXXXXX" + b"\0" * 64)
        self.assertEqual(report["status"], "UNSUPPORTED")
        self.assertIn("unknown magic", report["error"])

    def test_bad_version(self) -> None:
        blob = bytearray(make_nps1())
        struct.pack_into("<I", blob, 8, 99)
        report = inspect_bytes(bytes(blob))
        self.assertEqual(report["status"], "UNSUPPORTED")
        self.assertIn("version", report["error"])

    def test_truncated(self) -> None:
        blob = make_nps1()[:-1]
        report = inspect_bytes(blob)
        self.assertEqual(report["status"], "INVALID")

    def test_trailing_byte(self) -> None:
        blob = make_nps1() + b"x"
        report = inspect_bytes(blob)
        self.assertEqual(report["status"], "INVALID")
        self.assertIn("length", report["error"])

    def test_frame_count_length_mismatch(self) -> None:
        blob = bytearray(make_nps1())
        struct.pack_into("<I", blob, 16, 999)  # frame count no longer matches size
        report = inspect_bytes(bytes(blob))
        self.assertEqual(report["status"], "INVALID")

    def test_non_contiguous_tick(self) -> None:
        blob = bytearray(make_nps1())
        # second frame prefix starts at HEADER_SIZE (64); tick is the first Q.
        struct.pack_into("<Q", blob, 64, 5)
        report = inspect_bytes(bytes(blob))
        self.assertEqual(report["status"], "INVALID")
        self.assertIn("tick", report["error"])

    def test_nan_payload(self) -> None:
        blob = make_nps1(finite=False)
        report = inspect_bytes(blob)
        self.assertEqual(report["status"], "INVALID")
        self.assertIn("non-finite", report["error"])

    def test_fc1_missing_pid_base(self) -> None:
        report = inspect_bytes(make_fc1(), format_name="FC1")
        self.assertEqual(report["status"], "UNSUPPORTED")
        self.assertIn("--pid", report["error"])

    def test_fc1_wrong_pid_rejected(self) -> None:
        report = inspect_bytes(make_fc1(1234, 0x70000000),
                               format_name="FC1", pid=9999, base=0x70000000)
        self.assertEqual(report["status"], "INVALID")

    def test_known_unsupported_magic_fails_closed(self) -> None:
        report = inspect_bytes(b"A9CDT1\0\0" + b"\0" * 64)
        self.assertEqual(report["status"], "UNSUPPORTED")
        self.assertIn("known but not supported", report["error"])

    def test_renamed_extension_does_not_matter(self) -> None:
        # A file is identified by content magic, not by its extension: the
        # same A9NPS1 bytes under a foreign extension still inspect as A9NPS1.
        report = inspect_bytes(make_nps1())
        self.assertEqual(report["status"], "VALID")
        self.assertEqual(report["format"], "A9NPS1")


class ExitCodeTests(unittest.TestCase):
    def _run(self, argv: list[str]) -> tuple[int, str]:
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(argv)
        return code, buffer.getvalue()

    def test_valid_exit_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = write_fixture(Path(tmp), "n.bin", make_nps1())
            code, out = self._run([str(path)])
        self.assertEqual(code, 0)
        self.assertIn("A9_ARTIFACT_VALID format=A9NPS1", out)
        self.assertIn("read_only=1 device_access=0", out)

    def test_unknown_magic_exit_two(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = write_fixture(Path(tmp), "u.bin", b"XXXXXXXX" + b"\0" * 32)
            code, out = self._run([str(path)])
        self.assertEqual(code, 2)
        self.assertIn("status=UNSUPPORTED", out)

    def test_bad_version_exit_two(self) -> None:
        blob = bytearray(make_nps1())
        struct.pack_into("<I", blob, 8, 7)
        with tempfile.TemporaryDirectory() as tmp:
            path = write_fixture(Path(tmp), "v.bin", bytes(blob))
            code, _ = self._run([str(path)])
        self.assertEqual(code, 2)

    def test_truncated_exit_one(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = write_fixture(Path(tmp), "t.bin", make_nps1()[:-2])
            code, _ = self._run([str(path)])
        self.assertEqual(code, 1)

    def test_trailing_byte_exit_one(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = write_fixture(Path(tmp), "tr.bin", make_nps1() + b"\0")
            code, _ = self._run([str(path)])
        self.assertEqual(code, 1)

    def test_missing_file_exit_three(self) -> None:
        missing = Path(tempfile.gettempdir()) / "does_not_exist_9f3a.bin"
        code, out = self._run([str(missing)])
        self.assertEqual(code, 3)
        self.assertIn("status=IO_ERROR", out)

    def test_fc1_missing_args_exit_two(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = write_fixture(Path(tmp), "fc1.bin", make_fc1())
            code, out = self._run([str(path), "--format", "FC1"])
        self.assertEqual(code, 2)
        self.assertIn("--pid", out)

    def test_list_formats_exit_zero(self) -> None:
        code, out = self._run(["--list-formats"])
        self.assertEqual(code, 0)
        for format_name in ("A9NPS1", "A9UTK1", "A9USR1", "A9USR4",
                            "A9UER1", "A9UER7", "FC1"):
            self.assertIn(format_name, out)
        self.assertIn("read_only=1 device_access=0", out)


class JsonOutputTests(unittest.TestCase):
    def _run(self, argv: list[str]) -> tuple[int, str]:
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(argv)
        return code, buffer.getvalue()

    def test_json_valid_parseable_and_schema_complete(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = write_fixture(Path(tmp), "n.bin", make_nps1())
            code, out = self._run([str(path), "--json"])
        self.assertEqual(code, 0)
        report = json.loads(out)
        for key in ("status", "format", "version", "size", "frames", "first_tick",
                    "last_tick", "checks", "read_only", "device_access", "error"):
            self.assertIn(key, report)
        self.assertEqual(report["status"], "VALID")
        self.assertTrue(report["read_only"])
        self.assertEqual(report["device_access"], 0)

    def test_json_invalid_parseable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = write_fixture(Path(tmp), "u.bin", b"????????")
            code, out = self._run([str(path), "--json"])
        self.assertEqual(code, 2)
        report = json.loads(out)
        self.assertEqual(report["status"], "UNSUPPORTED")


# ---------------------------------------------------------------------------
# N3/B4-R2: explicit A9UTK1 companion cross-binding (A9USR1-A9USR4).
# ---------------------------------------------------------------------------

COMPANION_PAIRS: dict[str, tuple[bytes, bytes]] = {}


def _load_companion_pairs() -> dict[str, tuple[bytes, bytes]]:
    """Build (report, recording) pairs with the authoritative fixture helpers."""
    if COMPANION_PAIRS:
        return COMPANION_PAIRS
    pairs = {
        "A9USR1": make_capture(3),
        "A9USR2": make_brake_source(),
        "A9USR3": make_action_window(),
        "A9USR4": make_action_until_release(),
    }
    COMPANION_PAIRS.update(pairs)
    return pairs


def retag_usr_maximum(report: bytes, maximum: int) -> bytes:
    """Rewrite the A9USR4 bounded-maximum header field."""
    header = list(USR_HEADER.unpack_from(report))
    header[5] = maximum
    return USR_HEADER.pack(*header) + report[USR_HEADER_SIZE:]


def retag_as_usr4(report: bytes) -> bytes:
    """Re-label an A9USR3-shaped report as A9USR4 with a large maximum."""
    header = list(USR_HEADER.unpack_from(report))
    header[0], header[1], header[4], header[5] = b"A9USR4\0\0", 4, 0x1FF, 3600
    return USR_HEADER.pack(*header) + report[USR_HEADER_SIZE:]


def tamper_recording_steering_bits(recording: bytes, frame_index: int) -> bytes:
    """Flip one steering bit so A9UTK1 still parses but the cross-bind breaks."""
    offset = UTK1_HEADER_SIZE + frame_index * UTK1_FRAME_SIZE + 16
    changed = bytearray(recording)
    bits = struct.unpack_from("<I", changed, offset)[0] ^ 0x1
    struct.pack_into("<I", changed, offset, bits)
    return bytes(changed)


def tamper_recording_physics_byte(recording: bytes, frame_index: int) -> bytes:
    """Flip one transform byte so A9UTK1 still parses but physics cross-bind breaks."""
    offset = UTK1_HEADER_SIZE + frame_index * UTK1_FRAME_SIZE + 60
    changed = bytearray(recording)
    changed[offset] ^= 0x01
    return bytes(changed)


def flatten_window_pair(report: bytes, recording: bytes) -> tuple[bytes, bytes]:
    """Force every A9USR3 frame neutral (0,0): no valid action window remains."""
    changed = bytearray(recording)
    for index in range((len(recording) - UTK1_HEADER_SIZE) // UTK1_FRAME_SIZE):
        offset = UTK1_HEADER_SIZE + index * UTK1_FRAME_SIZE
        struct.pack_into("<II", changed, offset + 16, 0, 0)
    audits: list[bytes] = []
    frame_count = (len(report) - USR_HEADER_SIZE) // USR_FRAME_SIZE
    for index in range(frame_count):
        fields = list(USR_FRAME.unpack_from(report, USR_HEADER_SIZE + index * USR_FRAME_SIZE))
        fields[17] = 0
        fields[18] = 0
        audits.append(USR_FRAME.pack(*fields))
    return report[:USR_HEADER_SIZE] + b"".join(audits), bytes(changed)


class P0CompanionEdgeFixTests(unittest.TestCase):
    """Batch-3 P0: companion status truthfulness, read ordering, list-formats mutex."""

    def test_wrong_version_usr_with_companion_is_not_evaluated(self) -> None:
        _, recording = make_capture(3)
        for format_name in ("A9USR1", "A9USR2", "A9USR3", "A9USR4"):
            with self.subTest(format=format_name):
                blob = bytearray(valid_fixture(format_name))
                struct.pack_into("<I", blob, 8, 99)
                result = inspect_bytes(bytes(blob), companion_blob=recording)
                self.assertEqual(result["status"], "UNSUPPORTED")
                self.assertEqual(result["validation_scope"], "STRUCTURE_ONLY")
                self.assertEqual(result["companion_status"], "NOT_EVALUATED")
                self.assertEqual(result["companion_format"], "A9UTK1")
                self.assertFalse(result["capture_validated"])

    def test_truncated_usr_with_missing_companion_reports_main_invalid(self) -> None:
        report, _ = make_capture(3)
        with tempfile.TemporaryDirectory() as tmp:
            main_path = write_fixture(Path(tmp), "r.a9usr1", report[:-5])
            code, out = self._run(
                [str(main_path), "--companion", str(Path(tmp) / "nope.a9utk1")]
            )
        self.assertEqual(code, 1)
        self.assertIn("status=INVALID", out)
        self.assertIn("main report", out)
        self.assertNotIn("cannot read companion", out)
        self.assertIn("companion_status=NOT_EVALUATED", out)

    def test_non_usr_with_missing_companion_is_usage_error_not_io(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            nps = write_fixture(directory, "n.a9nps1", make_nps1())
            unknown = write_fixture(directory, "u.bin", b"????????")
            missing = str(directory / "nope.a9utk1")
            code_nps, out_nps = self._run([str(nps), "--companion", missing])
            code_unknown, out_unknown = self._run([str(unknown), "--companion", missing])
        self.assertEqual(code_nps, 3)
        self.assertIn("status=USAGE_ERROR", out_nps)
        self.assertNotIn("cannot read companion", out_nps)
        self.assertEqual(code_unknown, 3)
        self.assertIn("status=USAGE_ERROR", out_unknown)
        self.assertNotIn("cannot read companion", out_unknown)

    def test_valid_usr_with_missing_companion_keeps_main_fields(self) -> None:
        report, _ = make_capture(3)
        with tempfile.TemporaryDirectory() as tmp:
            main_path = write_fixture(Path(tmp), "r.a9usr1", report)
            code, out = self._run(
                [str(main_path), "--companion", str(Path(tmp) / "nope.a9utk1"), "--json"]
            )
        self.assertEqual(code, 3)
        parsed = json.loads(out)
        self.assertEqual(parsed["status"], "IO_ERROR")
        self.assertEqual(parsed["companion_status"], "IO_ERROR")
        self.assertFalse(parsed["capture_validated"])
        self.assertEqual(parsed["format"], "A9USR1")
        self.assertEqual(parsed["version"], 1)
        self.assertEqual(parsed["frames"], 3)
        self.assertIsNotNone(parsed["first_tick"])
        self.assertIsNotNone(parsed["last_tick"])

    def test_list_formats_is_mutually_exclusive(self) -> None:
        report, recording = make_capture(3)
        with tempfile.TemporaryDirectory() as tmp:
            main_path = str(write_fixture(Path(tmp), "r.a9usr1", report))
            comp_path = str(write_fixture(Path(tmp), "c.a9utk1", recording))
            combos = [
                ["--list-formats", main_path],
                ["--list-formats", "--format", "FC1"],
                ["--list-formats", "--pid", "1234"],
                ["--list-formats", "--base", "0x70000000"],
                ["--list-formats", "--companion", comp_path],
            ]
            for argv in combos:
                with self.subTest(argv=argv):
                    buffer = io.StringIO()
                    err = io.StringIO()
                    with self.assertRaises(SystemExit) as ctx, redirect_stdout(buffer), redirect_stderr(err):
                        main(argv)
                    self.assertEqual(ctx.exception.code, 3)
                    self.assertIn("list-formats", err.getvalue().lower())

    def test_p0_paths_keep_inputs_byte_identical(self) -> None:
        report, recording = make_capture(3)
        bad_version = bytearray(report)
        struct.pack_into("<I", bad_version, 8, 99)
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            main_path = write_fixture(directory, "r.a9usr1", report)
            comp_path = write_fixture(directory, "c.a9utk1", recording)
            before_main = sha256(main_path.read_bytes())
            before_comp = sha256(comp_path.read_bytes())
            # wrong-version main + companion (NOT_EVALUATED path)
            bad_path = write_fixture(directory, "bad.a9usr1", bytes(bad_version))
            self._run([str(bad_path), "--companion", str(comp_path)])
            self.assertEqual(sha256(bad_path.read_bytes()), sha256(bytes(bad_version)))
            self.assertEqual(sha256(comp_path.read_bytes()), before_comp)
            self.assertEqual(sha256(main_path.read_bytes()), before_main)

    def test_p0_error_paths_have_no_uncaught_tracebacks(self) -> None:
        report, recording = make_capture(3)
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            main_path = write_fixture(directory, "r.a9usr1", report)
            comp_path = write_fixture(directory, "c.a9utk1", recording)
            argv_cases = [
                [str(main_path), "--companion", str(directory / "missing.a9utk1")],
                [str(main_path), "--companion", str(comp_path)],
                [str(main_path), "--companion", str(comp_path), "--json"],
            ]
            for argv in argv_cases:
                with self.subTest(argv=argv):
                    buffer = io.StringIO()
                    err = io.StringIO()
                    try:
                        with redirect_stdout(buffer), redirect_stderr(err):
                            main(argv)
                    except SystemExit:
                        pass

    def _run(self, argv: list[str]) -> tuple[int, str]:
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(argv)
        return code, buffer.getvalue()


class CompanionValidPairTests(unittest.TestCase):
    """Correct report + A9UTK1 companion pairs validate fully."""

    def test_four_pairs_cross_bound(self) -> None:
        for format_name, (report, recording) in _load_companion_pairs().items():
            with self.subTest(format=format_name):
                result = inspect_bytes(report, companion_blob=recording)
                self.assertEqual(result["status"], "VALID", result["error"])
                self.assertEqual(result["validation_scope"], "CAPTURE_CROSS_BOUND")
                self.assertEqual(result["companion_status"], "VALID")
                self.assertEqual(result["companion_format"], "A9UTK1")
                self.assertTrue(result["capture_validated"])
                self.assertIn("companion_cross_bind", result["checks"])

    def test_pairs_without_companion_are_structure_only(self) -> None:
        for format_name, (report, _recording) in _load_companion_pairs().items():
            with self.subTest(format=format_name):
                result = inspect_bytes(report)
                self.assertEqual(result["status"], "VALID", result["error"])
                self.assertEqual(result["validation_scope"], "STRUCTURE_ONLY")
                self.assertEqual(result["companion_status"], "NOT_PROVIDED")
                self.assertFalse(result["capture_validated"])
                self.assertNotIn("companion_cross_bind", result["checks"])

    def test_a9usr2_input_cycle_anchor_pair_cross_bound(self) -> None:
        report, recording = _load_companion_pairs()["A9USR2"]
        anchored = _retag_usr(report, b"A9USR2\0\0", 2, 0x27F)
        result = inspect_bytes(anchored, companion_blob=recording)
        self.assertEqual(result["status"], "VALID", result["error"])
        self.assertEqual(result["validation_scope"], "CAPTURE_CROSS_BOUND")
        self.assertTrue(result["capture_validated"])

    def test_cli_success_text_and_json(self) -> None:
        report, recording = _load_companion_pairs()["A9USR1"]
        with tempfile.TemporaryDirectory() as tmp:
            main_path = write_fixture(Path(tmp), "r.a9usr1", report)
            comp_path = write_fixture(Path(tmp), "c.a9utk1", recording)
            code, out = self._run([str(main_path), "--companion", str(comp_path)])
            code_json, out_json = self._run(
                [str(main_path), "--companion", str(comp_path), "--json"]
            )
        self.assertEqual(code, 0)
        self.assertIn("validation_scope=CAPTURE_CROSS_BOUND", out)
        self.assertIn("companion_status=VALID", out)
        self.assertIn("capture_validated=true", out)
        self.assertEqual(code_json, 0)
        parsed = json.loads(out_json)
        self.assertEqual(parsed["validation_scope"], "CAPTURE_CROSS_BOUND")
        self.assertEqual(parsed["companion_status"], "VALID")
        self.assertTrue(parsed["capture_validated"])

    def _run(self, argv: list[str]) -> tuple[int, str]:
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(argv)
        return code, buffer.getvalue()


class CompanionFailureTests(unittest.TestCase):
    """Companion mismatch / corrupt companion / usage errors."""

    def test_frame_count_mismatch_exit_one(self) -> None:
        report = make_capture(3)[0]
        recording = make_capture(2)[1]
        result = inspect_bytes(report, companion_blob=recording)
        self.assertEqual(result["status"], "INVALID")
        self.assertEqual(result["validation_scope"], "CAPTURE_CROSS_BOUND")
        self.assertEqual(result["companion_status"], "INVALID")
        self.assertIn("frame-count", result["error"])

    def test_tampered_steering_bits_exit_one(self) -> None:
        report, recording = make_capture(3)
        result = inspect_bytes(report, companion_blob=tamper_recording_steering_bits(recording, 1))
        self.assertEqual(result["status"], "INVALID")
        self.assertIn("steering", result["error"])

    def test_tampered_physics_byte_exit_one(self) -> None:
        report, recording = make_capture(3)
        result = inspect_bytes(report, companion_blob=tamper_recording_physics_byte(recording, 1))
        self.assertEqual(result["status"], "INVALID")
        self.assertIn("physics", result["error"])

    def test_usr3_missing_action_window_exit_one(self) -> None:
        report, recording = make_action_window()
        flat_report, flat_recording = flatten_window_pair(report, recording)
        result = inspect_bytes(flat_report, companion_blob=flat_recording)
        self.assertEqual(result["status"], "INVALID")
        self.assertEqual(result["validation_scope"], "CAPTURE_CROSS_BOUND")
        self.assertIn("steering+brake", result["error"])

    def test_usr4_wrong_post_release_length_exit_one(self) -> None:
        long_report, long_recording = make_action_window(180)
        result = inspect_bytes(
            retag_as_usr4(long_report), companion_blob=long_recording
        )
        self.assertEqual(result["status"], "INVALID")
        self.assertIn("exactly 30", result["error"])

    def test_usr4_maximum_mismatch_exit_one(self) -> None:
        report, recording = make_action_until_release()
        result = inspect_bytes(
            retag_usr_maximum(report, 59), companion_blob=recording
        )
        self.assertEqual(result["status"], "INVALID")
        self.assertIn("bounded maximum", result["error"])

    def test_corrupt_companion_exit_one(self) -> None:
        report, recording = make_capture(3)
        corruptions = {
            "wrong magic": b"A9UTK2\0\0" + recording[8:],
            "wrong version": None,
            "truncated": recording[:-1],
            "trailing byte": recording + b"\0",
        }
        for name, blob in corruptions.items():
            with self.subTest(case=name):
                if name == "wrong version":
                    changed = bytearray(recording)
                    struct.pack_into("<I", changed, 8, 2)
                    blob = bytes(changed)
                result = inspect_bytes(report, companion_blob=blob)
                self.assertEqual(result["status"], "INVALID", result["error"])
                self.assertEqual(result["validation_scope"], "CAPTURE_CROSS_BOUND")
                self.assertEqual(result["companion_status"], "INVALID")

    def test_missing_companion_file_exit_three(self) -> None:
        report, _ = make_capture(3)
        with tempfile.TemporaryDirectory() as tmp:
            main_path = write_fixture(Path(tmp), "r.a9usr1", report)
            code, out = self._run([str(main_path), "--companion", str(Path(tmp) / "nope.a9utk1")])
        self.assertEqual(code, 3)
        self.assertIn("status=IO_ERROR", out)
        self.assertIn("cannot read companion", out)

    def test_non_usr_main_with_companion_exit_three(self) -> None:
        _, recording = make_capture(3)
        with tempfile.TemporaryDirectory() as tmp:
            main_path = write_fixture(Path(tmp), "n.a9nps1", make_nps1())
            comp_path = write_fixture(Path(tmp), "c.a9utk1", recording)
            code, out = self._run([str(main_path), "--companion", str(comp_path)])
            code_unknown, out_unknown = self._run(
                [str(write_fixture(Path(tmp), "u.bin", b"????????")),
                 "--companion", str(comp_path)]
            )
        self.assertEqual(code, 3)
        self.assertIn("status=USAGE_ERROR", out)
        self.assertIn("A9USR1-A9USR4", out)
        self.assertEqual(code_unknown, 3)
        self.assertIn("status=USAGE_ERROR", out_unknown)

    def test_fc1_with_companion_exit_three(self) -> None:
        _, recording = make_capture(3)
        with tempfile.TemporaryDirectory() as tmp:
            main_path = write_fixture(Path(tmp), "f.fc1", make_fc1())
            comp_path = write_fixture(Path(tmp), "c.a9utk1", recording)
            buffer = io.StringIO()
            err = io.StringIO()
            with self.assertRaises(SystemExit) as ctx, redirect_stdout(buffer), redirect_stderr(err):
                main([str(main_path), "--format", "FC1", "--pid", "1234",
                      "--base", "0x70000000", "--companion", str(comp_path)])
        self.assertEqual(ctx.exception.code, 3)
        self.assertIn("cannot be combined with --format FC1", err.getvalue())

    def _run(self, argv: list[str]) -> tuple[int, str]:
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(argv)
        return code, buffer.getvalue()


class CompanionOutputAndSafetyTests(unittest.TestCase):
    """Output fields, no auto-discovery, byte-identity and no tracebacks."""

    def test_json_and_text_always_carry_scope_fields(self) -> None:
        report, recording = make_capture(3)
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            cases = [
                ("json valid", [str(write_fixture(directory, "j.a9usr1", report)),
                                "--companion",
                                str(write_fixture(directory, "j.a9utk1", recording)),
                                "--json"], 0),
                ("text valid", [str(write_fixture(directory, "t.a9usr1", report)),
                                "--companion",
                                str(write_fixture(directory, "t.a9utk1", recording))], 0),
            ]
            for name, argv, expected in cases:
                with self.subTest(case=name):
                    code, out = self._run(argv)
                    self.assertEqual(code, expected)
                    for field in ("validation_scope", "companion_status",
                                  "companion_format", "capture_validated"):
                        self.assertIn(field, out)

    def test_json_invalid_carries_scope_fields(self) -> None:
        report, _ = make_capture(3)
        bad = tamper_recording_steering_bits(make_capture(3)[1], 1)
        with tempfile.TemporaryDirectory() as tmp:
            main_path = write_fixture(Path(tmp), "r.a9usr1", report)
            comp_path = write_fixture(Path(tmp), "c.a9utk1", bad)
            code, out = self._run([str(main_path), "--companion", str(comp_path), "--json"])
        self.assertEqual(code, 1)
        parsed = json.loads(out)
        for field in ("validation_scope", "companion_status",
                      "companion_format", "capture_validated"):
            self.assertIn(field, parsed)
        self.assertEqual(parsed["status"], "INVALID")
        self.assertEqual(parsed["validation_scope"], "CAPTURE_CROSS_BOUND")
        self.assertFalse(parsed["capture_validated"])

    def test_no_auto_discovery_of_adjacent_companion(self) -> None:
        report, recording = make_capture(3)
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            main_path = write_fixture(directory, "capture.a9usr1", report)
            write_fixture(directory, "capture.a9utk1", recording)
            code, out = self._run([str(main_path)])
            comp_after = sha256((directory / "capture.a9utk1").read_bytes())
        self.assertEqual(code, 0)
        self.assertIn("validation_scope=STRUCTURE_ONLY", out)
        self.assertIn("companion_status=NOT_PROVIDED", out)
        self.assertIn("capture_validated=false", out)
        self.assertEqual(comp_after, sha256(recording))

    def test_inputs_byte_identical_on_success_and_failure(self) -> None:
        report, recording = make_capture(3)
        tampered = tamper_recording_steering_bits(recording, 1)
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            main_path = write_fixture(directory, "r.a9usr1", report)
            ok_comp = write_fixture(directory, "ok.a9utk1", recording)
            bad_comp = write_fixture(directory, "bad.a9utk1", tampered)
            before_main = sha256(main_path.read_bytes())
            before_ok = sha256(ok_comp.read_bytes())
            before_bad = sha256(bad_comp.read_bytes())
            code_ok, _ = self._run([str(main_path), "--companion", str(ok_comp)])
            code_bad, _ = self._run([str(main_path), "--companion", str(bad_comp)])
            self.assertEqual(code_ok, 0)
            self.assertEqual(code_bad, 1)
            self.assertEqual(sha256(main_path.read_bytes()), before_main)
            self.assertEqual(sha256(ok_comp.read_bytes()), before_ok)
            self.assertEqual(sha256(bad_comp.read_bytes()), before_bad)

    def test_no_uncaught_tracebacks_on_error_paths(self) -> None:
        report, recording = make_capture(3)
        cases: list[list[str]] = []
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            main_path = write_fixture(directory, "r.a9usr1", report)
            ok_comp = write_fixture(directory, "ok.a9utk1", recording)
            bad_comp = write_fixture(directory, "bad.a9utk1",
                                     tamper_recording_steering_bits(recording, 1))
            wrong_comp = write_fixture(directory, "wrong.bin", make_nps1())
            cases = [
                [str(main_path), "--companion", str(Path(tmp) / "missing.a9utk1")],
                [str(main_path), "--companion", str(bad_comp)],
                [str(main_path), "--companion", str(wrong_comp)],
                [str(main_path), "--companion", str(ok_comp), "--json"],
            ]
            for argv in cases:
                with self.subTest(argv=argv):
                    buffer = io.StringIO()
                    err = io.StringIO()
                    try:
                        with redirect_stdout(buffer), redirect_stderr(err):
                            main(argv)
                    except SystemExit:
                        pass  # argparse usage errors exit via SystemExit

    def test_list_formats_still_28(self) -> None:
        code, out = self._run(["--list-formats"])
        self.assertEqual(code, 0)
        entries = [
            line for line in out.splitlines()
            if line and not line.startswith("A9_ARTIFACT_FORMATS")
            and "read_only=" not in line
        ]
        self.assertEqual(len(entries), 28)

    def test_list_formats_json_schema(self) -> None:
        code, out = self._run(["--list-formats", "--json"])
        self.assertEqual(code, 0)
        parsed = json.loads(out)
        self.assertEqual(parsed["schema"], "A9_FORMAT_LIST_V1")
        self.assertEqual(parsed["summary"]["format_count"], 28)
        self.assertEqual(len(parsed["formats"]), 28)
        self.assertTrue(parsed["read_only"])
        self.assertEqual(parsed["device_access"], 0)
        # Order matches ALL_FORMATS exactly.
        self.assertEqual(
            [item["name"] for item in parsed["formats"]],
            [spec.name for spec in inspector.ALL_FORMATS],
        )
        by_name = {item["name"]: item for item in parsed["formats"]}
        nps1 = by_name["A9NPS1"]
        self.assertEqual(nps1["magic_ascii"], "A9NPS1\\0\\0")
        self.assertEqual(nps1["magic_hex"], "41394e5053310000")
        self.assertEqual(nps1["version"], 1)
        # Only A9USR1-4 carry companion fields.
        self.assertEqual(by_name["A9USR1"]["companion_format"], "A9UTK1")
        self.assertTrue(by_name["A9USR4"]["companion_authoritative"])
        self.assertIsNone(by_name["A9NPS1"]["companion_format"])
        # FC1 has no magic and needs pid/base.
        self.assertIsNone(by_name["FC1"]["magic_ascii"])
        self.assertIsNone(by_name["FC1"]["magic_hex"])
        self.assertTrue(by_name["FC1"]["needs_pid_base"])
        # version=None becomes JSON null.
        self.assertIsNone(by_name["A9PST1"]["version"])
        # KNOWN_UNSUPPORTED formats execute no checks: empty check lists.
        for name in ("A9CDT1", "A9NPA1", "A9NRS1"):
            with self.subTest(name=name):
                self.assertEqual(by_name[name]["checks"], [])

    def _run(self, argv: list[str]) -> tuple[int, str]:
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(argv)
        return code, buffer.getvalue()


if __name__ == "__main__":
    unittest.main()
