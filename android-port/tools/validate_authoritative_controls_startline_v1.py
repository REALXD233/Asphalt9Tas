#!/usr/bin/env python3
"""Strict A9AST1 + A9UTK1 validator for direct steering+brake replay."""
from __future__ import annotations

import argparse
import math
import pathlib
import struct
import sys

from unified_tick_recording_v1 import (
    SKIP_ACCELERATOR,
    SKIP_BARREL_ANGULAR,
    SKIP_BARREL_RBX,
    SKIP_NITRO,
    SKIP_RESPAWN,
    UnifiedTickFrameV1,
    decode_recording,
    encode_recording,
)
from validate_authoritative_steering_v1 import (
    BUILD_ID,
    FIXED,
    FLAGS,
    FRAME,
    HEADER,
    MAGIC,
    selftest_blob,
)

CONTROLS_SKIP = (
    SKIP_NITRO | SKIP_ACCELERATOR | SKIP_BARREL_ANGULAR |
    SKIP_BARREL_RBX | SKIP_RESPAWN
)


def require(value: bool, message: str) -> None:
    if not value:
        raise ValueError(message)


def raw_bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def validate(report_blob: bytes, recording_blob: bytes) -> dict[str, int]:
    fixed, recording = decode_recording(recording_blob)
    require(fixed == FIXED, "recording fixed interval")
    require(len(report_blob) >= HEADER.size, "short report header")
    h = HEADER.unpack_from(report_blob)
    require((h[0], h[1], h[2], h[3]) == (MAGIC, 1, HEADER.size, FRAME.size), "report ABI")
    require(h[4] == FLAGS and h[5] == BUILD_ID and h[6] == 0, "report identity/flags")
    require(all(h[index] != 0 for index in range(7, 20)), "zero runtime identity/address")
    require(h[15] == h[14] + 4, "C9C is not C98+4")
    frames = h[38]
    bound = h[39]
    require(frames == bound == len(recording), "recording/report frame count")
    require(len(report_blob) == HEADER.size + frames * FRAME.size, "report length")
    require((h[21], h[22], h[41]) == (1, 0, 0), "direct startline handoff counters")
    require((h[23], h[24], h[25]) == (0, 0, 0), "runtime errors")
    require((h[26], h[27], h[28]) == (frames, frames, 0), "delta counters")
    require((h[29], h[30], h[31]) == (frames * 2, frames * 2, 0), "pair counters")
    require((h[32], h[33]) == (0, 0), "forbidden action/correction")
    require(h[36] == h[35] + h[34], "thread detach accounting")
    require(h[37] == FIXED and h[42] == 0, "report shape")

    nonzero = 0
    last_world = 0
    brake_nonzero = 0
    for index, packet in enumerate(recording):
        require(packet.tick == index, f"recording tick {index}")
        require(packet.skip_flags == CONTROLS_SKIP, f"recording scope {index}")
        require(math.isfinite(packet.steering) and abs(packet.steering) <= 1.0, f"steering {index}")
        require(math.isfinite(packet.brake) and abs(packet.brake) <= 1.05, f"brake {index}")
        require(raw_bits(packet.accelerator) == 0, f"accelerator {index}")
        require(packet.nitro_activations == 0 and not packet.respawn, f"action {index}")
        require(all(raw_bits(value) == 0 for value in packet.barrel_angular + packet.barrel_rbx), f"barrel {index}")

        audit = FRAME.unpack_from(report_blob, HEADER.size + index * FRAME.size)
        require((audit[0], audit[1]) == (index, index), f"audit tick {index}")
        require(audit[2] > 0 and audit[3] > 0 and audit[2] != audit[3], f"thread {index}")
        require(0 < audit[6] <= 1_000_000 and audit[7] == FIXED, f"delta {index}")
        delta, c98, c9c, prefix, f64, callback, deferred, world = audit[8:16]
        require(delta < c98 < c9c == prefix < f64 < callback < deferred < world, f"event order {index}")
        require(world > last_world, f"world monotonic {index}")
        last_world = world
        require(audit[16] != audit[17] and (audit[18] & 0xFF) == 1 and audit[19] == 0, f"prefix proof {index}")

        steering_bits = raw_bits(packet.steering)
        brake_bits = raw_bits(packet.brake)
        expected_pair = (steering_bits << 32) | brake_bits
        require(audit[4] == steering_bits, f"steering audit {index}")
        for base in (20, 23):
            _, intended, after = audit[base:base + 3]
            require(intended == after == expected_pair, f"control pair {index}/{base}")
        require(audit[21] == audit[24], f"C98/C9C pair divergence {index}")
        nonzero += steering_bits != 0
        brake_nonzero += brake_bits != 0

    require(nonzero == h[40] and h[20] >= last_world, "summary counters")
    return {
        "frames": frames,
        "delta_writes": h[27],
        "pair_writes": h[30],
        "nonzero_steering": nonzero,
        "nonzero_brake": brake_nonzero,
    }


def make_selftest() -> tuple[bytes, bytes]:
    frame_count = 7
    report = bytearray(selftest_blob(frame_count))
    packets: list[UnifiedTickFrameV1] = []
    for index in range(frame_count):
        audit = list(FRAME.unpack_from(report, HEADER.size + index * FRAME.size))
        steering = struct.unpack("<f", struct.pack("<I", audit[4]))[0]
        brake = -1.0 if 2 <= index <= 4 else 0.0
        brake_bits = raw_bits(brake)
        pair = (audit[4] << 32) | brake_bits
        audit[21] = audit[22] = pair
        audit[24] = audit[25] = pair
        report[HEADER.size + index * FRAME.size:HEADER.size + (index + 1) * FRAME.size] = FRAME.pack(*audit)
        packets.append(UnifiedTickFrameV1(
            tick=index,
            monotonic_ns=index + 1,
            steering=steering,
            brake=brake,
            accelerator=0.0,
            nitro_activations=0,
            skip_flags=CONTROLS_SKIP,
            respawn=False,
            barrel_angular=(0.0, 0.0, 0.0),
            barrel_rbx=(0.0, 0.0),
            transform=bytes(64),
            linear_velocity=bytes(12),
        ))
    header = list(HEADER.unpack_from(report))
    header[21], header[22], header[41] = 1, 0, 0
    report[:HEADER.size] = HEADER.pack(*header)
    return bytes(report), encode_recording(packets, fixed_interval_us=FIXED)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", nargs="?", type=pathlib.Path)
    parser.add_argument("recording", nargs="?", type=pathlib.Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        report, recording = make_selftest()
        print("A9AST1_CONTROLS_VALID passed=1", validate(report, recording))
        return 0
    if args.report is None or args.recording is None:
        parser.error("report and recording are required")
    print("A9AST1_CONTROLS_VALID passed=1", validate(
        args.report.read_bytes(), args.recording.read_bytes()))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error) as error:
        print(f"A9AST1_CONTROLS_VALID passed=0 error={error}", file=sys.stderr)
        raise SystemExit(1)
