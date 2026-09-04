#!/usr/bin/env python3
"""Regression tests for faithful AluTasV2 action semantics."""

from __future__ import annotations

import struct
import unittest

from original_action_semantics_v1 import (
    BOUNDED_A9UER6_CAPABILITIES_V1,
    LIVE_PROVEN_CAPABILITIES_V1,
    Operation,
    Phase,
    RuntimeCapabilitiesV1,
    drift_brake_values,
    nitro_click_counts,
    plan_original_actions,
    require_runtime_capabilities,
    unsupported_runtime_fields,
)
from unified_tick_recording_v1 import (
    SKIP_ACCELERATOR,
    SKIP_BARREL_ANGULAR,
    SKIP_BARREL_RBX,
    SKIP_BRAKE,
    SKIP_NITRO,
    SKIP_RESPAWN,
    SKIP_STEER,
    SKIP_TRANSFORM,
    UnifiedTickFrameV1,
)


ALL_SKIPS = (
    SKIP_STEER
    | SKIP_BRAKE
    | SKIP_NITRO
    | SKIP_ACCELERATOR
    | SKIP_BARREL_ANGULAR
    | SKIP_BARREL_RBX
    | SKIP_RESPAWN
    | SKIP_TRANSFORM
)


def floats(*values: float) -> bytes:
    return struct.pack(f"<{len(values)}f", *values)


def frame(*, skip_flags: int = 0, nitro: int = 2, respawn: bool = True):
    return UnifiedTickFrameV1(
        tick=7,
        monotonic_ns=123,
        steering=0.25,
        brake=-1.0,
        accelerator=1.0,
        nitro_activations=nitro,
        skip_flags=skip_flags,
        respawn=respawn,
        barrel_angular=(1.0, 2.0, 3.0),
        barrel_rbx=(4.0, 5.0),
        transform=floats(*range(16)),
        linear_velocity=floats(6.0, 7.0, 8.0),
    )


