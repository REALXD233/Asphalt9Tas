#!/usr/bin/env python3
"""Offline contract tests for the FC-3 guarded-runner design.

This module validates design text only.  It never invokes ADB, a runner, an
emulator or a game process, and it does not authorize runner implementation.
"""

from __future__ import annotations

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
DESIGN = (ROOT / "FC3_GUARDED_RUNNER_DESIGN_ONLY_20260820.md").read_text(
    encoding="utf-8"
)
COMPACT = " ".join(DESIGN.split())


def cleanup_required(*, acknowledgements_complete: bool,
                     receipt_present: bool, probe_passed: bool) -> bool:
    """Model the package-scoped cleanup arming boundary."""
    return acknowledgements_complete and receipt_present and not probe_passed


def parse_proc_stat_start_time(line: str) -> int | None:
    """Model the controller's last-')', field-22 parser."""
    close = line.rfind(")")
    if close < 0:
        return None
    suffix = line[close + 1:].strip().split()
    if len(suffix) < 20 or len(suffix[0]) != 1:
        return None
    token = suffix[19]
    if not re.fullmatch(r"[0-9]+", token):
        return None
    value = int(token)
    if value > 0xFFFFFFFFFFFFFFFF:
        return None
    return value


def make_proc_stat(comm: str, start_time: str = "987654321") -> str:
    # Suffix tokens are fields 3..22.  Field 22 is token index 19.
    return f"123 ({comm}) S " + " ".join(["1"] * 18 + [start_time]) + "\n"


class CleanupArmingModelTest(unittest.TestCase):
    def test_malformed_receipt_still_requires_cleanup(self) -> None:
        self.assertTrue(cleanup_required(
            acknowledgements_complete=True,
            receipt_present=True,
            probe_passed=False,
        ))

    def test_missing_receipt_does_not_expand_cleanup_authority(self) -> None:
        self.assertFalse(cleanup_required(
            acknowledgements_complete=True,
            receipt_present=False,
            probe_passed=False,
        ))

    def test_unacknowledged_probe_does_not_expand_cleanup_authority(self) -> None:
        self.assertFalse(cleanup_required(
            acknowledgements_complete=False,
            receipt_present=True,
            probe_passed=False,
        ))

    def test_success_does_not_force_stop(self) -> None:
        self.assertFalse(cleanup_required(
            acknowledgements_complete=True,
            receipt_present=True,
            probe_passed=True,
        ))


class ProcessStatParserModelTest(unittest.TestCase):
    def test_normal_comm(self) -> None:
        self.assertEqual(parse_proc_stat_start_time(make_proc_stat("game")),
                         987654321)

    def test_comm_with_spaces_and_right_parentheses(self) -> None:
        self.assertEqual(
            parse_proc_stat_start_time(make_proc_stat("Asphalt ) worker")),
            987654321,
        )

    def test_missing_close_or_fields_is_rejected(self) -> None:
        self.assertIsNone(parse_proc_stat_start_time("123 broken S 1 2"))
        self.assertIsNone(parse_proc_stat_start_time("123 (short) S 1 2"))

    def test_nonnumeric_or_overflow_start_time_is_rejected(self) -> None:
        self.assertIsNone(parse_proc_stat_start_time(
            make_proc_stat("game", "not-a-number")
        ))
        self.assertIsNone(parse_proc_stat_start_time(
            make_proc_stat("game", str(1 << 64))
        ))

class DesignBindingTest(unittest.TestCase):
    def test_controller_contract_binds_start_time(self) -> None:
        self.assertIn(
            "controller PID EXPECTED_START_TIME LIB_BASE_HEX TIMEOUT_MS",
            DESIGN,
        )
        self.assertGreaterEqual(DESIGN.count("EXPECTED_START_TIME"), 2)
        first_read = COMPACT.index("before its first process read")
        attach = COMPACT.index("immediately before its first ptrace attach")
        self.assertLess(first_read, attach)
        controller = (
            ROOT / "src" / "fc2_frame_callback_transaction_controller_v1.cpp"
        ).read_text(encoding="utf-8")
        parser = controller.index("bool Fc3ParseProcessStatStartTime")
        last_close = controller.index("std::strrchr(line, ')')", parser)
        field_loop = controller.index("field = 4; field < 22", last_close)
        parse_value = controller.index("std::strtoull(cursor, &end, 10)",
                                       field_loop)
        reader = controller.index("bool Fc3ReadProcessStartTime", parse_value)
        reader_binding = controller.index(
            "Fc3ParseProcessStatStartTime(line, output)", reader
        )
        self.assertLess(parser, last_close)
        self.assertLess(last_close, field_loop)
        self.assertLess(field_loop, parse_value)
        self.assertLess(parse_value, reader)
        self.assertLess(reader, reader_binding)

    def test_receipt_binds_boot_nonce_age_and_offline_closure(self) -> None:
        for token in (
            "device boot_id",
            "cryptographically random attempt nonce",
            "runner-policy hash",
            "closure-audit hash",
            "maximum receipt age",
        ):
            self.assertIn(token, COMPACT)

    def test_cleanup_is_armed_before_receipt_trust(self) -> None:
        arm = DESIGN.index("package cleanup is armed")
        parse = DESIGN.index("before parsing or trusting any receipt field")
        force_stop = DESIGN.index("Force-stop is package-based")
        self.assertLess(arm, parse)
        self.assertLess(parse, force_stop)

    def test_report_commit_is_exclusive_and_hash_bound(self) -> None:
        for token in (
            "exclusive/no-follow semantics",
            "reject symlinks and non-regular files",
            "fsync",
            "never overwrite an existing path",
            "remote SHA-256",
        ):
            self.assertIn(token, COMPACT)

    def test_success_contract_contains_new_identity_barriers(self) -> None:
        success = DESIGN[DESIGN.index("## Success contract"):]
        for token in (
            "matching boot ID",
            "valid nonce",
            "unexpired receipt",
            "controller-side start-time checks",
            "matching remote/local SHA-256 values",
        ):
            self.assertIn(token, success)

    def test_design_remains_non_live_and_review_gated(self) -> None:
        self.assertIn("Gate 12 is closed", DESIGN)
        self.assertIn("independent review in", DESIGN)
        self.assertIn("does not authorize runner implementation", DESIGN)


if __name__ == "__main__":
    unittest.main()
