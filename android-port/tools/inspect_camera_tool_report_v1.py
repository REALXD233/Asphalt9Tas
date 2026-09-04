#!/usr/bin/env python3
"""Strict reader for the fixed A9CTTR1 Camera Tool transaction receipt."""

from __future__ import annotations

import argparse
import json
import pathlib
import struct


REPORT_SIZE = 504
EVIDENCE_OFFSET = 248


def read_report(path: pathlib.Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) != REPORT_SIZE:
        raise ValueError(f"unexpected report size: {len(data)}")
    if data[:8] != b"A9CTTR1\0" or struct.unpack_from("<I", data, 8)[0] != 1:
        raise ValueError("invalid report header")
    if struct.unpack_from("<I", data, 12)[0] != REPORT_SIZE:
        raise ValueError("invalid report size field")
    evidence = data[EVIDENCE_OFFSET:]
    if evidence[:8] != b"A9CTE1\0\0":
        raise ValueError("invalid evidence magic")
    if struct.unpack_from("<II", evidence, 8) != (1, 256):
        raise ValueError("invalid evidence header")
    result: dict[str, object] = {
        "action": struct.unpack_from("<I", data, 16)[0],
        "report_flags": struct.unpack_from("<I", data, 20)[0],
        "pid": struct.unpack_from("<Q", data, 24)[0],
        "manager": struct.unpack_from("<Q", data, 56)[0],
        "shape": struct.unpack_from("<Q", data, 64)[0],
        "source": struct.unpack_from("<Q", data, 136)[0],
        "source_vptr": struct.unpack_from("<Q", data, 144)[0],
        "position_setter": struct.unpack_from("<Q", data, 152)[0],
        "rotation_setter": struct.unpack_from("<Q", data, 160)[0],
        "fov_setter": struct.unpack_from("<Q", data, 168)[0],
        "override_flags": struct.unpack_from("<I", data, 176)[0],
        "command_sequence": struct.unpack_from("<Q", data, 184)[0],
        "wrapper_entries": struct.unpack_from("<Q", evidence, 16)[0],
        "original_calls": struct.unpack_from("<Q", evidence, 24)[0],
        "original_returns": struct.unpack_from("<Q", evidence, 32)[0],
        "active_entries": struct.unpack_from("<Q", evidence, 40)[0],
        "inactive_entries": struct.unpack_from("<Q", evidence, 48)[0],
        "combined_calls": struct.unpack_from("<Q", evidence, 56)[0],
        "combined_returns": struct.unpack_from("<Q", evidence, 64)[0],
        "fov_writes": struct.unpack_from("<Q", evidence, 72)[0],
        "command_busy_entries": struct.unpack_from("<Q", evidence, 80)[0],
        "failures": struct.unpack_from("<Q", evidence, 88)[0],
        "recursive_entries": struct.unpack_from("<Q", evidence, 96)[0],
        "producer_tid": struct.unpack_from("<I", evidence, 104)[0],
        "last_status": struct.unpack_from("<i", evidence, 108)[0],
        "last_command_sequence": struct.unpack_from("<Q", evidence, 112)[0],
        "last_override_flags": struct.unpack_from("<I", evidence, 144)[0],
        "natural_transform": list(struct.unpack_from("<7f", evidence, 152)),
        "applied_transform": list(struct.unpack_from("<7f", evidence, 180)),
        "natural_fov": struct.unpack_from("<f", evidence, 208)[0],
        "applied_fov": struct.unpack_from("<f", evidence, 212)[0],
        "source_override_calls": struct.unpack_from("<Q", evidence, 216)[0],
        "source_override_returns": struct.unpack_from("<Q", evidence, 224)[0],
        "source_readback_passes": struct.unpack_from("<Q", evidence, 232)[0],
        "last_source": struct.unpack_from("<Q", evidence, 240)[0],
        "last_source_vptr": struct.unpack_from("<Q", evidence, 248)[0],
    }
    if result["original_calls"] != result["original_returns"]:
        raise ValueError("unbalanced original callback")
    if result["combined_calls"] != result["combined_returns"]:
        raise ValueError("unbalanced combined transform")
    if result["source_override_calls"] != result["source_override_returns"]:
        raise ValueError("unbalanced source override")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("report", type=pathlib.Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    result = read_report(args.report)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(
            "CAMERA_TOOL_REPORT_VALID "
            f"action={result['action']} entries={result['wrapper_entries']} "
            f"active={result['active_entries']} failures={result['failures']} "
            f"status={result['last_status']} sequence={result['last_command_sequence']}"
        )
        print("natural_transform=" + ",".join(map(str, result["natural_transform"])))
        print("applied_transform=" + ",".join(map(str, result["applied_transform"])))
        print(f"natural_fov={result['natural_fov']} applied_fov={result['applied_fov']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
