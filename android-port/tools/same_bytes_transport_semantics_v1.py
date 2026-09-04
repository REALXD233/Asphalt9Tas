#!/usr/bin/env python3
"""Pure policy model for the one-shot same-bytes transport gate."""

from __future__ import annotations

from dataclasses import dataclass


NATIVE_BODY_SIZE = 0x290
WRAPPER_AUDIT_SIZE = 0x48
TRANSFORM_OFFSET = 0x10
TRANSFORM_SIZE = 64
LINEAR_OFFSET = 0x150
LINEAR_SIZE = 12


@dataclass(frozen=True)
class SameBytesWrite:
    native_offset: int
    payload: bytes


def plan_same_bytes_transport(native_body: bytes) -> tuple[SameBytesWrite, ...]:
    if len(native_body) != NATIVE_BODY_SIZE:
        raise ValueError(f"native body must be exactly {NATIVE_BODY_SIZE} bytes")
    return (
        SameBytesWrite(
            TRANSFORM_OFFSET,
            bytes(native_body[TRANSFORM_OFFSET : TRANSFORM_OFFSET + TRANSFORM_SIZE]),
        ),
        SameBytesWrite(
            LINEAR_OFFSET,
            bytes(native_body[LINEAR_OFFSET : LINEAR_OFFSET + LINEAR_SIZE]),
        ),
    )


def verify_immediate_audit(before: bytes, immediate: bytes) -> None:
    if len(before) != len(immediate):
        raise ValueError("audit snapshots have different sizes")
    if before != immediate:
        first = next(
            index
            for index, (left, right) in enumerate(zip(before, immediate, strict=True))
            if left != right
        )
        raise ValueError(f"immediate audit changed at byte {first}")
