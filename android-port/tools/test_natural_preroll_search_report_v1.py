#!/usr/bin/env python3
"""Basic ABI and fail-closed tests for A9NPR1."""

from __future__ import annotations

import unittest

from natural_preroll_search_report_v1 import HEADER_SIZE, _HEADER, verify_search_report


class NaturalPrerollSearchReportTests(unittest.TestCase):
    def test_header_abi_is_fixed(self) -> None:
        self.assertEqual(HEADER_SIZE, 200)
        self.assertEqual(_HEADER.size, HEADER_SIZE)

    def test_short_report_fails_closed_before_anchor_use(self) -> None:
        with self.assertRaisesRegex(ValueError, "shorter"):
            verify_search_report(b"", None)  # type: ignore[arg-type]


if __name__ == "__main__":
    unittest.main()
