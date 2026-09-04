from pathlib import Path
import importlib.util
import struct
import tempfile


module_path = Path(__file__).with_name("parse_physics_interval_shadow_receipt_v2.py")
spec = importlib.util.spec_from_file_location("parser_v2", module_path)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)

report = bytearray(module.REPORT_SIZE)
report[:8] = b"A9PGTR2\0"
struct.pack_into("<4I", report, 8, 2, module.REPORT_SIZE, 3, 0x3FFF)
struct.pack_into("<13Q", report, 24, *range(1, 14))
struct.pack_into("<2I", report, 128, 1, 1)
struct.pack_into("<4Q", report, 136, 7, 1, 1, 0)
module.CONTROL.pack_into(
    report, module.CONTROL_OFFSET, b"A9PGC2\0\0", 2, 64, 1, 0, 1, 1, 1, 0,
    4, 6, 7)
module.EVIDENCE.pack_into(
    report, module.EVIDENCE_OFFSET, b"A9PGE2\0\0", 2, 128, 2, 0,
    1, 1, 0, 0, 0, 0, 42, 42, 0, 0, 0, 0)
report[module.HASH_OFFSET:module.HASH_OFFSET + 32] = bytes(range(32))
bits = 0x3C888889
event = module.EVENT.pack(0, 4, 99, 100, 42, bits, 0, bits, 0x33, 0, 0)

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / "receipt.a9pgtr2"
    path.write_bytes(report + struct.pack("<I", bits) + event)
    result, interval_bytes = module.parse(path)
    assert result["complete"] is True
    assert result["control"]["cursor"] == 1
    assert result["evidence"]["calls"] == 1
    assert result["intervals"][0]["bits"] == "0x3c888889"
    assert result["events"][0]["flags"] == "0x33"
    assert interval_bytes == struct.pack("<I", bits)

print("PARSE_PHYSICS_INTERVAL_SHADOW_RECEIPT_V2_SELFTEST passed=1")
