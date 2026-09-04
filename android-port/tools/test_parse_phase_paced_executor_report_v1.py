#!/usr/bin/env python3

import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PARSER = ROOT / "tools" / "parse_phase_paced_executor_report_v1.py"
REPORT = struct.Struct("<8s4I24Q2I4i3Q32s32s")


class ParserTests(unittest.TestCase):
    def make_report(
        self,
        recording: bytes,
        payload: bytes,
        *,
        flags: int = 0x1FF,
        delta_writes: int = 5,
        committed: int = 5,
        writer: int = 5,
        event_total: int = 24,
    ) -> bytes:
        qwords = [
            123, 456, 0x1000, 0x2000, 0x3000,
            0x4000, 0x5000, 0x5004, 0x6000,
            0x7000, 0x8000, 0x9000,
            event_total, delta_writes, 1, 0, 0, 0,
            9, 5, 5, 5, 4, committed,
        ]
        return REPORT.pack(
            b"A9PHEX1\0", 1, REPORT.size, flags, 5,
            *qwords, 4, 4, 0, 0, 0, 0,
            5, writer, 5,
            hashlib.sha256(recording).digest(),
            hashlib.sha256(payload).digest(),
        )

    def run_parser(self, report: bytes) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            recording = root / "input.a9utk1"
            payload = root / "payload.so"
            report_path = root / "report.a9phex1"
            recording.write_bytes(b"recording")
            payload.write_bytes(b"payload")
            report_path.write_bytes(report)
            return subprocess.run(
                [
                    sys.executable, "-B", str(PARSER), str(report_path),
                    str(recording), str(payload), "--frames", "5",
                ],
                text=True, capture_output=True, check=False,
            )

    def test_accepts_complete_world_commit_report(self) -> None:
        result = self.run_parser(self.make_report(b"recording", b"payload"))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_old_pre_next_flags(self) -> None:
        result = self.run_parser(
            self.make_report(b"recording", b"payload", flags=0xFF)
        )
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_fifth_write_without_fifth_commit(self) -> None:
        result = self.run_parser(
            self.make_report(b"recording", b"payload", committed=4)
        )
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_lagging_game_owned_writer(self) -> None:
        result = self.run_parser(
            self.make_report(b"recording", b"payload", writer=4)
        )
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_phase_event_accounting_mismatch(self) -> None:
        result = self.run_parser(
            self.make_report(b"recording", b"payload", event_total=25)
        )
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_sixth_delta_write(self) -> None:
        result = self.run_parser(
            self.make_report(b"recording", b"payload", delta_writes=6)
        )
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
