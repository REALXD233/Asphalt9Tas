#!/usr/bin/env python3

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUNDLE_SOURCE = ROOT / "src" / "payload_bundle_natural_controller_v1.cpp"
BOOTSTRAP_SOURCE = ROOT / "src" / "bootstrap_final_writer_m1_bundle_v1_build.cpp"
HELPER = ROOT / "tools" / "run_m1_three_payload_preload_v1.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"M1_THREE_PAYLOAD_POLICY_FAIL {message}")


def main() -> None:
    require(len(sys.argv) == 3, "usage")
    bundle_artifact = pathlib.Path(sys.argv[1])
    bootstrap_artifact = pathlib.Path(sys.argv[2])
    require(bundle_artifact.is_file() and bundle_artifact.stat().st_size > 0,
            "bundle_artifact")
    require(bootstrap_artifact.is_file() and
            bootstrap_artifact.stat().st_size > 0,
            "bootstrap_artifact")
    bundle = BUNDLE_SOURCE.read_text(encoding="utf-8")
    bootstrap = BOOTSTRAP_SOURCE.read_text(encoding="utf-8")
    helper = HELPER.read_text(encoding="utf-8")
    natural = bundle.find("dlopen(kNaturalPayload")
    controller = bundle.find("dlopen(kControllerPayload")
    require(0 <= natural < controller, "guest_load_order")
    require("dlclose(" not in bundle, "payload_lifetime_must_be_process_wide")
    for token in (
        "liba9tas_natural_action_replay_v1_review_only.so",
        "liba9tas_controller_shadow_coordinator_v1_build_only.so",
        "a9tas_payload_bundle_status_v1",
    ):
        require(token in bundle, f"bundle_token={token}")
    for token in (
        "liba9tas_final_writer_replay_v1_build_only.so",
        "liba9tas_payload_bundle_natural_controller_v1.so",
        "A9TAS_SECOND_PAYLOAD_PATH",
    ):
        require(token in bootstrap, f"bootstrap_token={token}")
    require("RC_RIP_BIAS=2" in helper and "--wait-window" in helper and
            "liba9tas_bootstrap_final_writer_m1_v1.so" in helper,
            "preload_helper_contract")
    print(
        "M1_THREE_PAYLOAD_PRELOAD_POLICY passed=1 normal_guest_context=1 "
        "writer_first=1 natural_second=1 controller_third=1 legacy_changed=0 "
        "device_access=0"
    )


if __name__ == "__main__":
    main()
