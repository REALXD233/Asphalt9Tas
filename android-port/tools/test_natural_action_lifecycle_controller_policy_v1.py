#!/usr/bin/env python3
"""Static policy for the build-only lifecycle controller foundation."""

from __future__ import annotations

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "natural_action_lifecycle_controller_v1.cpp"
FC2 = ROOT / "src" / "fc2_frame_callback_transaction_controller_v1.cpp"
BUILD = ROOT / "build-natural-action-lifecycle-controller-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    source = SOURCE.read_text(encoding="utf-8")
    fc2 = FC2.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8")
    for token in (
        "A9TAS_NAL_CONTROLLER_REVIEW", "A9TAS_FC2_CONTROLLER_NO_MAIN",
        "natural_action_lifecycle_elf_resolver_v1.h",
        "natural_action_lifecycle_report_v1.h",
        "natural_action_runtime_identity_v1.h", "NAL_ACTION_IDENTITY",
        "ResolveUniqueNalActionOwner", "ResolveNalNitroState",
        "PrepareLifecyclePayload", "InitialControl", "InitialEvidence",
        "producer_tid == 0", "control.expected_producer_tid = producer_tid",
        "PayloadWriteExact", "ArmZeroCallMailbox", "PublishZeroCall",
        "command.nitro_activations != 0", "published_selector",
        "RequestCleanRemoval", "remove_requested",
        "AttachLifecycleThreads", "NAL_ATTACH_RETIRED",
        "access(task_path, F_OK) != 0 && errno == ENOENT",
        "threads.size()) + retired",
        "next_rescan = MonotonicNs() + 250000000ULL",
        "NAL_OWNER_RESCAN added=",
        "RegistrationEvidenceComplete", "ZeroReceiptComplete",
        "RemovalEvidenceComplete", "WriteLifecycleReportExclusive",
        "Same proven rule as FC-1", "kOwnerRejectUnknownWait",
        "kOwnerRejectNonzeroExit", "kOwnerRejectNamePrefix",
        "kOwnerRejectPhysicsIdentity", "kOwnerRejectDeferred",
        "NAL_OWNER_REJECT reason=0x%x",
        "WaitForArmMarker", "NAL_ARM_ACCEPTED",
        "READY_MARKER ARM_MARKER ACK", "argc != 9",
        "WaitOwnerFlagTransition", "NAL_REGISTRATION_EDGE",
        "repeated_state_writes > 64",
        "ResumeNalWorkersExcept", "NAL_WORKERS_RELEASED",
        "Reuse the proven FC3 release order",
        "RestoreAndDetachNal", "NAL_CLEANUP_RETIRED",
        "NAL_REGISTRATION_PROOF", "if (evidence_read) report.evidence = evidence",
        "evidence.callback_entries == 0", "evidence.last_object == 0",
        "evidence.last_token == 0", "evidence.callback_entries >= frames",
        "evidence.last_object == dedicated_object", "evidence.last_token != 0",
        "0xA9E1000000000000ULL",
        "runtime=disabled return=-100 device_access=0",
    ):
        require(token in source, f"controller policy missing: {token}")
    for forbidden in (
        "SpoofCallToEnableNitroFunction", "NitroService", "dispatch_action",
        "0x367B414", "input keyevent", "socket(", "pthread_create",
    ):
        require(forbidden not in source,
                f"controller forbidden action path: {forbidden}")
    require("}\n    close(mem);\n    report.physics_context = context" not in source,
            "read-only process memory must not close after identity success")
    require("WaitForPositiveDelta(mem" in source and
            "report.resume_observed_ns = MonotonicNs();" in source,
            "read-only process memory must remain live through positive delta")
    deadline_bindings = (
        ("arm_deadline", "WaitForArmMarker"),
        ("prearm_deadline", "WaitForPositiveDelta"),
        ("owner_deadline", "FindInitialOwner"),
        ("registration_deadline", "WaitOwnerFlagTransition"),
        ("receipt_deadline", "PollForZeroReceipt"),
        ("removal_deadline", "PollForNaturalRemoval"),
    )
    require("const std::uint64_t timeout_ns" in source,
            "controller timeout budget missing")
    for deadline_name, consumer in deadline_bindings:
        declaration = re.search(
            rf"const\s+std::uint64_t\s+{deadline_name}\s*=\s*"
            rf"MonotonicNs\(\)\s*\+\s*timeout_ns;", source)
        declaration_at = declaration.start() if declaration else -1
        consumer_at = source.find(consumer, declaration_at)
        require(declaration_at >= 0 and consumer_at > declaration_at and
                deadline_name in source[consumer_at:consumer_at + 400],
                f"{consumer} must use a fresh {deadline_name}")
    arm_wait = source.index("WaitForArmMarker(argv[7]")
    delta_wait = source.index("WaitForPositiveDelta(mem")
    require(arm_wait < delta_wait,
            "exact ARM marker must be accepted before positive-delta observation")
    require(source.count("registration_deadline,") == 2 and
            source.count("WaitOwnerFlagTransition(") == 3,
            "both bounded registration transitions require one fresh phase budget")
    require("#if A9TAS_FC2_CONTROLLER_NO_MAIN == 0" in fc2 and
            "must be 0 or 1" in fc2,
            "FC-2 reusable no-main boundary missing")
    require("-DA9TAS_NAL_CONTROLLER_REVIEW=1" in build and
            '"-c"' in build and "review object" in build.lower(),
            "review branch must remain unlinked")
    require("device_access=0" in build and "adb" not in build.lower(),
            "controller build must be device-free")
    print("NATURAL_ACTION_LIFECYCLE_CONTROLLER_POLICY passed=1 "
          "fc1_fc2_reuse=1 payload_prepare=1 zero_publish=1 "
          "clean_remove=1 review_unlinked=1 live_runner=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
