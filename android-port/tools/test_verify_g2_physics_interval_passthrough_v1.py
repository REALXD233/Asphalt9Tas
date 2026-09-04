#!/usr/bin/env python3
"""Negative tests for the G2 source/ABI policy."""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path


def rejected(callable_) -> bool:
    try:
        callable_()
    except ValueError:
        return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--verifier", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--disassembly", type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("g2_policy", args.verifier)
    if spec is None or spec.loader is None:
        raise SystemExit("cannot load verifier")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    header = args.header.read_text(encoding="utf-8")
    source = args.source.read_text(encoding="utf-8")
    disassembly = args.disassembly.read_text(encoding="utf-8")
    module.verify_source(header, source)
    module.verify_abi(disassembly)
    cases = (
        rejected(lambda: module.verify_source(
            header, source.replace("kTargetRva = 0x3695474", "kTargetRva = 0"))),
        rejected(lambda: module.verify_source(header, source + "\npwrite(0,0,0,0);\n")),
        rejected(lambda: module.verify_source(header, source + "\n*output = 0;\n")),
        rejected(lambda: module.verify_abi(
            disassembly.replace("msr\tFPCR, x10", "nop", 1))),
        rejected(lambda: module.verify_abi(
            disassembly.replace("G2PhysicsIntervalOriginalTrampolineV1>",
                                "MissingOriginal>", 1))),
    )
    if not all(cases):
        print(f"G2_POLICY_SELFTEST passed=0 cases={sum(cases)}/{len(cases)}")
        return 1
    print(f"G2_POLICY_SELFTEST passed=1 negative_cases={len(cases)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
