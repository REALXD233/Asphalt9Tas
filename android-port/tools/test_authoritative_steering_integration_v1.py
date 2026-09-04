#!/usr/bin/env python3
"""End-to-end offline model: authoritative selection to dual steering pairs."""

from __future__ import annotations

import pathlib
import struct
import unittest

from authoritative_tick_state_machine_v1 import SelectionOutcome
from test_authoritative_unified_adapter_v1 import AdapterModel
from unified_tick_executor_semantics_v1 import Event, State
from unified_tick_recording_v1 import decode_recording


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
RECORDING = (
    WORKSPACE
    / "android-port"
    / "evidence"
    / "a9tas_authoritative_steering_projection_344f_20260820.a9utk1"
)


def raw_f32(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def apply_steering(steering: float, pair_before: int) -> int:
    return (raw_f32(steering) << 32) | (pair_before & 0xFFFFFFFF)


def finish_selected_tick(model: AdapterModel, tick: int) -> None:
    model.step(Event.C98)
    model.step(Event.C9C)
    model.step(Event.PREFIX_CERTIFIED)
    model.step(Event.F64)
    model.step(Event.CALLBACK_CLOSE, payload_equal=False)
    model.step(Event.DEFERRED_CALLBACK_CLEAR)
    model.step(Event.WORLD_COMMIT, consumed_tick=tick)


class AuthoritativeSteeringIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fixed_us, cls.frames = decode_recording(RECORDING.read_bytes())

    def test_all_344_selected_packets_feed_both_pairs(self) -> None:
        model = AdapterModel.create(len(self.frames), len(self.frames) - 1)
        writes = 0
        nonzero = 0
        for tick, frame in enumerate(self.frames):
            self.assertEqual(model.step(Event.DT_NONZERO), SelectionOutcome.EXACT_PACKET)
            self.assertEqual(model.selected_tick, tick)
            c98_before = (0xAAAAAAAA << 32) | ((tick * 17) & 0xFFFFFFFF)
            c9c_before = (0x55555555 << 32) | ((tick * 31 + 7) & 0xFFFFFFFF)
            c98_after = apply_steering(frame.steering, c98_before)
            c9c_after = apply_steering(frame.steering, c9c_before)
            self.assertEqual(c98_after >> 32, raw_f32(frame.steering))
            self.assertEqual(c9c_after >> 32, raw_f32(frame.steering))
            self.assertEqual(c98_after & 0xFFFFFFFF, c98_before & 0xFFFFFFFF)
            self.assertEqual(c9c_after & 0xFFFFFFFF, c9c_before & 0xFFFFFFFF)
            writes += 2
            nonzero += raw_f32(frame.steering) != 0
            finish_selected_tick(model, tick)
        self.assertEqual((writes, nonzero), (688, 94))
        self.assertEqual((model.machine.state, model.next_tick), (State.COMPLETE, 344))
        self.assertFalse(model.control.block_mode_active)

    def test_pause_rollback_reselects_identical_real_packet(self) -> None:
        model = AdapterModel.create(len(self.frames), len(self.frames) - 1)
        for tick in range(250):
            self.assertEqual(model.step(Event.DT_NONZERO), SelectionOutcome.EXACT_PACKET)
            finish_selected_tick(model, tick)
        expected_bits = raw_f32(self.frames[250].steering)
        self.assertNotEqual(expected_bits, 0)
        self.assertEqual(model.step(Event.DT_NONZERO), SelectionOutcome.EXACT_PACKET)
        self.assertEqual(model.selected_tick, 250)
        model.step(Event.DT_ZERO)
        self.assertEqual((model.machine.state, model.machine.frame_index), (State.WAITING, 250))
        self.assertIsNone(model.selected_tick)
        self.assertEqual(model.step(Event.DT_NONZERO), SelectionOutcome.EXACT_PACKET)
        self.assertEqual(model.selected_tick, 250)
        self.assertEqual(raw_f32(self.frames[model.selected_tick].steering), expected_bits)

    def test_initial_zero_values_remain_selected_not_skipped(self) -> None:
        self.assertTrue(all(raw_f32(frame.steering) == 0 for frame in self.frames[:250]))
        self.assertTrue(all((frame.skip_flags & 1) == 0 for frame in self.frames[:250]))
        model = AdapterModel.create(2, 1)
        for tick in range(2):
            self.assertEqual(model.step(Event.DT_NONZERO), SelectionOutcome.EXACT_PACKET)
            self.assertEqual(model.selected_tick, tick)
            self.assertEqual(apply_steering(0.0, 0xDEADBEEF12345678), 0x12345678)
            finish_selected_tick(model, tick)


if __name__ == "__main__":
    unittest.main()

