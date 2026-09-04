#!/usr/bin/env python3
"""Offline regression for the hash-pinned Android race lifecycle resolver."""

from __future__ import annotations

import pathlib
import unittest

import resolve_race_lifecycle_static_v1 as resolver


class RaceLifecycleStaticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.result = resolver.resolve(resolver.DEFAULT_LIBRARY)

    def test_authoritative_phase_transition(self) -> None:
        self.assertEqual(self.result["schema"], "A9RLS1")
        self.assertEqual(self.result["binary_sha256"], resolver.EXPECTED_SHA256)
        self.assertEqual(self.result["device_access"], 0)
        self.assertEqual(self.result["gameplay_writes"], 0)
        self.assertEqual(
            self.result["phase_state"],
            {"field_offset": "0x2d8", "intro": 1,
             "countdown": 2, "racing": 3},
        )
        self.assertEqual(self.result["rvas"]["phase_gate"], "0x3a5955c")
        self.assertEqual(
            self.result["rvas"]["racing_phase_entry"], "0x3a59578")
        self.assertEqual(
            self.result["rvas"]["racing_state_store"], "0x3a596c4")

    def test_complete_vtable_family(self) -> None:
        vtable = self.result["vtable"]
        self.assertEqual(vtable["phase_gate_slot"], "0x1d8")
        self.assertEqual(vtable["phase_enter_slot"], "0x1e0")
        self.assertEqual(vtable["candidate_count"], 294)
        self.assertEqual(
            vtable["implementations"],
            [
                {"rva": "0x382eb04", "vtable_count": 10},
                {"rva": "0x3a59578", "vtable_count": 284},
            ],
        )
        self.assertEqual(len(vtable["candidates"]), 294)
        self.assertEqual(vtable["candidates"][0]["vptr_rva"], "0x7f3ee98")

    def test_unique_matcher_rejects_ambiguity(self) -> None:
        data = b"\x7fELF" + b"A" * 60 + b"needle--needle"
        segment = resolver.LoadSegment(64, 0x1000, 14, 14, resolver.PF_X)
        with self.assertRaisesRegex(ValueError, "instead of 1"):
            resolver.find_unique_executable(
                data, [segment], b"needle", "synthetic")


if __name__ == "__main__":
    unittest.main()
