#!/usr/bin/env python3
"""Project one strict A9UTK1 recording into an integrity-bound A9FWT1 blob."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import sys

from unified_tick_recording_v1 import (
    SKIP_TRANSFORM,
    SUPPORTED_BUILD_ID,
    decode_recording,
)


MAGIC = b"A9FWT1\0\0"
VERSION = 1
HEADER_SIZE = 128
RECORD_SIZE = 80
MAXIMUM_FRAMES = 3600
REQUIRED_INTERVAL_US = 16667
FLAG_SOURCE_SHA256 = 1 << 0
FLAG_ALL_OR_NOTHING_64_12 = 1 << 1
REQUIRED_FLAGS = FLAG_SOURCE_SHA256 | FLAG_ALL_OR_NOTHING_64_12

_HEADER = struct.Struct("<8s8I32s20s36s")
_TARGET = struct.Struct("<64s12sI")

assert _HEADER.size == HEADER_SIZE
assert _TARGET.size == RECORD_SIZE


def encode_target_blob(recording: bytes) -> bytes:
    fixed_interval_us, frames = decode_recording(recording)
    if not 2 <= len(frames) <= MAXIMUM_FRAMES:
        raise ValueError(f"final-writer frame count must be in 2..{MAXIMUM_FRAMES}")
    if fixed_interval_us != REQUIRED_INTERVAL_US:
        raise ValueError(
            f"final-writer interval must be exactly {REQUIRED_INTERVAL_US} us"
        )
    for index, frame in enumerate(frames):
        if frame.skip_flags & SKIP_TRANSFORM:
            raise ValueError(f"frame {index}: transform correction is disabled")
    source_hash = hashlib.sha256(recording).digest()
    output = bytearray(
        _HEADER.pack(
            MAGIC,
            VERSION,
            HEADER_SIZE,
            RECORD_SIZE,
            len(frames),
            fixed_interval_us,
            REQUIRED_FLAGS,
            len(recording),
            0,
            source_hash,
            SUPPORTED_BUILD_ID,
            bytes(36),
        )
    )
    for frame in frames:
        output += _TARGET.pack(frame.transform, frame.linear_velocity, 0)
    return bytes(output)


def decode_target_blob(blob: bytes, *, expected_recording: bytes | None = None):
    if len(blob) < HEADER_SIZE:
        raise ValueError("target blob is shorter than its header")
    values = _HEADER.unpack_from(blob)
    (magic, version, header_size, record_size, frame_count,
     fixed_interval_us, flags, source_size, reserved_u32,
     source_hash, build_id, reserved) = values
    if magic != MAGIC or version != VERSION:
        raise ValueError("unsupported A9FWT1 magic/version")
    if header_size != HEADER_SIZE or record_size != RECORD_SIZE:
        raise ValueError("A9FWT1 ABI size mismatch")
    if not 2 <= frame_count <= MAXIMUM_FRAMES:
        raise ValueError("invalid A9FWT1 frame count")
    if fixed_interval_us != REQUIRED_INTERVAL_US or flags != REQUIRED_FLAGS:
        raise ValueError("unsupported A9FWT1 timing/semantics")
    if source_size < 1 or reserved_u32 != 0 or reserved != bytes(36):
        raise ValueError("noncanonical A9FWT1 header")
    if build_id != SUPPORTED_BUILD_ID:
        raise ValueError("A9FWT1 build ID mismatch")
    expected_size = HEADER_SIZE + frame_count * RECORD_SIZE
    if len(blob) != expected_size:
        raise ValueError(f"A9FWT1 length must be exactly {expected_size} bytes")
    if expected_recording is not None:
        if source_size != len(expected_recording):
            raise ValueError("A9FWT1 source size mismatch")
        if source_hash != hashlib.sha256(expected_recording).digest():
            raise ValueError("A9FWT1 source SHA-256 mismatch")
    targets = []
    cursor = HEADER_SIZE
    for index in range(frame_count):
        transform, linear, frame_reserved = _TARGET.unpack_from(blob, cursor)
        if frame_reserved != 0:
            raise ValueError(f"target {index}: reserved field is nonzero")
        targets.append((transform, linear))
        cursor += RECORD_SIZE
    return {
        "frame_count": frame_count,
        "fixed_interval_us": fixed_interval_us,
        "source_size": source_size,
        "source_sha256": source_hash,
        "targets": tuple(targets),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        recording = args.recording.read_bytes()
        encoded = encode_target_blob(recording)
        decoded = decode_target_blob(encoded, expected_recording=recording)
        args.output.write_bytes(encoded)
    except (OSError, ValueError, struct.error) as error:
        print(f"a9fwt1_error={error}", file=sys.stderr)
        return 1
    print(
        f"A9FWT1_TARGET_BLOB passed=1 frames={decoded['frame_count']} "
        f"fixed_interval_us={decoded['fixed_interval_us']} "
        f"source_sha256={decoded['source_sha256'].hex()} device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
