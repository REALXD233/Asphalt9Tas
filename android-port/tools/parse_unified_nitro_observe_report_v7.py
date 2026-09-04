#!/usr/bin/env python3
"""Strict A9UER7 verifier for the one-frame pre-physics nitro observe gate."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from parse_unified_executor_report_v2 import _HEADER as _HEADER_V5
from parse_unified_executor_report_v5 import _FRAME as _FRAME_V5, decode_report as decode_v5


MAGIC = b"A9UER7\0\0"
VERSION = 7
HEADER_SIZE = 320
FRAME_SIZE = 1940
NITRO_RPC_USED = 1 << 5
NITRO_RPC_OBSERVED = 1 << 10
NITRO_ACTIVATED = 1 << 11
NITRO_RESPONSE_MAGIC = b"A9NRS1\0\0"

# The V5 header ends with 29 uint64 fields followed by the two uint32 thread
# counters. V7 appends its three uint64 RPC counters after those thread fields.
# Keeping this order explicit matters: ``32Q2I`` has the same total size but
# silently reads initial_threads/final_threads as part of the first RPC counter.
_HEADER = struct.Struct("<8s6I20sI29Q2I3Q")
_FRAME = struct.Struct("<QQiIqq7Q2QHi2sI4sQQ64s12sQII96s804s804s")
_RESPONSE = struct.Struct("<8sIIQiIQQQQBB5sBIIBB5sBII")
assert _HEADER.size == HEADER_SIZE
assert _FRAME.size == FRAME_SIZE
assert _RESPONSE.size == 96


def decode_report(blob: bytes):
    if len(blob) < HEADER_SIZE:
        raise ValueError("report is shorter than A9UER7 header")
    header = list(_HEADER.unpack_from(blob))
    if header[0] != MAGIC or header[1] != VERSION:
        raise ValueError("unsupported A9UER7 magic/version")
    if header[2] != HEADER_SIZE or header[3] != FRAME_SIZE:
        raise ValueError("A9UER7 ABI size mismatch")
    frame_count = header[5]
    if frame_count != 1 or len(blob) != HEADER_SIZE + FRAME_SIZE:
        raise ValueError("nitro observe gate must contain exactly one frame")
    if not header[4] & NITRO_RPC_USED:
        raise ValueError("A9UER7 nitro RPC header flag is missing")
    # The three counters are appended after the complete V5 header.
    requests, activation_calls, failures = header[40:43]
    if (requests, activation_calls, failures) != (1, 0, 0):
        raise ValueError("nitro observe gate RPC counters mismatch")

    frame = list(_FRAME.unpack_from(blob, HEADER_SIZE))
    flags = frame[3]
    nitro_phase_event, requested, reserved = frame[24:27]
    response = _RESPONSE.unpack(frame[27])
    if not flags & NITRO_RPC_OBSERVED or flags & NITRO_ACTIVATED:
        raise ValueError("nitro observe frame flags mismatch")
    if nitro_phase_event == 0 or nitro_phase_event != frame[6]:
        raise ValueError("nitro RPC was not bound to the fixed-delta event")
    if requested != 0 or reserved != 0:
        raise ValueError("nitro observe request/reserved mismatch")
    (
        magic,
        version,
        size,
        sequence,
        result,
        calls,
        guest_base,
        vehicle_owner,
        service,
        dispatch,
        before_optional,
        before_active,
        before_gates,
        before_reserved,
        before_optional_value,
        before_mode,
        after_optional,
        after_active,
        after_gates,
        after_reserved,
        after_optional_value,
        after_mode,
    ) = response
    if (magic, version, size, result, calls) != (
        NITRO_RESPONSE_MAGIC,
        1,
        96,
        0,
        0,
    ) or sequence == 0:
        raise ValueError("nitro observe response protocol mismatch")
    if guest_base != header[10] or vehicle_owner != header[12]:
        raise ValueError("nitro observe response identity mismatch")
    if service == 0 or dispatch == 0:
        raise ValueError("nitro observe response object chain is incomplete")
    before = (
        before_optional,
        before_active,
        before_gates,
        before_reserved,
        before_optional_value,
        before_mode,
    )
    after = (
        after_optional,
        after_active,
        after_gates,
        after_reserved,
        after_optional_value,
        after_mode,
    )
    if before_reserved != 0 or after_reserved != 0 or before != after:
        raise ValueError("observe-only RPC changed nitro state")

    # Strip V7-only counters/audit and reuse the mature A9UER5 structural
    # verifier for the certified delta -> C98 -> C9C -> final -> commit chain.
    old_header = header[:40]
    old_header[0], old_header[1], old_header[2], old_header[3] = (
        b"A9UER5\0\0",
        5,
        _HEADER_V5.size,
        _FRAME_V5.size,
    )
    old_header[4] &= ~NITRO_RPC_USED
    old_frame = frame[:24] + frame[28:]
    old_frame[3] &= ~(NITRO_RPC_OBSERVED | NITRO_ACTIVATED)
    return decode_v5(_HEADER_V5.pack(*old_header) + _FRAME_V5.pack(*old_frame))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    try:
        summary = decode_report(args.report.read_bytes())
    except (OSError, ValueError, struct.error) as error:
        print(f"a9uer7_nitro_observe_error={error}")
        return 1
    print(
        f"a9uer7_nitro_observe_supported=1 frames={summary.frames} "
        f"delta_writes={summary.delta_writes} activation_calls=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
