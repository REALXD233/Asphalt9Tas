#!/usr/bin/env python3
"""Static policy for the review-only lifecycle barrel recorder variant."""

from __future__ import annotations

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: policy.py SOURCE BINARY")
    source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    binary = pathlib.Path(sys.argv[2]).read_bytes()
    required_source = (
        "A9TAS_BARREL_CAPTURE_V1",
        "kBarrelRbxOwnerOffset = 0x1968",
        "backend.angular_source_base + kBarrelRbxOwnerOffset",
        "backend.native_angular_address",
        "frame.barrel_angular_velocity",
        "frame.barrel_rbx",
        "kSyncRecordedSkipFlags == 0x48",
        "A9TAS_COMPLETION_WRITE_CERTIFICATE_V1",
        "kSyncCompletionWriteCertificate = 1u << 11",
        "completion_write_observed",
        "CompletionInputDr7()",
        "completion_write_not_observed",
        "pending_audit.reserved[0] = 1",
        "!ReadExact(mem, backend.native_angular_address",
    )
    required_binary = (
        b"A9USR6",
        b"I_ACCEPT_SYNC_RECORDER_NATURAL_ACTION_V1",
        b"NATURAL_ACTION_RECORDING_DONE",
    )
    if not all(token in source for token in required_source):
        raise SystemExit("lifecycle barrel source policy failed: source")
    if not all(token in binary for token in required_binary):
        raise SystemExit("lifecycle barrel source policy failed: binary")
    print(
        "LIFECYCLE_BARREL_SOURCE_POLICY passed=1 "
        "barrel_angular_read=1 barrel_rbx_read=1 game_writes_added=0 "
        "completion_write_certificate=1 deployed=0 device_access=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
