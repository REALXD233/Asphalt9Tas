#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("parse_successor_classifier_v1.py")
SPEC = importlib.util.spec_from_file_location("successor_parser", MODULE_PATH)
assert SPEC and SPEC.loader
parser = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = parser
SPEC.loader.exec_module(parser)


BASE = 0x7000000000
PID = 4242
OWNER = 0x7100000000
PHYSICS = 0x7200000000
NATIVE_BODY = 0x7300000000
NATIVE_ANGULAR = NATIVE_BODY + 0x160
RBX_WATCH = 0x7400001968
RBX_OWNER = RBX_WATCH - 0x1968


def make_event(
    sequence: int, flags: int, *, tid: int = 77,
    markers: tuple[tuple[int, int], ...] = (),
    stack_nonce: int | None = None,
) -> bytes:
    nonce = sequence if stack_nonce is None else stack_nonce
    stack = [0x1111000000000000 + nonce * 0x100 + index
             for index in range(parser.STACK_WORDS)]
    for index, address in markers:
        stack[index] = address
    registers = [
        flags,  # DR6 lower status bits
        0x50000000 + sequence,  # host RIP
        0x60000000 + sequence * 0x1000,  # RSP
        0x61000000 + sequence * 0x1000,  # RBP
        0x202,
        0xFFFFFFFFFFFFFFFF,
        *range(0x100 + sequence * 0x10, 0x10E + sequence * 0x10),
    ]
    assert len(registers) == 20
    integers = [
        0x3F800000, 0x40000000,
        0x40400000, 0x40800000, 0x40A00000, 0x40C00000,
        0x3F000000, 0x3F400000,
        1, 1, 1, 0,
    ]
    return parser.EVENT_STRUCT.pack(
        sequence, 1_000_000 + sequence, tid, flags,
        *registers, *integers, *stack,
    )


def make_trace(events: list[bytes], slot_hits: tuple[int, int, int, int],
               *, flags: int = parser.REQUIRED_HEADER_FLAGS,
               errors: tuple[int, int, int, int, int] = (0, 0, 0, 0, 0)) -> bytes:
    header = parser.HEADER_STRUCT.pack(
        parser.MAGIC,
        parser.VERSION,
        parser.HEADER_STRUCT.size,
        parser.EVENT_STRUCT.size,
        flags,
        parser.STACK_WORDS,
        0,
        PID,
        BASE,
        OWNER,
        RBX_OWNER,
        PHYSICS,
        NATIVE_BODY,
        NATIVE_ANGULAR,
        RBX_WATCH,
        NATIVE_ANGULAR + 0x0C,
        OWNER + 0x0C9C,
        PHYSICS + 0x0F64,
        BASE + parser.MARKER_RVAS["rbx_caller"],
        BASE + parser.MARKER_RVAS["yaw_candidate"],
        BASE + parser.MARKER_RVAS["rbx_internal_angular"],
        999_000,
        len(events),
        *slot_hits,
        errors[0], errors[1], errors[2], errors[3],
        0,  # thread additions
        errors[4],
        2, 2,
    )
    return header + b"".join(events)


