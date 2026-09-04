#!/usr/bin/env python3
import tempfile
from pathlib import Path

from parse_g3_begin_identity_v1 import parse


BASE = 0x700000000000


def receipt(*, caller: int, vptr: int, slot40: int, slot70: int) -> str:
    return f"""magic=A9G3BI1
guest_base=0x{BASE:x}
x0=0x710000001000
x1=0x710000002000
caller=0x{caller:x}
tid=42
vptr=0x{vptr:x}
source=0x710000003000
source_vptr=0x{BASE + 0x80BF1C0:x}
slot40=0x{slot40:x}
slot70=0x{slot70:x}
object_read=1
source_read=1
slot40_read=1
slot70_read=1
"""


def run() -> None:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "identity.txt"
        path.write_text(receipt(
            caller=BASE + 0x37D98C4,
            vptr=BASE + 0x80C4DA8,
            slot40=BASE + 0x386AD04,
            slot70=BASE + 0x386B1E0), encoding="utf-8")
        assert parse(path)["passed"]
        path.write_text(receipt(
            caller=BASE + 0x37D98C4,
            vptr=BASE + 0x80C5058,
            slot40=BASE + 0x386B3C8,
            slot70=BASE + 0x386B654), encoding="utf-8")
        assert parse(path)["passed"]
        path.write_text(receipt(
            caller=BASE + 0x37D98C8,
            vptr=BASE + 0x80C4DA8,
            slot40=BASE + 0x386AD04,
            slot70=BASE + 0x386B1E0), encoding="utf-8")
        assert not parse(path)["passed"]
    print("G3_BEGIN_IDENTITY_PARSER_SELFTEST passed=1 cases=3")


if __name__ == "__main__":
    run()
