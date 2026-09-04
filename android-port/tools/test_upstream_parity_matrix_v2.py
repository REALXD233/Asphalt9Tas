#!/usr/bin/env python3
"""Prevent upstream-source facts from being reported as Android live parity."""

from __future__ import annotations

import pathlib
import unittest


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
CORRECTED = WORKSPACE / "android-port" / "UPSTREAM_PARITY_MATRIX_CORRECTED_20260820.md"
LEGACY = WORKSPACE / "android-port" / "UPSTREAM_PARITY_MATRIX_20260818.md"
STATUS = WORKSPACE / "android-port" / "CURRENT_STATUS_20260817.md"


def matrix_rows(text: str) -> dict[str, list[str]]:
    rows: dict[str, list[str]] = {}
    for line in text.splitlines():
        if not line.startswith("| ") or line.startswith("| ---"):
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        if cells[0] == "Capability":
            continue
        if len(cells) == 6:
            rows[cells[0]] = cells
    return rows


class CorrectedParityMatrixTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = CORRECTED.read_text(encoding="utf-8")
        cls.legacy = LEGACY.read_text(encoding="utf-8")
        cls.status = STATUS.read_text(encoding="utf-8")
        cls.rows = matrix_rows(cls.text)

    def test_legacy_matrix_is_unambiguously_superseded(self) -> None:
        first_lines = "\n".join(self.legacy.splitlines()[:12])
        self.assertIn("SUPERSEDED 2026-08-20", first_lines)
        self.assertIn(CORRECTED.name, first_lines)

    def test_axes_are_separate_and_old_status_is_forbidden(self) -> None:
        self.assertIn("| Capability | Upstream contract | Android evidence | Faithful integration |", self.text)
        matrix = self.text.split("## Corrected matrix", 1)[1].split("## Direction verdict", 1)[0]
        self.assertNotIn("LIVE_PROVEN", matrix)
        allowed_android = {"LIVE_BOUNDED", "LIVE_OBSERVE_ONLY", "BUILD_ONLY", "NONE"}
        allowed_faithful = {"PROVEN_SUBSET", "PARTIAL", "PREPARATORY", "MISSING", "PRODUCT_MISSING"}
        self.assertGreaterEqual(len(self.rows), 25)
        for cells in self.rows.values():
            self.assertEqual(cells[1], "UPSTREAM_CONFIRMED")
            self.assertIn(cells[2], allowed_android)
            self.assertIn(cells[3], allowed_faithful)

    def test_defining_missing_semantics_cannot_be_overclaimed(self) -> None:
        expected = {
            "Tick packet selection and empty-buffer blocking": ("LIVE_OBSERVE_ONLY", "PARTIAL"),
            "Accelerator": ("NONE", "MISSING"),
            "Nitro activation count": ("LIVE_OBSERVE_ONLY", "MISSING"),
            "Respawn edge": ("NONE", "MISSING"),
            "Determinism / multi-run equivalence": ("NONE", "MISSING"),
            "User interface and editor": ("NONE", "PRODUCT_MISSING"),
        }
        for capability, statuses in expected.items():
            self.assertIn(capability, self.rows)
            self.assertEqual(tuple(self.rows[capability][2:4]), statuses)

    def test_upstream_future_tick_gap_is_not_idealized_away(self) -> None:
        row = " ".join(self.rows["Tick packet selection and empty-buffer blocking"])
        self.assertIn("spins while the queue is empty", row)
        self.assertIn("future-tick head exits without consuming", row)
        self.assertIn("zero gameplay writes/actions", row)
        self.assertIn("gameplay-value transport were not exercised", row)

    def test_nitro_and_callback_semantics_are_explicit(self) -> None:
        nitro = " ".join(self.rows["Nitro colour outcome"])
        self.assertIn("activation count", nitro)
        self.assertIn("without direct state writes", nitro)
        callback = self.rows["Natural callback route FC-1 to FC-3"]
        self.assertEqual(callback[2:4], ["BUILD_ONLY", "PREPARATORY"])
        self.assertIn("do not count it as replay completion", callback[5])

    def test_current_status_points_to_the_correction(self) -> None:
        self.assertIn(CORRECTED.name, self.status)
        self.assertIn("old", self.status)
        self.assertIn("cannot be used as an Android", self.status)


if __name__ == "__main__":
    unittest.main()
