#!/usr/bin/env python3
"""Regression for the LDPlayer-sized controller mapping adapter."""

from __future__ import annotations

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "gameplay_input_controller_mapping_adapter_v1.h"


def convert(mappings: list[tuple[int, int, str]]) -> list[tuple[int, int]]:
    output: list[tuple[int, int]] = []
    for begin, end, perms in mappings:
        if (len(perms) >= 4 and perms[0] == "r" and perms[1] == "w" and
                perms[2] != "x" and perms[3] == "p"):
            output.append((begin, end))
    if not output or len(output) > 4096:
        return []
    previous_end = 0
    for begin, end in output:
        if begin == 0 or begin >= end or begin % 8 or end % 8 or begin < previous_end:
            return []
        previous_end = end
    return output


class MappingAdapterTests(unittest.TestCase):
    def test_ldplayer_7075_shape_filters_under_strict_bound(self) -> None:
        mappings = []
        cursor = 0x10000
        for index in range(7075):
            mappings.append((cursor, cursor + 0x1000,
                             "rw-p" if index % 3 == 0 else "r--p"))
            cursor += 0x1000
        output = convert(mappings)
        self.assertEqual(len(output), 2359)
        self.assertLessEqual(len(output), 4096)

    def test_non_object_mappings_are_not_required_by_resolver(self) -> None:
        self.assertEqual(convert([(0x10000, 0x11000, "r--p")]), [])
        self.assertEqual(convert([(0x10000, 0x11000, "r-xp")]), [])
        self.assertEqual(convert([(0x10000, 0x11000, "rw-s")]), [])

    def test_overlap_still_fails_closed(self) -> None:
        self.assertEqual(convert([
            (0x10000, 0x12000, "rw-p"),
            (0x11000, 0x13000, "rw-p"),
        ]), [])

    def test_cpp_adapter_owns_only_the_filter(self) -> None:
        source = HEADER.read_text(encoding="utf-8")
        self.assertIn("!private_mapping", source)
        self.assertIn("mapping.perms[3] == 'p'", source)
        self.assertIn("resolver::MappingSnapshotValid", source)
        self.assertNotIn("kMaximumMappings =", source)


if __name__ == "__main__":
    unittest.main()