class SuccessorClassifierParserTests(unittest.TestCase):
    def write_trace(self, data: bytes) -> Path:
        temporary = tempfile.NamedTemporaryFile(suffix=".a9scv1", delete=False)
        temporary.write(data)
        temporary.close()
        self.addCleanup(Path(temporary.name).unlink, missing_ok=True)
        return Path(temporary.name)

    def test_exact_indices_fingerprints_and_dual_phase(self) -> None:
        rbx = BASE + parser.MARKER_RVAS["rbx_caller"]
        yaw = BASE + parser.MARKER_RVAS["yaw_candidate"]
        successor = BASE + parser.MARKER_RVAS["rbx_internal_angular"]
        events = [
            make_event(0, parser.HIT_C9C),
            make_event(1, parser.HIT_RBX, markers=((7, rbx),)),
            make_event(2, parser.HIT_ANGULAR_AUX,
                       markers=((12, yaw), (50, successor))),
            make_event(3, parser.HIT_F64),
        ]
        analysis = parser.analyze(self.write_trace(
            make_trace(events, (1, 1, 1, 1))))
        parser.require_gate(
            analysis, require_window=True, require_dual_candidates=True,
            require_all_markers=True,
        )
        indices = {(hit.marker, hit.stack_index) for hit in analysis.marker_hits}
        self.assertEqual(indices, {
            ("rbx_caller", 7), ("yaw_candidate", 12),
            ("rbx_internal_angular", 50),
        })
        self.assertTrue(all(len(hit.neighbor_fingerprint_sha256) == 64
                            for hit in analysis.marker_hits))
        self.assertEqual(len(analysis.windows), 1)
        self.assertEqual(analysis.windows[0].candidate_sequences, (1, 2))

    def test_cross_tid_candidate_does_not_satisfy_window(self) -> None:
        rbx = BASE + parser.MARKER_RVAS["rbx_caller"]
        events = [
            make_event(0, parser.HIT_C9C, tid=11),
            make_event(1, parser.HIT_RBX, tid=12, markers=((8, rbx),)),
            make_event(2, parser.HIT_F64, tid=11),
        ]
        analysis = parser.analyze(self.write_trace(
            make_trace(events, (1, 0, 1, 1))))
        with self.assertRaisesRegex(parser.TraceError, "same-tid"):
            parser.require_gate(
                analysis, require_window=True, require_dual_candidates=False,
                require_all_markers=False,
            )

    def test_rbx_internal_setter_is_negative_not_yaw(self) -> None:
        negative = BASE + parser.MARKER_RVAS["rbx_internal_angular"]
        events = [
            make_event(0, parser.HIT_C9C),
            make_event(1, parser.HIT_ANGULAR_AUX,
                       markers=((9, negative),)),
            make_event(2, parser.HIT_F64),
        ]
        analysis = parser.analyze(self.write_trace(
            make_trace(events, (0, 1, 1, 1))))
        self.assertEqual(len(analysis.windows), 1)
        self.assertFalse(analysis.windows[0].has_angular)
        self.assertFalse(analysis.windows[0].has_positive_candidate)
        with self.assertRaisesRegex(parser.TraceError, "positive successor"):
            parser.require_gate(
                analysis, require_window=True, require_dual_candidates=False,
                require_all_markers=False,
            )
        with self.assertRaisesRegex(parser.TraceError, "positive successor"):
            parser.require_gate(
                analysis, require_window=True, require_dual_candidates=True,
                require_all_markers=False,
            )

    def test_all_markers_may_be_proved_in_separate_phase_windows(self) -> None:
        rbx = BASE + parser.MARKER_RVAS["rbx_caller"]
        yaw = BASE + parser.MARKER_RVAS["yaw_candidate"]
        negative = BASE + parser.MARKER_RVAS["rbx_internal_angular"]
        events = [
            make_event(0, parser.HIT_C9C),
            make_event(1, parser.HIT_RBX, markers=((7, rbx),)),
            make_event(2, parser.HIT_F64),
            make_event(3, parser.HIT_C9C),
            make_event(4, parser.HIT_ANGULAR_AUX,
                       markers=((19, negative),)),
            make_event(5, parser.HIT_F64),
            make_event(6, parser.HIT_C9C),
            make_event(7, parser.HIT_ANGULAR_AUX, markers=((12, yaw),)),
            make_event(8, parser.HIT_F64),
        ]
        analysis = parser.analyze(self.write_trace(
            make_trace(events, (1, 2, 3, 3))))
        parser.require_gate(
            analysis, require_window=True, require_dual_candidates=False,
            require_all_markers=True,
        )

    def test_all_marker_gate_rejects_unstable_stack_index(self) -> None:
        rbx = BASE + parser.MARKER_RVAS["rbx_caller"]
        yaw = BASE + parser.MARKER_RVAS["yaw_candidate"]
        negative = BASE + parser.MARKER_RVAS["rbx_internal_angular"]
        events = [
            make_event(0, parser.HIT_C9C),
            make_event(1, parser.HIT_RBX, markers=((7, rbx),)),
            make_event(2, parser.HIT_ANGULAR_AUX,
                       markers=((12, yaw), (19, negative))),
            make_event(3, parser.HIT_F64),
            make_event(4, parser.HIT_C9C),
            make_event(5, parser.HIT_RBX, markers=((8, rbx),)),
            make_event(6, parser.HIT_F64),
        ]
        analysis = parser.analyze(self.write_trace(
            make_trace(events, (2, 1, 2, 2))))
        with self.assertRaisesRegex(parser.TraceError, "stable stack index"):
            parser.require_gate(
                analysis, require_window=True, require_dual_candidates=False,
                require_all_markers=True,
            )

    def test_stable_classifier_requires_repetition_and_neighbor_fingerprint(self) -> None:
        rbx = BASE + parser.MARKER_RVAS["rbx_caller"]
        yaw = BASE + parser.MARKER_RVAS["yaw_candidate"]
        negative = BASE + parser.MARKER_RVAS["rbx_internal_angular"]
        events = []
        for window in range(2):
            sequence = window * 4
            events.extend([
                make_event(sequence, parser.HIT_C9C),
                make_event(sequence + 1, parser.HIT_RBX,
                           markers=((7, rbx),), stack_nonce=10),
                make_event(sequence + 2, parser.HIT_ANGULAR_AUX,
                           markers=((12, yaw), (19, negative)),
                           stack_nonce=20),
                make_event(sequence + 3, parser.HIT_F64),
            ])
        analysis = parser.analyze(self.write_trace(
            make_trace(events, (2, 2, 2, 2))))
        parser.require_gate(
            analysis, require_window=True, require_dual_candidates=True,
            require_all_markers=True, require_stable_classifier=True,
        )

        unstable = list(events)
        unstable[5] = make_event(5, parser.HIT_RBX,
                                 markers=((7, rbx),), stack_nonce=11)
        analysis = parser.analyze(self.write_trace(
            make_trace(unstable, (2, 2, 2, 2))))
        with self.assertRaisesRegex(parser.TraceError, "neighbor fingerprint"):
            parser.require_gate(
                analysis, require_window=True, require_dual_candidates=True,
                require_all_markers=True, require_stable_classifier=True,
            )

    def test_stable_classifier_rejects_single_observation(self) -> None:
        rbx = BASE + parser.MARKER_RVAS["rbx_caller"]
        yaw = BASE + parser.MARKER_RVAS["yaw_candidate"]
        negative = BASE + parser.MARKER_RVAS["rbx_internal_angular"]
        events = [
            make_event(0, parser.HIT_C9C),
            make_event(1, parser.HIT_RBX, markers=((7, rbx),)),
            make_event(2, parser.HIT_ANGULAR_AUX,
                       markers=((12, yaw), (19, negative))),
            make_event(3, parser.HIT_F64),
        ]
        analysis = parser.analyze(self.write_trace(
            make_trace(events, (1, 1, 1, 1))))
        with self.assertRaisesRegex(parser.TraceError, "at least two"):
            parser.require_gate(
                analysis, require_window=True, require_dual_candidates=True,
                require_all_markers=True, require_stable_classifier=True,
            )

    def test_candidate_outside_phase_does_not_satisfy_window(self) -> None:
        yaw = BASE + parser.MARKER_RVAS["yaw_candidate"]
        events = [
            make_event(0, parser.HIT_ANGULAR_AUX, markers=((15, yaw),)),
            make_event(1, parser.HIT_C9C),
            make_event(2, parser.HIT_F64),
        ]
        analysis = parser.analyze(self.write_trace(
            make_trace(events, (0, 1, 1, 1))))
        with self.assertRaises(parser.TraceError):
            parser.require_gate(
                analysis, require_window=True, require_dual_candidates=False,
                require_all_markers=False,
            )

    def test_duplicate_marker_is_rejected_as_ambiguous(self) -> None:
        rbx = BASE + parser.MARKER_RVAS["rbx_caller"]
        events = [make_event(0, parser.HIT_RBX,
                             markers=((3, rbx), (17, rbx)))]
        path = self.write_trace(make_trace(events, (1, 0, 0, 0)))
        with self.assertRaisesRegex(parser.TraceError, "duplicate"):
            parser.analyze(path)

    def test_non_clean_header_is_rejected(self) -> None:
        events = [make_event(0, parser.HIT_C9C)]
        path = self.write_trace(make_trace(
            events, (0, 0, 1, 0),
            flags=parser.FLAG_TARGET_VERIFIED | parser.FLAG_READ_ONLY,
        ))
        with self.assertRaisesRegex(parser.TraceError, "not clean"):
            parser.analyze(path)

    def test_capture_error_counter_is_rejected(self) -> None:
        events = [make_event(0, parser.HIT_C9C)]
        path = self.write_trace(make_trace(
            events, (0, 0, 1, 0), errors=(0, 1, 0, 0, 0),
        ))
        with self.assertRaisesRegex(parser.TraceError, "error counters"):
            parser.analyze(path)

    def test_new_c9c_before_f64_is_rejected(self) -> None:
        events = [
            make_event(0, parser.HIT_C9C),
            make_event(1, parser.HIT_C9C),
            make_event(2, parser.HIT_F64),
        ]
        path = self.write_trace(make_trace(events, (0, 0, 2, 1)))
        with self.assertRaisesRegex(parser.TraceError, "before F64"):
            parser.analyze(path)


if __name__ == "__main__":
    unittest.main()
