#!/usr/bin/env python3
"""Pure model of upstream ReplayStateManager playback control flow.

This is intentionally descriptive, including the saturating-last-index packet
duplication. It is not the Android product policy and performs no I/O.
"""

from __future__ import annotations

from dataclasses import dataclass, replace


@dataclass(frozen=True)
class ReplayV1:
    ticks: tuple[int, ...]
    fixed_interval_us: int = 8333

    def __post_init__(self) -> None:
        if not self.ticks:
            raise ValueError("replay must contain at least one frame")
        if self.ticks != tuple(range(len(self.ticks))):
            raise ValueError("upstream replay ticks must equal their indices")
        if not 0 <= self.fixed_interval_us <= 0xFFFFFFFF:
            raise ValueError("fixed interval must be uint32")


@dataclass(frozen=True)
class PlaybackSessionV1:
    replay: ReplayV1
    final_tick: int
    frame_index: int = 0


@dataclass(frozen=True)
class ControlPlaneV1:
    queue_capacity: int
    queue: tuple[int, ...] = ()
    session: PlaybackSessionV1 | None = None
    block_mode_active: bool = False
    fixed_interval_us: int | None = None

    def __post_init__(self) -> None:
        if self.queue_capacity < 1:
            raise ValueError("queue capacity must be positive")
        if len(self.queue) > self.queue_capacity:
            raise ValueError("queue exceeds capacity")

    def queue_replay(
        self,
        replay: ReplayV1,
        target_tick: int,
        *,
        in_race: bool,
        current_race_tick: int,
    ) -> tuple["ControlPlaneV1", bool]:
        if target_tick < 0:
            raise ValueError("target tick must be nonnegative")
        if (
            in_race
            and self.session is not None
            and self.session.final_tick >= current_race_tick
        ):
            return self, False
        final_tick = min(target_tick, len(replay.ticks) - 1)
        result = replace(
            self,
            queue=(),
            session=PlaybackSessionV1(replay, final_tick, 0),
        )
        if not in_race:
            result = result._init_new_replay()
        return result, True

    def clear_queued_replay(self) -> "ControlPlaneV1":
        return replace(self, queue=(), session=None, block_mode_active=False)

    def change_target_tick(self, target_tick: int) -> tuple["ControlPlaneV1", bool]:
        if target_tick < 0:
            raise ValueError("target tick must be nonnegative")
        if self.session is None:
            return self, False
        final_tick = min(target_tick, len(self.session.replay.ticks) - 1)
        return replace(self, session=replace(self.session, final_tick=final_tick)), True

    def on_update(self, *, current_race_tick: int) -> "ControlPlaneV1":
        # This matches the source ordering: completion is checked before refill.
        if self.session is None or current_race_tick >= self.session.final_tick:
            return replace(self, block_mode_active=False)
        return self._push_playback_frames()

    def on_race_ended(self) -> "ControlPlaneV1":
        result = self._init_new_replay() if self.session is not None else self
        # Upstream initializes the queued replay and then clears the input queue.
        return replace(result, queue=())

    def _init_new_replay(self) -> "ControlPlaneV1":
        if self.session is None:
            raise ValueError("cannot initialize without queued replay")
        return replace(
            self,
            session=replace(self.session, frame_index=0),
            block_mode_active=True,
            fixed_interval_us=self.session.replay.fixed_interval_us,
        )

    def _push_playback_frames(self) -> "ControlPlaneV1":
        if self.session is None:
            raise ValueError("cannot push without queued replay")
        queue = list(self.queue)
        session = self.session
        while len(queue) < self.queue_capacity:
            tick = session.replay.ticks[session.frame_index]
            if tick > session.final_tick:
                break
            queue.append(tick)
            # Literal Replay::IncrementFrameIndex saturation. At the final
            # replay frame this repeats the last packet until the queue is full.
            next_index = min(session.frame_index + 1, len(session.replay.ticks) - 1)
            session = replace(session, frame_index=next_index)
        return replace(self, queue=tuple(queue), session=session)
