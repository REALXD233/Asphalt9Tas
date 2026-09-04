#!/usr/bin/env python3
"""Pure planning logic for the AluTasV2 transform/linear correction.

This module cannot access another process.  It accepts four byte strings and
returns either no writes or the two exact Android native-body write ranges.
Keeping the decision logic side-effect free makes the source parity contract
testable before any runtime transport is authorized.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass


TRANSFORM_OFFSET = 0x10
TRANSFORM_SIZE = 16 * 4
LINEAR_VELOCITY_OFFSET = 0x150
LINEAR_VELOCITY_SIZE = 3 * 4


@dataclass(frozen=True)
class PlannedCopy:
    native_offset: int
    payload: bytes


def _require_size(name: str, value: bytes, expected: int) -> None:
    if len(value) != expected:
        raise ValueError(f"{name} must be exactly {expected} bytes, got {len(value)}")


def _component_float_equal(left: bytes, right: bytes) -> bool:
    """Match the upstream C++ component-wise float ``operator==`` behavior."""

    return all(
        left_value == right_value
        for (left_value,), (right_value,) in zip(
            struct.iter_unpack("<f", left),
            struct.iter_unpack("<f", right),
            strict=True,
        )
    )


def plan_transform_linear_correction(
    current_transform: bytes,
    current_linear_velocity: bytes,
    recorded_transform: bytes,
    recorded_linear_velocity: bytes,
) -> tuple[PlannedCopy, ...]:
    """Return the exact all-or-nothing final-correction copy plan.

    Equal state yields an empty tuple.  If either the transform or linear
    velocity differs, both recorded byte ranges are returned in source order.
    Comparisons are numeric float equality, while returned payloads preserve
    their raw bytes.
    """

    _require_size("current_transform", current_transform, TRANSFORM_SIZE)
    _require_size(
        "current_linear_velocity", current_linear_velocity, LINEAR_VELOCITY_SIZE
    )
    _require_size("recorded_transform", recorded_transform, TRANSFORM_SIZE)
    _require_size(
        "recorded_linear_velocity", recorded_linear_velocity, LINEAR_VELOCITY_SIZE
    )

    transform_equal = _component_float_equal(current_transform, recorded_transform)
    linear_equal = _component_float_equal(
        current_linear_velocity, recorded_linear_velocity
    )
    if transform_equal and linear_equal:
        return ()

    return (
        PlannedCopy(TRANSFORM_OFFSET, bytes(recorded_transform)),
        PlannedCopy(LINEAR_VELOCITY_OFFSET, bytes(recorded_linear_velocity)),
    )
