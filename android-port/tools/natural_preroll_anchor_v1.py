#!/usr/bin/env python3
"""Strict A9NPA1 natural pre-roll anchor binder and verifier."""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import struct
from dataclasses import dataclass, replace
from pathlib import Path

from synchronized_brake_recording_v1 import MAGIC as BRAKE_REPORT_MAGIC
from synchronized_brake_recording_v1 import decode_sync_brake_report
from synchronized_action_window_recording_v1 import MAGIC as ACTION_REPORT_MAGIC
from synchronized_action_window_recording_v1 import decode_action_window_report
from synchronized_action_until_release_recording_v1 import MAGIC as ACTION_RELEASE_MAGIC
from synchronized_action_until_release_recording_v1 import decode_action_until_release_report
from synchronized_tick_recording_v1 import decode_sync_report
from unified_tick_recording_v1 import SUPPORTED_BUILD_ID, decode_recording


MAGIC = b"A9NPA1\0\0"
VERSION = 1
SIZE = 424

FLAG_TARGET_VERIFIED = 1 << 0
FLAG_IDENTITY_VERIFIED = 1 << 1
FLAG_ANCHOR_CERTIFIED = 1 << 2
FLAG_SOURCE_HASHES_BOUND = 1 << 3
FLAG_FRAME0_BOUND = 1 << 4
UNBOUND_FLAGS = (
    FLAG_TARGET_VERIFIED
    | FLAG_IDENTITY_VERIFIED
    | FLAG_ANCHOR_CERTIFIED
    | FLAG_FRAME0_BOUND
)
BOUND_FLAGS = UNBOUND_FLAGS | FLAG_SOURCE_HASHES_BOUND

_ANCHOR = struct.Struct(
    "<8s6I20s4s32s32s6Q2i7Q2QH6s2Q64s12s64s12s"
)
assert _ANCHOR.size == SIZE


@dataclass(frozen=True)
class NaturalPrerollAnchorV1:
    flags: int
    fixed_interval_us: int
    frame_count: int
    build_id: bytes
    recording_sha256: bytes
    report_sha256: bytes
    source_pid: int
    source_library_base: int
    source_main_object: int
    source_final_owner: int
    source_physics_context: int
    source_native_body: int
    cycle_tid: int
    commit_tid: int
    events: tuple[int, int, int, int, int, int, int]
    completion_before: int
    completion_after: int
    callback_flags: int
    c98_pair_after: int
    c9c_pair_after: int
    anchor_transform: bytes
    anchor_linear: bytes
    frame0_transform: bytes
    frame0_linear: bytes


def _finite_physics(transform: bytes, linear: bytes) -> bool:
    values = struct.unpack("<19f", transform + linear)
    return all(math.isfinite(value) and abs(value) <= 1_000_000.0 for value in values)


