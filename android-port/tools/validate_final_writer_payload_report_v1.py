#!/usr/bin/env python3
"""Validate an A9FWR1 final-writer payload report against A9FWT1 targets."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import sys

import make_final_writer_target_blob_v1 as target_blob_v1


MAGIC = b"A9FWR1\0\0"
VERSION = 1
HEADER_SIZE = 368
AUDIT_SIZE = 160
REQUIRED_FLAGS = 0x1F
EVIDENCE_MAGIC = b"A9FWRE1\0"
EVIDENCE_SIZE = 192
MAXIMUM_FRAMES = 3600

AUDIT_ORIGINAL_RETURNED = 1 << 0
AUDIT_EQUAL = 1 << 1
AUDIT_CORRECTED = 1 << 2
AUDIT_IMMEDIATE_EXACT = 1 << 3
AUDIT_FINAL_FRAME = 1 << 4
AUDIT_VPTR_RESTORED = 1 << 5

_HEADER = struct.Struct("<8s6I10Q32s32s192s")
_AUDIT = struct.Struct("<II64s12s64s12s")
_EVIDENCE_PREFIX = struct.Struct("<8sII8QIi4Qq3Q")

assert _HEADER.size == HEADER_SIZE
assert _AUDIT.size == AUDIT_SIZE
assert _EVIDENCE_PREFIX.size == 152


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_report(report: bytes, target_blob: bytes,
                    payload: bytes | None = None) -> dict[str, int | str]:
    _require(len(report) >= HEADER_SIZE, "A9FWR1 is shorter than its header")
    values = _HEADER.unpack_from(report)
    (magic, version, header_size, audit_size, flags, frame_count, reserved0,
     pid, library_base, object_address, wrapper, shadow_vptr, original_vptr,
     control_address, target_address, audit_address, evidence_address,
     recording_sha256, payload_sha256, evidence_raw) = values
    _require(magic == MAGIC and version == VERSION,
             "unsupported A9FWR1 magic/version")
    _require(header_size == HEADER_SIZE and audit_size == AUDIT_SIZE,
             "A9FWR1 ABI size mismatch")
    _require(flags == REQUIRED_FLAGS and reserved0 == 0,
             "unsupported or noncanonical A9FWR1 header")
    _require(2 <= frame_count <= MAXIMUM_FRAMES,
             "invalid A9FWR1 frame count")
    _require(len(report) == HEADER_SIZE + frame_count * AUDIT_SIZE,
             "A9FWR1 length mismatch")
    for name, value in (
        ("pid", pid), ("library_base", library_base),
        ("object", object_address), ("wrapper", wrapper),
        ("shadow_vptr", shadow_vptr), ("original_vptr", original_vptr),
        ("control_address", control_address), ("target_address", target_address),
        ("audit_address", audit_address), ("evidence_address", evidence_address),
    ):
        _require(value != 0, f"A9FWR1 {name} is zero")
    _require(shadow_vptr != original_vptr,
             "shadow and original vptr must differ")

    decoded_target = target_blob_v1.decode_target_blob(target_blob)
    _require(decoded_target["frame_count"] == frame_count,
             "A9FWR1/A9FWT1 frame count mismatch")
    _require(decoded_target["source_sha256"] == recording_sha256,
             "A9FWR1/A9FWT1 recording SHA-256 mismatch")
    if payload is not None:
        _require(hashlib.sha256(payload).digest() == payload_sha256,
                 "payload SHA-256 mismatch")

    evidence_values = _EVIDENCE_PREFIX.unpack_from(evidence_raw)
    (evidence_magic, evidence_version, evidence_size,
     wrapper_entries, original_calls, clean_returns, equal_frames,
     corrected_frames, correction_writes, failures, recursive_entries,
     processed_frames, last_status, last_object, last_token,
     observed_vptr, final_vptr, _last_result,
     reserved1, reserved2, reserved3) = evidence_values
    _require(evidence_raw[_EVIDENCE_PREFIX.size:] == bytes(
        EVIDENCE_SIZE - _EVIDENCE_PREFIX.size), "nonzero Evidence tail padding")
    _require(evidence_magic == EVIDENCE_MAGIC and evidence_version == VERSION and
             evidence_size == EVIDENCE_SIZE, "Evidence ABI mismatch")
    _require(wrapper_entries == frame_count and original_calls == frame_count and
             clean_returns == frame_count and processed_frames == frame_count,
             "Evidence cursor/call totals are incomplete")
    _require(equal_frames + corrected_frames == frame_count,
             "Evidence equal/corrected partition mismatch")
    _require(correction_writes == corrected_frames * 2,
             "Evidence correction write count mismatch")
    _require(failures == 0 and recursive_entries == 0 and last_status == 1,
             "Evidence reports a payload failure")
    _require(last_object == object_address and last_token != 0 and
             observed_vptr == shadow_vptr and final_vptr == original_vptr,
             "Evidence identity or final restoration mismatch")
    _require(reserved1 == reserved2 == reserved3 == 0,
             "Evidence reserved fields are nonzero")

    counted_equal = 0
    counted_corrected = 0
    cursor = HEADER_SIZE
    for index, (target_transform, target_linear) in enumerate(
            decoded_target["targets"]):
        (frame_index, audit_flags, before_transform, before_linear,
         immediate_transform, immediate_linear) = _AUDIT.unpack_from(report, cursor)
        cursor += AUDIT_SIZE
        _require(frame_index == index, f"audit {index}: frame index mismatch")
        required = AUDIT_ORIGINAL_RETURNED | AUDIT_IMMEDIATE_EXACT
        _require((audit_flags & required) == required,
                 f"audit {index}: original/immediate proof missing")
        is_equal = bool(audit_flags & AUDIT_EQUAL)
        is_corrected = bool(audit_flags & AUDIT_CORRECTED)
        _require(is_equal != is_corrected,
                 f"audit {index}: equal/corrected partition invalid")
        _require(immediate_transform == target_transform and
                 immediate_linear == target_linear,
                 f"audit {index}: immediate bytes differ from A9FWT1")
        before_matches = (before_transform == target_transform and
                          before_linear == target_linear)
        _require(before_matches == is_equal,
                 f"audit {index}: before bytes disagree with audit class")
        is_final = index + 1 == frame_count
        _require(bool(audit_flags & AUDIT_FINAL_FRAME) == is_final and
                 bool(audit_flags & AUDIT_VPTR_RESTORED) == is_final,
                 f"audit {index}: final restoration flags invalid")
        known = (AUDIT_ORIGINAL_RETURNED | AUDIT_EQUAL | AUDIT_CORRECTED |
                 AUDIT_IMMEDIATE_EXACT | AUDIT_FINAL_FRAME |
                 AUDIT_VPTR_RESTORED)
        _require(audit_flags & ~known == 0,
                 f"audit {index}: unknown audit flags")
        counted_equal += int(is_equal)
        counted_corrected += int(is_corrected)
    _require(counted_equal == equal_frames and
             counted_corrected == corrected_frames,
             "audit totals differ from Evidence")
    return {
        "frames": frame_count,
        "equal": counted_equal,
        "corrected": counted_corrected,
        "recording_sha256": recording_sha256.hex(),
        "payload_sha256": payload_sha256.hex(),
    }


def _selftest() -> None:
    source_hash = hashlib.sha256(b"synthetic-source").digest()
    targets = (
        (bytes(range(64)), bytes(range(12))),
        (bytes(reversed(range(64))), bytes(reversed(range(12)))),
    )
    target = bytearray(target_blob_v1._HEADER.pack(
        target_blob_v1.MAGIC, target_blob_v1.VERSION,
        target_blob_v1.HEADER_SIZE, target_blob_v1.RECORD_SIZE, 2,
        target_blob_v1.REQUIRED_INTERVAL_US, target_blob_v1.REQUIRED_FLAGS,
        len(b"synthetic-source"), 0, source_hash,
        target_blob_v1.SUPPORTED_BUILD_ID, bytes(36)))
    for transform, linear in targets:
        target += target_blob_v1._TARGET.pack(transform, linear, 0)
    payload = b"synthetic-payload"
    shadow_vptr = 0x2200
    original_vptr = 0x1100
    evidence_prefix = _EVIDENCE_PREFIX.pack(
        EVIDENCE_MAGIC, 1, EVIDENCE_SIZE,
        2, 2, 2, 1, 1, 2, 0, 0,
        2, 1, 0x3300, 0x4400, shadow_vptr, original_vptr, 7,
        0, 0, 0)
    evidence = evidence_prefix + bytes(EVIDENCE_SIZE - len(evidence_prefix))
    header = _HEADER.pack(
        MAGIC, 1, HEADER_SIZE, AUDIT_SIZE, REQUIRED_FLAGS, 2, 0,
        123, 0x100000, 0x3300, 0x5500, shadow_vptr, original_vptr,
        0x6600, 0x7700, 0x8800, 0x9900,
        source_hash, hashlib.sha256(payload).digest(), evidence)
    audits = bytearray()
    audits += _AUDIT.pack(0, AUDIT_ORIGINAL_RETURNED | AUDIT_EQUAL |
                          AUDIT_IMMEDIATE_EXACT,
                          targets[0][0], targets[0][1],
                          targets[0][0], targets[0][1])
    changed = bytes([targets[1][0][0] ^ 1]) + targets[1][0][1:]
    audits += _AUDIT.pack(1, AUDIT_ORIGINAL_RETURNED | AUDIT_CORRECTED |
                          AUDIT_IMMEDIATE_EXACT | AUDIT_FINAL_FRAME |
                          AUDIT_VPTR_RESTORED,
                          changed, targets[1][1], targets[1][0], targets[1][1])
    encoded = header + audits
    result = validate_report(encoded, bytes(target), payload)
    _require(result["frames"] == 2 and result["corrected"] == 1,
             "selftest valid fixture rejected")
    corrupted = bytearray(encoded)
    corrupted[HEADER_SIZE + AUDIT_SIZE + 8 + 64 + 12] ^= 1
    try:
        validate_report(bytes(corrupted), bytes(target), payload)
    except ValueError:
        pass
    else:
        raise AssertionError("selftest corrupted immediate bytes accepted")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", nargs="?", type=pathlib.Path)
    parser.add_argument("target_blob", nargs="?", type=pathlib.Path)
    parser.add_argument("payload", nargs="?", type=pathlib.Path)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    try:
        if args.selftest:
            _selftest()
            print("A9FWR1_VALIDATOR_SELFTEST passed=1 device_access=0")
            return 0
        if args.report is None or args.target_blob is None:
            parser.error("report and target_blob are required without --selftest")
        result = validate_report(
            args.report.read_bytes(), args.target_blob.read_bytes(),
            args.payload.read_bytes() if args.payload is not None else None)
    except (OSError, ValueError, struct.error, AssertionError) as error:
        print(f"a9fwr1_error={error}", file=sys.stderr)
        return 1
    print("A9FWR1_VALID passed=1 " + " ".join(
        f"{key}={value}" for key, value in result.items()) +
          " original_first=1 immediate_exact=1 restored=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
