#!/usr/bin/env python3
"""Pure model of the checked-in AluTasV2 OnNewTick/OnEndTick behavior.

This deliberately follows the source control flow, including its surprising
ActiveBlockThread future-head behavior.  It is a specification for the Android
port, not a claim that the Android runtime binding already exists.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
from enum import Enum, auto
from typing import Sequence


class ReplayMode(Enum):
    INACTIVE = auto()
    ACTIVE_BLOCK_THREAD = auto()
    ACTIVE_NO_BLOCK = auto()


class SelectionOutcome(Enum):
    OUTSIDE_RACE = auto()
    VERSION_MISMATCH = auto()
    INACTIVE = auto()
    BLOCKED_EMPTY = auto()
    MODE_CANCELLED = auto()
    EXACT_PACKET = auto()
    FUTURE_HEAD_GAP = auto()
    NO_PACKET = auto()
    STALE_DROPPED_NO_BLOCK = auto()


@dataclass(frozen=True)
class ReplayPacketV1:
    tick: int
    payload: object | None = None

    def __post_init__(self) -> None:
        if type(self.tick) is not int or not 0 <= self.tick <= 0xFFFFFFFF:
            raise ValueError("tick must be uint32")


@dataclass(frozen=True)
class SelectionResultV1:
    outcome: SelectionOutcome
    selected: ReplayPacketV1 | None
    remaining: tuple[ReplayPacketV1, ...]
    stale_dropped: int = 0
    clear_previous_packet: bool = True


def select_on_new_tick(
    *,
    current_tick: int,
    mode: ReplayMode,
    queue: Sequence[ReplayPacketV1],
    in_race: bool = True,
    communication_version_matches: bool = True,
    block_mode_still_active: bool = True,
) -> SelectionResultV1:
    """Model one complete source-level OnNewTick selection attempt.

    ``BLOCKED_EMPTY`` represents the source's busy wait at the point where it
    needs another general-buffer pump.  The caller may invoke the model again
    after adding a packet or changing replay mode.
    """

    if type(current_tick) is not int or not 0 <= current_tick <= 0xFFFFFFFF:
        raise ValueError("current_tick must be uint32")
    remaining = list(queue)
    if not in_race:
        return SelectionResultV1(SelectionOutcome.OUTSIDE_RACE, None, tuple(remaining))
    if not communication_version_matches:
        return SelectionResultV1(SelectionOutcome.VERSION_MISMATCH, None, tuple(remaining))
    if mode is ReplayMode.INACTIVE:
        return SelectionResultV1(SelectionOutcome.INACTIVE, None, tuple(remaining))

    if mode is ReplayMode.ACTIVE_NO_BLOCK:
        if not remaining:
            return SelectionResultV1(SelectionOutcome.NO_PACKET, None, ())
        head = remaining[0]
        if head.tick == current_tick:
            return SelectionResultV1(
                SelectionOutcome.EXACT_PACKET, head, tuple(remaining[1:])
            )
        if head.tick < current_tick:
            return SelectionResultV1(
                SelectionOutcome.STALE_DROPPED_NO_BLOCK,
                None,
                tuple(remaining[1:]),
                stale_dropped=1,
            )
        return SelectionResultV1(
            SelectionOutcome.FUTURE_HEAD_GAP, None, tuple(remaining)
        )

    if not block_mode_still_active:
        return SelectionResultV1(
            SelectionOutcome.MODE_CANCELLED, None, tuple(remaining)
        )

    stale_dropped = 0
    while remaining:
        head = remaining[0]
        if head.tick == current_tick:
            return SelectionResultV1(
                SelectionOutcome.EXACT_PACKET,
                head,
                tuple(remaining[1:]),
                stale_dropped,
            )
        if head.tick < current_tick:
            remaining.pop(0)
            stale_dropped += 1
            continue
        # This is the literal source behavior at DetourFunctions.cpp:467:
        # a future head breaks the blocking loop and remains queued.
        return SelectionResultV1(
            SelectionOutcome.FUTURE_HEAD_GAP,
            None,
            tuple(remaining),
            stale_dropped,
        )
    return SelectionResultV1(
        SelectionOutcome.BLOCKED_EMPTY, None, (), stale_dropped
    )


@dataclass(frozen=True)
class TickInputStateV1:
    race_tick: int
    steer: float = 0.0
    brake: float = 0.0
    accelerator: float = 1.0
    nitro_activation_count: int = 0
    respawn: bool = False
    barrel_angular: tuple[float, float, float] = (0.0, 0.0, 0.0)
    barrel_rbx: tuple[float, float] = (0.0, 0.0)


@dataclass(frozen=True)
class EndTickResultV1:
    published: TickInputStateV1
    next_state: TickInputStateV1


def end_tick(state: TickInputStateV1, *, in_race: bool) -> EndTickResultV1:
    """Publish first, then clear transients and increment only while in race."""

    if type(state.race_tick) is not int or not 0 <= state.race_tick <= 0xFFFFFFFF:
        raise ValueError("race_tick must be uint32")
    if in_race and state.race_tick == 0xFFFFFFFF:
        raise OverflowError("race tick would overflow uint32")
    next_state = replace(
        state,
        race_tick=state.race_tick + 1 if in_race else state.race_tick,
        nitro_activation_count=0,
        respawn=False,
        barrel_angular=(0.0, 0.0, 0.0),
        barrel_rbx=(0.0, 0.0),
    )
    return EndTickResultV1(published=state, next_state=next_state)
