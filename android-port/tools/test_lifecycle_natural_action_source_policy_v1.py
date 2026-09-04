#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: policy.py SOURCE BINARY")
    source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
    binary = pathlib.Path(sys.argv[2])
    binary_data = binary.read_bytes()
    required_source = (
        "natural_action_recording_host_v1::Stage",
        "natural_action_recording_host_v1::ArmFirstFrame",
        "natural_action_recording_host_v1::CommitFrame",
        "natural_action_recording_host_v1::Finish",
        "frames[index].nitro_activation_count = action_result.counts[index]",
        "~a9tas::unified_tick_v1::kSkipNitroActivation",
        "action_result.count_sum > 0",
    )
    required_strings = (
        "A9USR6",
        "I_ACCEPT_SYNC_RECORDER_NATURAL_ACTION_V1",
        "NATURAL_ACTION_RECORDING_DONE",
    )
    forbidden = ("NitroState", "kNalNitroStateOffset", "mode forcing")
    ok = all(item in source for item in required_source)
    ok = ok and all(item.encode("ascii") in binary_data
                    for item in required_strings)
    ok = ok and not any(item in source for item in forbidden)
    if not ok:
        raise SystemExit("natural-action lifecycle source policy failed")
    print("LIFECYCLE_NATURAL_ACTION_SOURCE_POLICY passed=1 "
          "natural_calls=1 frame_bound=1 merge=0 mode_forcing=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
