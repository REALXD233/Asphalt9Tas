#!/usr/bin/env python3
"""Pure AluTasV2 action ordering and Android capability contract.

This module deliberately models nitro as a count of calls to the real game
activation function.  Yellow/blue/purple/red are outcomes of the game state
machine and are not replay packet values.  Likewise, drift is represented by
the brake/longitudinal value consumed by the game, never by a synthetic flag.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from enum import IntEnum, auto
from typing import Iterable

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


class Phase(IntEnum):
    PACKET_SELECTION = 0
    FIXED_DELTA = auto()
    DISCRETE_PRE_PHYSICS = auto()
    CONTINUOUS_GAME_CONSUME = auto()
    GAME_PHYSICS = auto()
    BARREL_POST_CALL = auto()
    FINAL_WRITER = auto()
    END_TICK = auto()


class Operation(IntEnum):
    SELECT_PACKET = 0
    APPLY_FIXED_DELTA = auto()
    CALL_REAL_NITRO_ACTIVATION = auto()
    CALL_REAL_RESPAWN = auto()
    OVERRIDE_BRAKE_VALUE = auto()
    OVERRIDE_STEERING_VALUE = auto()
    OVERRIDE_ACCELERATOR_VALUE = auto()
    ENTER_REAL_GAME_PHYSICS = auto()
    OVERRIDE_BARREL_RBX = auto()
    OVERRIDE_BARREL_ANGULAR = auto()
    FINAL_TRANSFORM_LINEAR_CONDITIONAL = auto()
    PRESERVE_GAME_FINAL_TRANSFORM_LINEAR = auto()
    RESET_TRANSIENT_ACTION_STATE = auto()


@dataclass(frozen=True)
class ActionStepV1:
    phase: Phase
    operation: Operation
    value: int | float | tuple[float, ...] | tuple[str, ...] | None = None
    ordinal: int = 0


@dataclass(frozen=True)
class RuntimeCapabilitiesV1:
    steering: bool = False
    brake: bool = False
    accelerator: bool = False
    nitro_activation: bool = False
    respawn: bool = False
    barrel_angular: bool = False
    barrel_rbx: bool = False
    final_transform_linear: bool = False


# Fields admitted by the bounded A9USR4 -> A9UER6 344-frame replay evidence.
# This is deliberately not a claim of authoritative packet blocking, complete
# lifecycle parity, full-race determinism, or support for the missing actions.
BOUNDED_A9UER6_CAPABILITIES_V1 = RuntimeCapabilitiesV1(
    steering=True,
    brake=True,
    final_transform_linear=True,
)

# Compatibility name for older offline callers.  New code should use the
# evidence-scoped name above instead of treating a bounded proof as global
# Android live parity.
LIVE_PROVEN_CAPABILITIES_V1 = BOUNDED_A9UER6_CAPABILITIES_V1


def unsupported_runtime_fields(
    frame: UnifiedTickFrameV1, capabilities: RuntimeCapabilitiesV1
) -> tuple[str, ...]:
    unsupported: list[str] = []
    fields = (
        ("steering", SKIP_STEER, capabilities.steering),
        ("brake", SKIP_BRAKE, capabilities.brake),
        ("accelerator", SKIP_ACCELERATOR, capabilities.accelerator),
        ("nitro_activation", SKIP_NITRO, capabilities.nitro_activation),
        ("respawn", SKIP_RESPAWN, capabilities.respawn),
        ("barrel_angular", SKIP_BARREL_ANGULAR, capabilities.barrel_angular),
        ("barrel_rbx", SKIP_BARREL_RBX, capabilities.barrel_rbx),
        (
            "final_transform_linear",
            SKIP_TRANSFORM,
            capabilities.final_transform_linear,
        ),
    )
    for name, skip_flag, supported in fields:
        if not frame.skip_flags & skip_flag and not supported:
            unsupported.append(name)
    return tuple(unsupported)


def require_runtime_capabilities(
    frame: UnifiedTickFrameV1, capabilities: RuntimeCapabilitiesV1
) -> None:
    unsupported = unsupported_runtime_fields(frame, capabilities)
    if unsupported:
        raise ValueError(
            "runtime capability is not proven for: " + ", ".join(unsupported)
        )


def plan_original_actions(
    frame: UnifiedTickFrameV1, *, fixed_interval_us: int
) -> tuple[ActionStepV1, ...]:
    """Return the original semantic order without claiming Android bindings."""

    if not 1_000 <= fixed_interval_us <= 100_000:
        raise ValueError("fixed_interval_us is outside the A9UTK1 range")
    if not all(
        math.isfinite(value)
        for value in (frame.steering, frame.brake, frame.accelerator)
    ):
        raise ValueError("continuous control contains a non-finite value")
    if frame.nitro_activations not in (0, 1, 2):
        raise ValueError("nitro activation count must be 0, 1, or 2")

    steps: list[ActionStepV1] = [
        ActionStepV1(Phase.PACKET_SELECTION, Operation.SELECT_PACKET, frame.tick),
        ActionStepV1(
            Phase.FIXED_DELTA, Operation.APPLY_FIXED_DELTA, fixed_interval_us
        ),
    ]
    if not frame.skip_flags & SKIP_NITRO:
        for ordinal in range(frame.nitro_activations):
            steps.append(
                ActionStepV1(
                    Phase.DISCRETE_PRE_PHYSICS,
                    Operation.CALL_REAL_NITRO_ACTIVATION,
                    ordinal + 1,
                    ordinal + 1,
                )
            )
    if not frame.skip_flags & SKIP_RESPAWN and frame.respawn:
        steps.append(
            ActionStepV1(
                Phase.DISCRETE_PRE_PHYSICS, Operation.CALL_REAL_RESPAWN, True
            )
        )

    # The three continuous values are independent game-consumption hooks in
    # AluTasV2.  Their shared phase does not invent an unproven relative order
    # for the still-unmapped Android accelerator path.
    if not frame.skip_flags & SKIP_BRAKE:
        steps.append(
            ActionStepV1(
                Phase.CONTINUOUS_GAME_CONSUME,
                Operation.OVERRIDE_BRAKE_VALUE,
                frame.brake,
            )
        )
    if not frame.skip_flags & SKIP_STEER:
        steps.append(
            ActionStepV1(
                Phase.CONTINUOUS_GAME_CONSUME,
                Operation.OVERRIDE_STEERING_VALUE,
                frame.steering,
            )
        )
    if not frame.skip_flags & SKIP_ACCELERATOR:
        steps.append(
            ActionStepV1(
                Phase.CONTINUOUS_GAME_CONSUME,
                Operation.OVERRIDE_ACCELERATOR_VALUE,
                frame.accelerator,
            )
        )
    steps.append(
        ActionStepV1(Phase.GAME_PHYSICS, Operation.ENTER_REAL_GAME_PHYSICS)
    )

    if not frame.skip_flags & SKIP_BARREL_RBX:
        steps.append(
            ActionStepV1(
                Phase.BARREL_POST_CALL,
                Operation.OVERRIDE_BARREL_RBX,
                frame.barrel_rbx,
            )
        )
    if not frame.skip_flags & SKIP_BARREL_ANGULAR:
        steps.append(
            ActionStepV1(
                Phase.BARREL_POST_CALL,
                Operation.OVERRIDE_BARREL_ANGULAR,
                frame.barrel_angular,
            )
        )
    steps.append(
        ActionStepV1(
            Phase.FINAL_WRITER,
            Operation.PRESERVE_GAME_FINAL_TRANSFORM_LINEAR
            if frame.skip_flags & SKIP_TRANSFORM
            else Operation.FINAL_TRANSFORM_LINEAR_CONDITIONAL,
        )
    )
    steps.append(
        ActionStepV1(
            Phase.END_TICK,
            Operation.RESET_TRANSIENT_ACTION_STATE,
            ("barrel_angular", "barrel_rbx", "respawn", "nitro_activation_count"),
        )
    )
    return tuple(steps)


def nitro_click_counts(
    frame_count: int, click_ticks: Iterable[int]
) -> tuple[int, ...]:
    """Compile raw click ticks; two clicks in one tick remain two real calls."""

    if frame_count < 1:
        raise ValueError("frame_count must be positive")
    counts = [0] * frame_count
    for tick in click_ticks:
        if type(tick) is not int or not 0 <= tick < frame_count:
            raise ValueError("nitro click tick is outside the recording")
        counts[tick] += 1
        if counts[tick] > 2:
            raise ValueError("at most two nitro activations are allowed per tick")
    return tuple(counts)


def drift_brake_values(
    frame_count: int,
    held_ticks: Iterable[int],
    *,
    pressed_value: float = -1.0,
    released_value: float = 0.0,
) -> tuple[float, ...]:
    """Compile drift/S hold as consumed brake values, not a drift flag."""

    if frame_count < 1:
        raise ValueError("frame_count must be positive")
    if not math.isfinite(pressed_value) or not math.isfinite(released_value):
        raise ValueError("brake values must be finite")
    output = [released_value] * frame_count
    for tick in held_ticks:
        if type(tick) is not int or not 0 <= tick < frame_count:
            raise ValueError("drift hold tick is outside the recording")
        output[tick] = pressed_value
    return tuple(output)