class OriginalActionSemanticsTests(unittest.TestCase):
    def test_discrete_calls_are_after_delta_and_before_real_physics(self) -> None:
        steps = plan_original_actions(frame(), fixed_interval_us=16667)
        phases = [step.phase for step in steps]
        self.assertEqual(phases, sorted(phases))
        delta = next(i for i, step in enumerate(steps) if step.operation is Operation.APPLY_FIXED_DELTA)
        nitro = [i for i, step in enumerate(steps) if step.operation is Operation.CALL_REAL_NITRO_ACTIVATION]
        respawn = next(i for i, step in enumerate(steps) if step.operation is Operation.CALL_REAL_RESPAWN)
        physics = next(i for i, step in enumerate(steps) if step.operation is Operation.ENTER_REAL_GAME_PHYSICS)
        self.assertEqual(len(nitro), 2)
        self.assertLess(delta, nitro[0])
        self.assertLess(nitro[-1], respawn)
        self.assertLess(respawn, physics)

    def test_nitro_is_call_count_not_colour_enum(self) -> None:
        operations = {item.name.lower() for item in Operation}
        for colour in ("yellow", "blue", "purple", "red", "黄", "蓝", "紫", "红"):
            self.assertFalse(any(colour in name for name in operations))
        calls = [
            step
            for step in plan_original_actions(frame(nitro=2), fixed_interval_us=16667)
            if step.operation is Operation.CALL_REAL_NITRO_ACTIVATION
        ]
        self.assertEqual([step.ordinal for step in calls], [1, 2])

    def test_blue_timing_is_two_single_calls_on_distinct_ticks(self) -> None:
        self.assertEqual(nitro_click_counts(6, [0, 4]), (1, 0, 0, 0, 1, 0))

    def test_double_click_remains_two_calls_in_one_tick(self) -> None:
        self.assertEqual(nitro_click_counts(3, [1, 1]), (0, 2, 0))
        with self.assertRaisesRegex(ValueError, "at most two"):
            nitro_click_counts(3, [1, 1, 1])

    def test_nitro_skip_suppresses_calls_even_when_packet_contains_two(self) -> None:
        steps = plan_original_actions(frame(skip_flags=SKIP_NITRO), fixed_interval_us=16667)
        self.assertNotIn(Operation.CALL_REAL_NITRO_ACTIVATION, [step.operation for step in steps])

    def test_respawn_is_one_tick_edge_and_transients_reset_at_end(self) -> None:
        active = plan_original_actions(frame(respawn=True), fixed_interval_us=16667)
        inactive = plan_original_actions(frame(respawn=False), fixed_interval_us=16667)
        self.assertEqual(sum(step.operation is Operation.CALL_REAL_RESPAWN for step in active), 1)
        self.assertEqual(sum(step.operation is Operation.CALL_REAL_RESPAWN for step in inactive), 0)
        reset = active[-1]
        self.assertEqual(reset.phase, Phase.END_TICK)
        self.assertEqual(reset.operation, Operation.RESET_TRANSIENT_ACTION_STATE)
        self.assertEqual(
            reset.value,
            ("barrel_angular", "barrel_rbx", "respawn", "nitro_activation_count"),
        )

    def test_drift_is_brake_hold_not_synthetic_flag(self) -> None:
        self.assertEqual(drift_brake_values(6, [1, 2, 3]), (0.0, -1.0, -1.0, -1.0, 0.0, 0.0))
        self.assertFalse(any("drift" in item.name.lower() for item in Operation))

    def test_every_skip_is_independent(self) -> None:
        steps = plan_original_actions(frame(skip_flags=ALL_SKIPS), fixed_interval_us=16667)
        operations = {step.operation for step in steps}
        for operation in (
            Operation.CALL_REAL_NITRO_ACTIVATION,
            Operation.CALL_REAL_RESPAWN,
            Operation.OVERRIDE_BRAKE_VALUE,
            Operation.OVERRIDE_STEERING_VALUE,
            Operation.OVERRIDE_ACCELERATOR_VALUE,
            Operation.OVERRIDE_BARREL_RBX,
            Operation.OVERRIDE_BARREL_ANGULAR,
            Operation.FINAL_TRANSFORM_LINEAR_CONDITIONAL,
        ):
            self.assertNotIn(operation, operations)
        self.assertIn(Operation.PRESERVE_GAME_FINAL_TRANSFORM_LINEAR, operations)

    def test_bounded_a9uer6_contract_accepts_brake_and_fails_closed_for_unproven_actions(self) -> None:
        current = frame(
            skip_flags=SKIP_NITRO | SKIP_ACCELERATOR | SKIP_BARREL_ANGULAR | SKIP_BARREL_RBX | SKIP_RESPAWN
        )
        self.assertIs(LIVE_PROVEN_CAPABILITIES_V1, BOUNDED_A9UER6_CAPABILITIES_V1)
        self.assertTrue(BOUNDED_A9UER6_CAPABILITIES_V1.brake)
        self.assertEqual(unsupported_runtime_fields(current, BOUNDED_A9UER6_CAPABILITIES_V1), ())
        nitro_enabled = frame(
            skip_flags=current.skip_flags & ~SKIP_NITRO,
            nitro=1,
        )
        self.assertEqual(
            unsupported_runtime_fields(nitro_enabled, BOUNDED_A9UER6_CAPABILITIES_V1),
            ("nitro_activation",),
        )
        with self.assertRaisesRegex(ValueError, "nitro_activation"):
            require_runtime_capabilities(nitro_enabled, BOUNDED_A9UER6_CAPABILITIES_V1)

    def test_fully_proven_future_capability_accepts_full_packet(self) -> None:
        require_runtime_capabilities(
            frame(),
            RuntimeCapabilitiesV1(True, True, True, True, True, True, True, True),
        )


if __name__ == "__main__":
    unittest.main()
