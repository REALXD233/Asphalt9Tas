#!/usr/bin/env python3
"""Offline identity proof for the action-review lifecycle payload resolver."""

from __future__ import annotations

import hashlib
import pathlib
import sys

import test_natural_action_lifecycle_elf_resolver_v1 as passive_test


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "natural_action_lifecycle_elf_resolver_v1.h"
DEFAULT_PAYLOAD = (
    ROOT / "build" / "natural-action-scheduler-payload-v1" /
    "liba9tas_natural_action_scheduler_v1_review_only.so"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    payload = pathlib.Path(sys.argv[1]) if len(sys.argv) == 2 else DEFAULT_PAYLOAD
    if len(sys.argv) > 2:
        raise SystemExit(f"usage: {sys.argv[0]} [action_payload]")
    text = HEADER.read_text(encoding="utf-8")
    for token in (
        "A9TAS_NAL_ACTION_PAYLOAD_REVIEW",
        "liba9tas_natural_action_scheduler_v1_review_only.so",
        "kExpectedActionSha256",
        "kExpectedActionBuildId",
        "kExpectedSha256 = kExpectedActionSha256",
        "kExpectedBuildId = kExpectedActionBuildId",
    ):
        require(token in text, f"action resolver identity missing: {token}")
    data, _, _, _, _, build_ids = passive_test.parse_elf(payload)
    expected_hash = passive_test.c_bytes(text, "kExpectedActionSha256", 32)
    expected_build_id = passive_test.c_bytes(
        text, "kExpectedActionBuildId", 20
    )
    require(hashlib.sha256(data).digest() == expected_hash,
            "action payload SHA-256 pin")
    require(build_ids == [expected_build_id], "action payload GNU Build ID pin")
    passive_test.verify_source_policy(text)
    print("NATURAL_ACTION_SCHEDULER_ELF_RESOLVER passed=1 sha256=1 "
          "build_id=1 basename=1 action_variant=1 guest_calls=0 "
          "device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
