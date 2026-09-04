#!/usr/bin/env python3

import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PARSER = ROOT / "tools" / "parse_m1_executor_report_v1.py"
REPORT = struct.Struct("<8s4I14Q2I4i3Q32s32s")


class ParserTests(unittest.TestCase):
    def make_report(self, recording: bytes, payload: bytes, *, flags=0x1FF,
                    completed=5, delta_writes=5) -> bytes:
        qwords = [123, 456, 0x1000, 0x2000, 0x3000, 0x4000, 0x5000,
                  0x6000, 9, delta_writes, 1, 0, 0, 0]
        return REPORT.pack(
            b"A9M1EX1\0", 1, REPORT.size, flags, 5,
            *qwords, 4, 4, 0, 0, 0, 0,
            completed, 5, 5,
            hashlib.sha256(recording).digest(),
            hashlib.sha256(payload).digest())

    def run_parser(self, report: bytes) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            recording = root / "input.a9utk1"
            payload = root / "payload.so"
            report_path = root / "report.a9m1ex1"
            recording.write_bytes(b"recording")
            payload.write_bytes(b"payload")
            report_path.write_bytes(report)
            return subprocess.run(
                [sys.executable, "-B", str(PARSER), str(report_path),
                 str(recording), str(payload), "--frames", "5"],
                text=True, capture_output=True, check=False)

    def test_accepts_complete_report(self) -> None:
        result = self.run_parser(self.make_report(b"recording", b"payload"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_missing_cleanup_flag(self) -> None:
        result = self.run_parser(
            self.make_report(b"recording", b"payload", flags=0x7F))
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_missing_pre_next_tick_completion_flag(self) -> None:
        result = self.run_parser(
            self.make_report(b"recording", b"payload", flags=0xFF))
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_receipt_mismatch(self) -> None:
        result = self.run_parser(
            self.make_report(b"recording", b"payload", completed=4))
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_extra_delta_write(self) -> None:
        result = self.run_parser(
            self.make_report(b"recording", b"payload", delta_writes=6))
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