def decode_anchor(blob: bytes, *, require_bound: bool = True) -> NaturalPrerollAnchorV1:
    if len(blob) != SIZE:
        raise ValueError(f"A9NPA1 length must be exactly {SIZE} bytes")
    values = _ANCHOR.unpack(blob)
    if values[0] != MAGIC or values[1] != VERSION or values[2] != SIZE:
        raise ValueError("unsupported A9NPA1 magic/version/size")
    flags = values[3]
    expected_flags = BOUND_FLAGS if require_bound else UNBOUND_FLAGS
    if flags != expected_flags:
        raise ValueError(
            f"A9NPA1 flags must be 0x{expected_flags:x}, got 0x{flags:x}"
        )
    if values[4] < 1_000 or values[4] > 100_000 or values[5] < 2:
        raise ValueError("A9NPA1 fixed interval/frame count is invalid")
    if values[6] != 0 or values[7] != SUPPORTED_BUILD_ID or values[8] != bytes(4):
        raise ValueError("A9NPA1 reserved/build field mismatch")
    recording_hash, report_hash = values[9], values[10]
    if require_bound:
        if recording_hash == bytes(32) or report_hash == bytes(32):
            raise ValueError("A9NPA1 source hashes are not bound")
    elif recording_hash != bytes(32) or report_hash != bytes(32):
        raise ValueError("unbound A9NPA1 must contain zero source hashes")
    identities = values[11:17]
    if any(value == 0 for value in identities):
        raise ValueError("A9NPA1 contains a zero source identity")
    cycle_tid, commit_tid = values[17], values[18]
    if cycle_tid <= 0 or commit_tid <= 0 or cycle_tid == commit_tid:
        raise ValueError("A9NPA1 cycle/commit thread identity is invalid")
    events = tuple(values[19:26])
    if not all(left < right for left, right in zip(events, events[1:])):
        raise ValueError("A9NPA1 event certificate is not strictly ordered")
    completion_before, completion_after = values[26], values[27]
    if completion_before == completion_after:
        raise ValueError("A9NPA1 completion token did not change")
    callback_flags = values[28]
    if callback_flags & 0xFF != 1 or values[29] != bytes(6):
        raise ValueError("A9NPA1 callback/reserved field is invalid")
    if not _finite_physics(values[32], values[33]):
        raise ValueError("A9NPA1 anchor physics is invalid")
    if not _finite_physics(values[34], values[35]):
        raise ValueError("A9NPA1 frame-0 physics is invalid")
    return NaturalPrerollAnchorV1(
        flags=flags,
        fixed_interval_us=values[4],
        frame_count=values[5],
        build_id=values[7],
        recording_sha256=recording_hash,
        report_sha256=report_hash,
        source_pid=values[11],
        source_library_base=values[12],
        source_main_object=values[13],
        source_final_owner=values[14],
        source_physics_context=values[15],
        source_native_body=values[16],
        cycle_tid=cycle_tid,
        commit_tid=commit_tid,
        events=events,
        completion_before=completion_before,
        completion_after=completion_after,
        callback_flags=callback_flags,
        c98_pair_after=values[30],
        c9c_pair_after=values[31],
        anchor_transform=values[32],
        anchor_linear=values[33],
        frame0_transform=values[34],
        frame0_linear=values[35],
    )


def encode_anchor(anchor: NaturalPrerollAnchorV1) -> bytes:
    return _ANCHOR.pack(
        MAGIC,
        VERSION,
        SIZE,
        anchor.flags,
        anchor.fixed_interval_us,
        anchor.frame_count,
        0,
        anchor.build_id,
        bytes(4),
        anchor.recording_sha256,
        anchor.report_sha256,
        anchor.source_pid,
        anchor.source_library_base,
        anchor.source_main_object,
        anchor.source_final_owner,
        anchor.source_physics_context,
        anchor.source_native_body,
        anchor.cycle_tid,
        anchor.commit_tid,
        *anchor.events,
        anchor.completion_before,
        anchor.completion_after,
        anchor.callback_flags,
        bytes(6),
        anchor.c98_pair_after,
        anchor.c9c_pair_after,
        anchor.anchor_transform,
        anchor.anchor_linear,
        anchor.frame0_transform,
        anchor.frame0_linear,
    )


def _sha256(path: Path) -> bytes:
    return hashlib.sha256(path.read_bytes()).digest()


def anchor_match_ratios(current: bytes, anchor: NaturalPrerollAnchorV1) -> tuple[float, ...]:
    if len(current) != 76:
        raise ValueError("current physics payload must be exactly 64+12 bytes")
    current_values = struct.unpack("<19f", current)
    anchor_values = struct.unpack("<19f", anchor.anchor_transform + anchor.anchor_linear)
    frame0_values = struct.unpack("<19f", anchor.frame0_transform + anchor.frame0_linear)
    ratios: list[float] = []
    for index, value in enumerate(current_values):
        if not math.isfinite(value):
            ratios.append(math.inf)
            continue
        natural_step = abs(frame0_values[index] - anchor_values[index])
        floor = 0.01 if index < 16 else 0.5
        tolerance = max(floor, natural_step * 2.0 + 0.001)
        ratios.append(abs(value - anchor_values[index]) / tolerance)
    return tuple(ratios)


