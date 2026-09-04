#!/usr/bin/env python3
"""Offline state-model proof for one external tick to one writer callback."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "final_writer_cursor_binding_v1.h"


class Model:
    def __init__(self, count: int):
        if not 2 <= count <= 3600:
            raise ValueError("count")
        self.count = count
        self.index = 0
        self.phase = "ready"

    def begin(self, external: int, processed: int, entries: int) -> None:
        if self.phase != "ready" or external != self.index:
            raise ValueError("begin")
        if processed != self.index or entries != self.index:
            raise ValueError("pre-cursor")
        self.phase = "await"

    def acknowledge(self, external: int, processed: int, entries: int) -> None:
        if self.phase != "await" or external != self.index:
            raise ValueError("ack")
        if processed != self.index + 1 or entries != self.index + 1:
            raise ValueError("post-cursor")
        self.phase = "acked"

    def commit(self, external: int, processed: int) -> None:
        if self.phase != "acked" or external != self.index:
            raise ValueError("commit")
        if processed != self.index + 1:
            raise ValueError("commit-cursor")
        self.index += 1
        self.phase = "complete" if self.index == self.count else "ready"


class CursorBindingTests(unittest.TestCase):
    def test_exact_two_frame_sequence(self) -> None:
        model = Model(2)
        for index in range(2):
            model.begin(index, index, index)
            model.acknowledge(index, index + 1, index + 1)
            model.commit(index, index + 1)
        self.assertEqual((model.index, model.phase), (2, "complete"))

    def test_rejects_callback_before_first_external_tick(self) -> None:
        model = Model(2)
        with self.assertRaisesRegex(ValueError, "pre-cursor"):
            model.begin(0, 1, 1)

    def test_rejects_missing_callback(self) -> None:
        model = Model(2)
        model.begin(0, 0, 0)
        with self.assertRaisesRegex(ValueError, "post-cursor"):
            model.acknowledge(0, 0, 0)

    def test_rejects_duplicate_callback(self) -> None:
        model = Model(2)
        model.begin(0, 0, 0)
        with self.assertRaisesRegex(ValueError, "post-cursor"):
            model.acknowledge(0, 2, 2)

    def test_rejects_external_skip(self) -> None:
        model = Model(2)
        with self.assertRaisesRegex(ValueError, "begin"):
            model.begin(1, 0, 0)

    def test_header_contains_full_runtime_accounting(self) -> None:
        text = HEADER.read_text(encoding="utf-8")
        for needle in (
            "SourceIdentityMatches", "recording_sha256", "BeginTick",
            "control.reserved[0] == kFramePermitDisarmed",
            "AcknowledgeWriter", "CommitTick", "wrapper_entries != expected",
            "original_calls != expected", "clean_returns != expected",
            "equal_frames + evidence.corrected_frames != expected",
            "correction_writes != evidence.corrected_frames * 2u",
            "evidence.failures != 0", "evidence.recursive_entries != 0",
        ):
            self.assertIn(needle, text)
        for forbidden in ("ptrace", "pwrite", "process_vm_writev", "dlopen"):
            self.assertNotIn(forbidden, text)


if __name__ == "__main__":
    unittest.main()
