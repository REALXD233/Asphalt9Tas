#!/usr/bin/env python3
"""Cross-artifact policy for the unlinked one-activation controller."""

from __future__ import annotations

import hashlib
import pathlib
import re
import subprocess
import sys


EXPECTED_PAYLOAD_SHA256 = (
    "d718a13578e7e37a3d2a0240f94b125620858d9657d69a4fc799b8c5263b7f9e"
)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def output(*args: str) -> str:
    return subprocess.check_output(args, text=True, errors="replace")


def main() -> int:
    if len(sys.argv) != 10:
        raise SystemExit(
            "usage: audit... OBJECT DISASM PAYLOAD READELF SOURCE REPORT_HEADER "
            "VALIDATOR BUILD OUT_DIR"
        )
    object_path, disasm_path, payload, readelf, source, report_header, validator, build, out_dir = (
        pathlib.Path(value) for value in sys.argv[1:]
    )
    for path in (object_path, disasm_path, payload, readelf, source,
                 report_header, validator, build):
        require(path.is_file(), f"missing audit input: {path}")
    require(out_dir.is_dir(), f"missing output directory: {out_dir}")
    require(hashlib.sha256(payload.read_bytes()).hexdigest() ==
            EXPECTED_PAYLOAD_SHA256, "action payload identity drift")

    header = output(str(readelf), "-h", str(object_path))
    require("Advanced Micro Devices X86-64" in header and
            re.search(r"Type:\s+REL", header),
            "controller review artifact must be an unlinked x86-64 object")
    produced_files = {item.name for item in out_dir.iterdir() if item.is_file()}
    require(produced_files <= {object_path.name, disasm_path.name},
            f"linked or unexpected controller artifact exists: {produced_files}")

    source_text = source.read_text(encoding="utf-8")
    report_text = report_header.read_text(encoding="utf-8")
    validator_text = validator.read_text(encoding="utf-8")
    build_text = build.read_text(encoding="utf-8")
    for token in (
        "A9TAS_NAL_ACTION_CONTROLLER_REVIEW",
        "A9TAS_NAL_ACTION_PAYLOAD_REVIEW",
        "natural_action_scheduler_report_v1.h",
        "I_ACCEPT_ONE_ACTION_LIFECYCLE_REVIEW_V1",
        "command.nitro_activations = 1",
        "natural_mailbox::kNitroOverrideEnabled",
        "offsetof(NalEvidence, action_command_completions)",
        "evidence.action_command_completions == 1",
        "evidence.action_calls_submitted == 1",
        "mailbox.calls_submitted == 1",
        "report.action_receipt_ns",
        "nal_report::kActionReceipt",
    ):
        require(token in source_text, f"action controller token missing: {token}")
    for token in (
        "kActionReceipt", "action_command_completions",
        "action_calls_submitted", "action_queue_end_before",
        "action_queue_end_after", "action_receipt_ns", "action_frames",
        "sizeof(Report) == 664",
    ):
        require(token in report_text, f"action report ABI token missing: {token}")
    for token in (EXPECTED_PAYLOAD_SHA256, "A9NASR1", "zero_call_completions == 0",
                  "action_completions == action_calls == 1",
                  "queue_before != 0 and queue_after != 0"):
        require(token in validator_text, f"action report validator token missing: {token}")
    require('"-DA9TAS_NAL_ACTION_CONTROLLER_REVIEW=1"' in build_text and
            '"-c"' in build_text and "linked_live=0" in build_text,
            "controller action branch must remain unlinked")
    require("adb" not in build_text.lower() and "device_access=0" in build_text,
            "controller build must remain device-free")

    object_bytes = object_path.read_bytes()
    require(b"I_ACCEPT_ONE_ACTION_LIFECYCLE_REVIEW_V1" in object_bytes,
            "action acknowledgement absent from object")
    require(b"liba9tas_natural_action_scheduler_v1_review_only.so" in object_bytes,
            "action payload basename absent from object")
    require(b"liba9tas_natural_action_callback_lifecycle_v1_build_only.so" not in object_bytes,
            "passive payload identity leaked into action object")
    disasm = disasm_path.read_text(encoding="utf-8", errors="replace")
    require("PublishZeroCall" in disasm and "PollForZeroReceipt" in disasm,
            "controller action bodies absent from disassembly")

    print("NATURAL_ACTION_SCHEDULER_CONTROLLER_AUDIT passed=1 "
          "activation_count=1 action_receipt=1 payload_pinned=1 "
          "review_unlinked=1 runner=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
