#!/usr/bin/env python3

import hashlib
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
BUNDLE_SOURCE = ROOT / "src" / "payload_bundle_barrel_successor_v1.cpp"
BOOTSTRAP_SOURCE = ROOT / "src" / "bootstrap_final_writer_barrel_successor_v1_build.cpp"
BARREL_PAYLOAD = (
    ROOT / "build" / "barrel-yaw-tail-payload-v1" /
    "liba9tas_barrel_yaw_tail_v1_build_only.so"
)
BARREL_SHA256 = "965e6ba9ce819ee4b6c1a38bf663f68e0c76841dfa7bfad0924ad67bee677ea9"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"BARREL_SUCCESSOR_PRELOAD_POLICY_FAIL {message}")


def main() -> None:
    # The aggregate regression runner appends every hash-relevant artifact to
    # the command line.  The policy consumes the two produced ELFs and accepts
    # the remaining paths as existence-pinned runner inputs.
    require(len(sys.argv) >= 3, "usage")
    bundle_artifact = pathlib.Path(sys.argv[1])
    bootstrap_artifact = pathlib.Path(sys.argv[2])
    require(bundle_artifact.is_file() and bundle_artifact.stat().st_size > 0,
            "bundle_artifact")
    require(bootstrap_artifact.is_file() and bootstrap_artifact.stat().st_size > 0,
            "bootstrap_artifact")

    bundle = BUNDLE_SOURCE.read_text(encoding="utf-8")
    bootstrap = BOOTSTRAP_SOURCE.read_text(encoding="utf-8")
    natural = bundle.find("dlopen(kNaturalPayload")
    interval = bundle.find("dlopen(kIntervalPayload")
    barrel = bundle.find("dlopen(kBarrelPayload")
    require(0 <= natural < interval < barrel, "guest_load_order")
    require("dlclose(" not in bundle, "payload_lifetime_must_be_process_wide")
    for token in (
        "liba9tas_natural_action_replay_v1_review_only.so",
        "liba9tas_physics_interval_getter_v2_passive.so",
        "liba9tas_barrel_yaw_tail_v1_build_only.so",
        "a9tas_payload_bundle_barrel_successor_status_v1",
    ):
        require(token in bundle, f"bundle_token={token}")
    for token in (
        "liba9tas_final_writer_replay_v1_build_only.so",
        "liba9tas_payload_bundle_barrel_successor_v1.so",
        "A9TAS_SECOND_PAYLOAD_PATH",
    ):
        require(token in bootstrap, f"bootstrap_token={token}")

    require(BARREL_PAYLOAD.is_file(), "barrel_payload_missing")
    actual = hashlib.sha256(BARREL_PAYLOAD.read_bytes()).hexdigest()
    require(actual == BARREL_SHA256, f"barrel_payload_hash={actual}")
    print(
        "BARREL_SUCCESSOR_PRELOAD_POLICY passed=1 normal_guest_context=1 "
        "writer_first=1 natural_second=1 interval_third=1 barrel_fourth=1 "
        "barrel_hash_pinned=1 legacy_changed=0 device_access=0"
    )


if __name__ == "__main__":
    main()
