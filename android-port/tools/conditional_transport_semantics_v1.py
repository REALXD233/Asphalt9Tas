#!/usr/bin/env python3
"""Pure immediate-audit policy for conditional A9NPS1 correction."""

from __future__ import annotations

from transform_correction_semantics_v1 import plan_transform_linear_correction


NATIVE_BODY_SIZE = 0x290
AUDIT_SNAPSHOT_SIZE = 804
TRANSFORM_OFFSET = 0x10
TRANSFORM_SIZE = 64
LINEAR_OFFSET = 0x150
LINEAR_SIZE = 12


def expected_immediate_audit(
    before: bytes, recorded_transform: bytes, recorded_linear: bytes
) -> tuple[bytes, bool]:
    if len(before) != AUDIT_SNAPSHOT_SIZE:
        raise ValueError(f"before audit must be exactly {AUDIT_SNAPSHOT_SIZE} bytes")
    current_transform = before[TRANSFORM_OFFSET : TRANSFORM_OFFSET + TRANSFORM_SIZE]
    current_linear = before[LINEAR_OFFSET : LINEAR_OFFSET + LINEAR_SIZE]
    plan = plan_transform_linear_correction(
        current_transform,
        current_linear,
        recorded_transform,
        recorded_linear,
    )
    expected = bytearray(before)
    for write in plan:
        expected[write.native_offset : write.native_offset + len(write.payload)] = write.payload
    return bytes(expected), bool(plan)


def verify_conditional_immediate(expected: bytes, immediate: bytes) -> None:
    if len(expected) != len(immediate):
        raise ValueError("conditional audit snapshots have different sizes")
    if expected != immediate:
        first = next(
            index
            for index, (left, right) in enumerate(zip(expected, immediate, strict=True))
            if left != right
        )
        raise ValueError(f"conditional immediate audit changed at byte {first}")
