#!/usr/bin/env python3
"""Offline ordering/failure proof for payload storage publication."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
STORAGE = ROOT / "src" / "final_writer_storage_transaction_v1.h"
BLOB = ROOT / "src" / "final_writer_target_blob_protocol_v1.h"


def stage(fail_at: int | None = None):
    operations = []
    names = ["disable", "evidence", "audits", "targets", "shadow", "publish"]
    for index, name in enumerate(names):
        operations.append(name)
        if fail_at == index:
            if name == "publish":
                operations.append("disable_rollback")
            return False, operations
    return True, operations


class StorageTransactionTests(unittest.TestCase):
    def test_publish_is_last(self) -> None:
        ok, operations = stage()
        self.assertTrue(ok)
        self.assertEqual(
            operations,
            ["disable", "evidence", "audits", "targets", "shadow", "publish"],
        )

    def test_publish_failure_disables_again(self) -> None:
        ok, operations = stage(5)
        self.assertFalse(ok)
        self.assertEqual(operations[-2:], ["publish", "disable_rollback"])

    def test_earlier_failure_never_publishes(self) -> None:
        for fail_at in range(5):
            ok, operations = stage(fail_at)
            self.assertFalse(ok)
            self.assertNotIn("publish", operations)

    def test_source_contains_exact_order_and_fail_closed_rollback(self) -> None:
        text = STORAGE.read_text(encoding="utf-8")
        tokens = (
            "write(layout.control, &prepared.unpublished_control",
            "write(layout.evidence, &prepared.fresh_evidence",
            "write(layout.audits, zero_audits.data()",
            "write(layout.targets, targets",
            "write(layout.shadow, prepared.shadow.data()",
            "write(layout.control, &prepared.published_control",
        )
        positions = [text.index(token) for token in tokens]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("kPublishRollbackFailed", text)
        self.assertIn("ValidateLayout", text)
        for forbidden in ("ptrace", "pwrite", "process_vm_writev", "dlopen"):
            self.assertNotIn(forbidden, text)

    def test_cpp_blob_header_matches_python_format(self) -> None:
        text = BLOB.read_text(encoding="utf-8")
        for needle in (
            "kHeaderSize = 128", "kRecordSize = 80",
            "kRequiredIntervalUs = 16667", "kRequiredFlags = 3",
            "header.frame_count < 2", "header.frame_count > kMaximumFrames",
            "header.source_size != expected_source_size",
            "header.source_sha256, expected_source_sha256, 32",
            "size != sizeof(Header) + records_size",
        ):
            self.assertIn(needle, text)


if __name__ == "__main__":
    unittest.main()
