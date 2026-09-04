#!/usr/bin/env python3
"""Offline policy audit for the fail-closed Barrel successor generator."""

from __future__ import annotations

from pathlib import Path
import sys

import generate_barrel_successor_source_v1 as generator


TOOLS_DIR = Path(__file__).resolve().parent
ANDROID_PORT = TOOLS_DIR.parent
GENERATOR = TOOLS_DIR / "generate_barrel_successor_source_v1.py"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    require(GENERATOR.is_file(), "generator source missing")
    source = GENERATOR.read_text(encoding="utf-8")
    required_tokens = (
        "PINNED_SHA256 =",
        "REQUIRED_ANCHOR_IDS =",
        'document.get("complete_successor") is not True',
        "declared != list(REQUIRED_ANCHOR_IDS)",
        "count != 1",
        "replacement anchors overlap",
        "for start, end, replacement in reversed(ordered_spans)",
        "verify_pinned_inputs(android_port_root)",
        "tempfile.mkdtemp",
        "os.replace(staging, output)",
        "output directory already exists",
        "source in output.parents",
        "--replacements-json",
        "required=True",
    )
    for token in required_tokens:
        require(token in source, f"generator policy token missing: {token}")
    for relative, expected in generator.PINNED_SHA256.items():
        require(relative in source, f"pinned path absent from generator: {relative}")
        require(expected in source, f"pinned SHA absent from generator: {relative}")
    require(len(generator.PINNED_SHA256) == 5, "pinned input cardinality changed")
    require(len(generator.WRAPPER_PATHS) == 3, "wrapper cardinality changed")
    require(len(generator.REQUIRED_ANCHOR_IDS) == 11,
            "required successor anchor cardinality changed")
    require(len(set(generator.REQUIRED_ANCHOR_IDS)) == 11,
            "required successor anchors are not unique")
    inputs = generator.verify_pinned_inputs(ANDROID_PORT)
    require(set(inputs) == set(generator.PINNED_SHA256),
            "real pinned inputs did not verify exactly")

    lowered = source.lower()
    for forbidden in (
        "adb ", "adb\"", "ptrace(", "process_vm_writev", "pwrite(",
        "remote_call", "input keyevent", "input tap",
    ):
        require(forbidden not in lowered,
                f"generator gained a live/device primitive: {forbidden}")
    print(
        "BARREL_SUCCESSOR_SOURCE_GENERATOR_POLICY passed=1 "
        "pinned_inputs=5 wrappers=3 required_anchors=11 exact_replace=1 "
        "atomic_directory_publish=1 incomplete_default=refused "
        "device_access=0 game_writes=0"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(
            f"BARREL_SUCCESSOR_SOURCE_GENERATOR_POLICY passed=0 error={error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
