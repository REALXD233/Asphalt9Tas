#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


HEX_KEYS = {
    "guest_base", "x0", "x1", "caller", "vptr", "source",
    "source_vptr", "slot40", "slot70",
}
INT_KEYS = {
    "tid", "object_read", "source_read", "slot40_read", "slot70_read",
}


def parse(path: Path) -> dict:
    fields: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        if not raw or "=" not in raw:
            continue
        key, value = raw.split("=", 1)
        if key in fields:
            raise ValueError(f"duplicate field: {key}")
        fields[key] = value
    required = {"magic"} | HEX_KEYS | INT_KEYS
    missing = sorted(required - fields.keys())
    if missing:
        raise ValueError("missing fields: " + ",".join(missing))
    if fields["magic"] != "A9G3BI1":
        raise ValueError("wrong magic")
    values = {key: int(fields[key], 16) for key in HEX_KEYS}
    values.update({key: int(fields[key], 10) for key in INT_KEYS})
    base = values["guest_base"]
    exact_caller = values["caller"] == base + 0x37D98C4
    primary_object = values["vptr"] == base + 0x80C4DA8
    primary_slot = values["slot70"] == base + 0x386B1E0
    secondary_object = values["vptr"] == base + 0x80C5058
    secondary_slot = values["slot40"] == base + 0x386B3C8
    reads_complete = all(values[key] == 1 for key in INT_KEYS if key != "tid")
    nonzero = all(values[key] != 0 for key in
                  ("guest_base", "x0", "x1", "caller", "vptr", "source",
                   "source_vptr", "tid"))
    recognized_route = ((primary_object and primary_slot) or
                        (secondary_object and secondary_slot))
    return {
        "passed": bool(reads_complete and nonzero and exact_caller and
                       recognized_route),
        "reads_complete": reads_complete,
        "exact_caller": exact_caller,
        "recognized_route": recognized_route,
        "primary_object": primary_object,
        "secondary_object": secondary_object,
        **{key: f"0x{values[key]:x}" for key in HEX_KEYS},
        "tid": values["tid"],
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("receipt", type=Path)
    args = ap.parse_args()
    try:
        result = parse(args.receipt)
    except (OSError, ValueError) as exc:
        print(json.dumps({"passed": False, "error": str(exc)}, indent=2))
        return 1
    print(json.dumps(result, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
