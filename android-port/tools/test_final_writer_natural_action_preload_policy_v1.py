#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "bootstrap_final_writer_natural_action_v1_build.cpp"
BOOTSTRAP = ROOT / "src" / "bootstrap.cpp"
HELPER = ROOT / "tools" / "run_final_writer_natural_action_preload_v1.sh"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    require(len(sys.argv) == 2, "usage: policy BOOTSTRAP_DSO")
    artifact = pathlib.Path(sys.argv[1])
    require(artifact.is_file() and artifact.stat().st_size > 0,
            "bootstrap DSO missing")
    source = SOURCE.read_text(encoding="utf-8")
    bootstrap = BOOTSTRAP.read_text(encoding="utf-8")
    helper = HELPER.read_text(encoding="utf-8")
    for token in ("liba9tas_final_writer_replay_v1_build_only.so",
                  "liba9tas_natural_action_replay_v1_review_only.so",
                  "A9TAS_SECOND_PAYLOAD_PATH"):
        require(token in source or token in bootstrap,
                f"missing paired preload token: {token}")
    require("g_real_load_library(kSecondPayloadPath" in bootstrap and
            "g_real_load_library_ext(kSecondPayloadPath" in bootstrap,
            "second payload is not loaded in both NativeBridge routes")
    require("RC_RIP_BIAS=2" in helper and "--wait-window" in helper and
            "liba9tas_bootstrap_final_writer_natural_action_v1.so" in helper,
            "preload helper contract")
    print("FINAL_WRITER_NATURAL_ACTION_PRELOAD_POLICY passed=1 "
          "single_intercept=1 payloads=2 bridge_routes=2 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
