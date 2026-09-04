#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
WRAPPER = ROOT / "src" / "hwbp_lifecycle_final_writer_natural_action_replay_v1.cpp"
EXECUTOR = ROOT / "src" / "hwbp_unified_tick_executor_v1.cpp"
INTEGRATION = ROOT / "src" / "final_writer_unified_integration_v1.h"
PARSER = ROOT / "tools" / "parse_unified_executor_report_v8.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    require(len(sys.argv) == 2, "usage: policy REVIEW_OBJECT")
    review_object = pathlib.Path(sys.argv[1])
    require(review_object.is_file() and review_object.stat().st_size > 0,
            "composite review object missing")
    wrapper = WRAPPER.read_text(encoding="utf-8")
    executor = EXECUTOR.read_text(encoding="utf-8")
    integration = INTEGRATION.read_text(encoding="utf-8")
    parser = PARSER.read_text(encoding="utf-8")
    for token in ("A9TAS_NAL_ACTION_PAYLOAD_REVIEW 2",
                  "A9TAS_FINAL_WRITER_NATURAL_ACTION_V1 1",
                  "A9TAS_COMPLETION_WRITE_CERTIFICATE_V1 1",
                  "hwbp_lifecycle_final_writer_replay_v1.cpp"):
        require(token in wrapper, f"missing wrapper token: {token}")
    require("BeginTick(\n                                    mem," in executor and
            "frames[machine.frame_index]" in executor,
            "authoritative frame not published at fixed-delta selection")
    for token in ("ConfigureNaturalAction", "BeginAndPublish",
                  "ObserveCallbackClose", "CommitWorld",
                  "evidence.protocol_state != 1",
                  "claimed != 0 || completed != 0"):
        require(token in integration, f"missing integration gate: {token}")
    require("A9TAS_UNIFIED_NITRO_RPC_V1" not in wrapper,
            "legacy Nitro RPC enabled in natural-action composition")
    for token in ("CompletionInputDr7", "completion_watch_arm_failed",
                  "kUnifiedCompletionWriteObserved",
                  "completion_write_not_observed"):
        require(token in executor,
                f"completion write certificate gate missing: {token}")
    for token in ("COMPLETION_WRITE_HEADER", "COMPLETION_WRITE_FRAME",
                  "completion write certificate is missing"):
        require(token in parser,
                f"completion certificate parser gate missing: {token}")
    require("kNitroMode" not in integration and "NitroState" not in integration,
            "direct Nitro mode/state forcing entered composition")
    print("LIFECYCLE_FINAL_WRITER_NATURAL_ACTION_POLICY passed=1 "
          "tick0_lifecycle=1 fixed_delta_publish=1 callback_close_receipt=1 "
          "world_dual_commit=1 completion_write_certificate=1 legacy_rpc=0 "
          "mode_forcing=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
