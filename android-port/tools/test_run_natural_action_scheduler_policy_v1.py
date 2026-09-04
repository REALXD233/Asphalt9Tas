#!/usr/bin/env python3
"""Static policy for the opt-in one-activation lifecycle runner branch."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNNER = ROOT / "run-natural-action-lifecycle-v1.ps1"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    text = RUNNER.read_text(encoding="utf-8")
    for token in (
        "[switch]$ActionSchedulerReview",
        "[switch]$AcknowledgeExactlyOneNaturalNitroActivation",
        "[switch]$AcknowledgeRacePausedNitroIdleUsable",
        '"one_natural_nitro_activation"',
        "liba9tas_natural_action_scheduler_v1_review_only.so",
        "natural_action_scheduler_candidate_v1",
        "liba9tas_bootstrap_nas_v1.so",
        "validate_natural_action_scheduler_report_v1.py",
            "d718a13578e7e37a3d2a0240f94b125620858d9657d69a4fc799b8c5263b7f9e",
            "bf2b772dcffd8346279193d56bc196e23b35a029aa7358b5834b62d5c6410fe7",
        "fc8e977465d967e92f5c1b1b3908c340105e5c96a0a5982cfeb7506cfb8f5fb4",
            "60985bf0bcc02861a4c1e95fb678f0912bb894c214d2aa8fb354401b03dbe9c7",
        "I_ACCEPT_ONE_ACTION_LIFECYCLE_REVIEW_V1",
        "exactly one natural producer-thread Nitro activation",
        "prepared.semantics -ne $semantics",
        "action_receipt=1 activation_count=1",
        "Assert-StableCleanTracer $gamePid",
        "$consecutive -ge 8",
        "$readyPath $armPath $ack",
        "input keyevent 111 && echo NAL_ARM",
        "sending_single_ESC_then_ARM=1",
        "rm -f $armPath",
        "bootstrap_verified=1 status=1 stage=2",
        "$proofPid",
        "PID-bound mappings and TracerPid=0",
        "$null -ne $startInfo.ArgumentList",
        "$startInfo.Arguments =",
    ):
        require(token in text, f"one-action runner token missing: {token}")
    action_branch = text[text.index("if ($ActionSchedulerReview) {"):
                         text.index("} else {", text.index("if ($ActionSchedulerReview) {"))]
    require("AcknowledgeZeroActionAndNitroCalls" not in action_branch,
            "one-action artifact branch must not use zero-call acknowledgement")
    require("$mutationRisk = $true" in text and
            "-not $passed -and $mutationRisk" in text and
            "Force-StopGame" in text,
            "post-ESC action failure must force-stop the exact fresh process")
    require("OfflineValidateOnly" in text and "device_access=0 deployed=0" in text,
            "offline validation disposition missing")
    print("NATURAL_ACTION_SCHEDULER_RUNNER_POLICY passed=1 opt_in=1 "
          "activation_count=1 mixed_receipt_rejected=1 "
          "post_esc_force_stop=1 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
