#!/usr/bin/env python3
"""Pure two-phase state machine for the build-only unified tick executor."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto


class State(Enum):
    WAITING = auto()
    INPUT_OPEN = auto()
    SAW_C98 = auto()
    WAIT_COMPLETION = auto()
    WAIT_CALLBACK_OPEN = auto()
    WAIT_F64 = auto()
    WAIT_CALLBACK_CLOSE = auto()
    WAIT_DEFERRED_CLEAR = auto()
    WAIT_WORLD_COMMIT = auto()
    COMPLETE = auto()


class Event(Enum):
    DT_NONZERO = auto()
    DT_ZERO = auto()
    C98 = auto()
    C9C = auto()
    PREFIX_CERTIFIED = auto()
    COMPLETION = auto()
    CALLBACK_OPEN = auto()
    F64 = auto()
    CALLBACK_CLOSE = auto()
    DEFERRED_CALLBACK_CLEAR = auto()
    WORLD_COMMIT = auto()


class Action(Enum):
    SELECT_FRAME_AND_APPLY_DT = auto()
    PAUSED_CYCLE_NO_COMMIT = auto()
    WRITE_CONTROL_PAIR = auto()
    REARM_POST_PHASE = auto()
    CORRECTION_ZERO_WRITE = auto()
    CORRECTION_COPY_BOTH = auto()
    CORRECTION_SKIPPED = auto()
    COMMIT_FRAME_AND_REARM_INPUT = auto()
    COMMIT_FINAL_FRAME = auto()


@dataclass(frozen=True)
class Machine:
    frame_count: int
    frame_index: int = 0
    state: State = State.WAITING

    def __post_init__(self) -> None:
        if self.frame_count < 1:
            raise ValueError("frame_count must be positive")
        if not 0 <= self.frame_index <= self.frame_count:
            raise ValueError("frame_index outside recording")

    def step(
        self,
        event: Event,
        *,
        payload_equal: bool | None = None,
        skip_transform: bool = False,
    ) -> tuple["Machine", tuple[Action, ...]]:
        if self.state is State.COMPLETE:
            raise ValueError("event after replay completion")
        if self.state is State.WAITING and event is Event.DT_ZERO:
            return self, ()
        if self.state is State.WAITING and event is Event.DT_NONZERO:
            return (
                Machine(self.frame_count, self.frame_index, State.INPUT_OPEN),
                (Action.SELECT_FRAME_AND_APPLY_DT,),
            )
        if self.state is State.INPUT_OPEN and event is Event.DT_ZERO:
            return (
                Machine(self.frame_count, self.frame_index, State.WAITING),
                (Action.PAUSED_CYCLE_NO_COMMIT,),
            )
        if self.state is State.INPUT_OPEN and event is Event.C98:
            return (
                Machine(self.frame_count, self.frame_index, State.SAW_C98),
                (Action.WRITE_CONTROL_PAIR,),
            )
        if self.state is State.SAW_C98 and event is Event.C9C:
            return (
                Machine(
                    self.frame_count,
                    self.frame_index,
                    State.WAIT_COMPLETION,
                ),
                (Action.WRITE_CONTROL_PAIR, Action.REARM_POST_PHASE),
            )
        if self.state is State.WAIT_COMPLETION and event is Event.COMPLETION:
            return (
                Machine(
                    self.frame_count,
                    self.frame_index,
                    State.WAIT_CALLBACK_OPEN,
                ),
                (),
            )
        if self.state is State.WAIT_COMPLETION and event is Event.PREFIX_CERTIFIED:
            return (
                Machine(self.frame_count, self.frame_index, State.WAIT_F64),
                (),
            )
        if self.state is State.WAIT_CALLBACK_OPEN and event is Event.CALLBACK_OPEN:
            return (
                Machine(self.frame_count, self.frame_index, State.WAIT_F64),
                (),
            )
        if self.state is State.WAIT_F64 and event is Event.F64:
            return (
                Machine(
                    self.frame_count,
                    self.frame_index,
                    State.WAIT_CALLBACK_CLOSE,
                ),
                (),
            )
        if self.state is State.WAIT_CALLBACK_CLOSE and event is Event.CALLBACK_CLOSE:
            if payload_equal is None:
                raise ValueError("callback close requires comparator result")
            correction = (
                Action.CORRECTION_SKIPPED
                if skip_transform
                else Action.CORRECTION_ZERO_WRITE
                if payload_equal
                else Action.CORRECTION_COPY_BOTH
            )
            return (
                Machine(
                    self.frame_count,
                    self.frame_index,
                    State.WAIT_DEFERRED_CLEAR,
                ),
                (correction,),
            )
        if self.state is State.WAIT_DEFERRED_CLEAR and event is Event.DEFERRED_CALLBACK_CLEAR:
            return (
                Machine(self.frame_count, self.frame_index, State.WAIT_WORLD_COMMIT),
                (),
            )
        if self.state is State.WAIT_WORLD_COMMIT and event is Event.WORLD_COMMIT:
            next_index = self.frame_index + 1
            if next_index == self.frame_count:
                return (
                    Machine(self.frame_count, next_index, State.COMPLETE),
                    (Action.COMMIT_FINAL_FRAME,),
                )
            return (
                Machine(self.frame_count, next_index, State.WAITING),
                (Action.COMMIT_FRAME_AND_REARM_INPUT,),
            )
        raise ValueError(f"invalid transition: {self.state.name} + {event.name}")
