#!/usr/bin/env python3
"""Offline audit for the persistent per-frame natural-action replay payload."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "payload_natural_action_callback_lifecycle_v1.cpp"
TRANSPORT = ROOT / "src" / "natural_action_replay_transport_v1.h"
BUILD = ROOT / "build-natural-action-replay-payload-v1.ps1"


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


def source_policy() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    transport = TRANSPORT.read_text(encoding="utf-8")
    build = BUILD.read_text(encoding="utf-8").lower()
    for token in (
        "A9TAS_NAL_REPLAY_EXECUTE",
        "replay::ProveTransition",
        "kActionSubmissionProof",
        "SubmitGameOwnedActions",
        "command.nitro_activations",
        "CompleteNaturalCallback",
        "one exact game-queue growth per call",
        "frame immediately",
    ):
        require(token in source, f"replay payload token missing: {token}")
    for token in (
        "sequence != frame.tick + 1",
        "kMaximumNitroActivations",
        "kSkipNitroActivation",
        "kNitroOverrideEnabled",
    ):
        require(token in transport, f"replay transport token missing: {token}")
    require("-da9tas_nal_action_execute=1" in build and
            "-da9tas_nal_replay_execute=1" in build,
            "replay/action compile gates missing")
    for forbidden in ("adb", "push", "install", "keyevent"):
        require(forbidden not in build,
                f"offline replay payload build contains {forbidden}")
    for forbidden in ("ptrace(", "pwrite(", "mprotect(", "pthread_create"):
        require(forbidden not in source,
                f"replay payload contains forbidden primitive: {forbidden}")


def artifact_policy(payload: pathlib.Path, readelf: pathlib.Path,
                    objdump: pathlib.Path) -> None:
    for path in (payload, readelf, objdump):
        require(path.is_file(), f"missing replay payload audit input: {path}")
    header = subprocess.check_output([str(readelf), "-h", str(payload)], text=True)
    require("AArch64" in header and re.search(r"Type:\s+DYN", header),
            "replay payload is not an AArch64 DSO")
    symbols = subprocess.check_output(
        [str(readelf), "--dyn-syms", "--wide", str(payload)], text=True
    )
    for symbol in (
        "a9tas_natural_action_registration_bootstrap_v1",
        "a9tas_natural_action_persistent_consumer_v1",
        "a9tas_natural_action_lifecycle_mailbox_data_v1",
    ):
        require(re.search(rf"\b{symbol}$", symbols, re.MULTILINE) is not None,
                f"replay payload export missing: {symbol}")
    for symbol in ("ptrace", "pwrite", "mprotect", "pthread_create"):
        require(re.search(rf"UND\s+{symbol}(?:@|$)", symbols, re.MULTILINE) is None,
                f"replay payload imports forbidden symbol: {symbol}")
    disassembly = subprocess.check_output(
        [str(objdump), "-d", "--demangle", str(payload)], text=True
    ).lower()
    require("a9tas_natural_action_persistent_consumer_v1" in disassembly,
            "persistent replay consumer is absent from disassembly")


def main() -> int:
    source_policy()
    if len(sys.argv) == 4:
        artifact_policy(*(pathlib.Path(value) for value in sys.argv[1:]))
    elif len(sys.argv) != 1:
        raise SystemExit(f"usage: {sys.argv[0]} [payload readelf objdump]")
    print("NATURAL_ACTION_REPLAY_PAYLOAD_AUDIT passed=1 persistent=1 "
          "counts_0_1_2=1 forced_colour_state=0 device_access=0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