def anchor_matches(current: bytes, anchor: NaturalPrerollAnchorV1) -> bool:
    return max(anchor_match_ratios(current, anchor)) <= 1.0


def verify_source_binding(
    anchor: NaturalPrerollAnchorV1, report_path: Path, recording_path: Path
) -> None:
    report_blob = report_path.read_bytes()
    recording_blob = recording_path.read_bytes()
    if report_blob[:8] == ACTION_RELEASE_MAGIC:
        report = decode_action_until_release_report(report_blob).report
    elif report_blob[:8] == ACTION_REPORT_MAGIC:
        report = decode_action_window_report(report_blob)
    elif report_blob[:8] == BRAKE_REPORT_MAGIC:
        report = decode_sync_brake_report(report_blob)
    else:
        report = decode_sync_report(report_blob)
    fixed_interval_us, frames = decode_recording(recording_blob)
    if len(frames) != anchor.frame_count:
        raise ValueError("A9NPA1 frame count differs from the synchronized source")
    if fixed_interval_us != anchor.fixed_interval_us:
        raise ValueError("A9NPA1 fixed interval differs from the synchronized source")
    if anchor.build_id != SUPPORTED_BUILD_ID or report.pid != anchor.source_pid:
        raise ValueError("A9NPA1 build/PID differs from the synchronized source")
    identities = (
        report.library_base,
        report.main_object,
        report.final_owner,
        report.physics_context,
        report.native_body,
    )
    expected = (
        anchor.source_library_base,
        anchor.source_main_object,
        anchor.source_final_owner,
        anchor.source_physics_context,
        anchor.source_native_body,
    )
    if identities != expected:
        raise ValueError("A9NPA1 object identity differs from the synchronized source")
    frame0 = frames[0]
    if (
        frame0.transform != anchor.frame0_transform
        or frame0.linear_velocity != anchor.frame0_linear
    ):
        raise ValueError("A9NPA1 frame-0 payload differs from A9UTK1")
    if anchor.recording_sha256 != hashlib.sha256(recording_blob).digest():
        raise ValueError("A9NPA1 A9UTK1 SHA-256 mismatch")
    if anchor.report_sha256 != hashlib.sha256(report_blob).digest():
        raise ValueError("A9NPA1 A9USR1 SHA-256 mismatch")


def bind_anchor(
    raw_path: Path, report_path: Path, recording_path: Path, output_path: Path
) -> NaturalPrerollAnchorV1:
    raw = decode_anchor(raw_path.read_bytes(), require_bound=False)
    bound = replace(
        raw,
        flags=BOUND_FLAGS,
        recording_sha256=_sha256(recording_path),
        report_sha256=_sha256(report_path),
    )
    # Verify every source relationship before making the bound artifact visible.
    verify_source_binding(bound, report_path, recording_path)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    fd = os.open(output_path, flags, 0o600)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(encode_anchor(bound))
            stream.flush()
    except BaseException:
        output_path.unlink(missing_ok=True)
        raise
    return bound


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    bind = subparsers.add_parser("bind")
    bind.add_argument("raw_anchor", type=Path)
    bind.add_argument("source_report", type=Path)
    bind.add_argument("recording", type=Path)
    bind.add_argument("output", type=Path)
    verify = subparsers.add_parser("verify")
    verify.add_argument("anchor", type=Path)
    verify.add_argument("source_report", type=Path)
    verify.add_argument("recording", type=Path)
    args = parser.parse_args()
    if args.command == "bind":
        anchor = bind_anchor(
            args.raw_anchor, args.source_report, args.recording, args.output
        )
    else:
        anchor = decode_anchor(args.anchor.read_bytes())
        verify_source_binding(anchor, args.source_report, args.recording)
    print(
        "a9npa1_supported=1 "
        f"frames={anchor.frame_count} fixed={anchor.fixed_interval_us} "
        f"events={anchor.events[0]}..{anchor.events[-1]} "
        f"threads={anchor.cycle_tid}/{anchor.commit_tid}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
