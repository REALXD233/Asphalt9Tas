#!/usr/bin/env python3
"""Unit tests for the read-only ARM64 vtable-call scanner."""

from __future__ import annotations

import struct
import unittest

from scan_arm64_vslot_calls import scan


def encode_ldr_x_unsigned(target: int, base: int, byte_offset: int) -> int:
    return 0xF9400000 | ((byte_offset // 8) << 10) | (base << 5) | target


def encode_blr(target: int) -> int:
    return 0xD63F0000 | (target << 5)


def make_elf(words: list[int], virtual_address: int = 0x1000) -> bytes:
    code = struct.pack(f"<{len(words)}I", *words)
    blob = bytearray(0x100 + len(code))
    blob[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<Q", blob, 0x20, 0x40)
    struct.pack_into("<H", blob, 0x36, 56)
    struct.pack_into("<H", blob, 0x38, 1)
    struct.pack_into(
        "<IIQQQQQQ",
        blob,
        0x40,
        1,
        1,
        0x100,
        virtual_address,
        virtual_address,
        len(code),
        len(code),
        4,
    )
    blob[0x100:] = code
    return bytes(blob)


class ScanArm64VslotCallsTests(unittest.TestCase):
    def test_accepts_linear_vtable_call(self) -> None:
        blob = make_elf([
            encode_ldr_x_unsigned(8, 9, 0x58),
            0xD503201F,
            encode_blr(8),
        ])
        matches = scan(blob, 0x58, 4)
        self.assertEqual([(m.load_address, m.call_address) for m in matches], [("0x1000", "0x1008")])

    def test_rejects_target_register_clobber(self) -> None:
        blob = make_elf([
            encode_ldr_x_unsigned(8, 9, 0x58),
            encode_ldr_x_unsigned(8, 10, 0x68),
            encode_blr(8),
        ])
        self.assertEqual(scan(blob, 0x58, 4), [])

    def test_rejects_stack_relative_false_positive(self) -> None:
        blob = make_elf([
            encode_ldr_x_unsigned(8, 31, 0x58),
            encode_blr(8),
        ])
        self.assertEqual(scan(blob, 0x58, 2), [])

    def test_address_range_is_applied_to_load(self) -> None:
        blob = make_elf([
            encode_ldr_x_unsigned(8, 9, 0x58),
            encode_blr(8),
        ])
        self.assertEqual(len(scan(blob, 0x58, 2, start_address=0x1000, end_address=0x1004)), 1)
        self.assertEqual(scan(blob, 0x58, 2, start_address=0x1004, end_address=0x2000), [])

    def test_optional_preceding_context_does_not_change_match_validation(self) -> None:
        blob = make_elf([
            0x52800023,  # mov w3, #1
            encode_ldr_x_unsigned(8, 9, 0x58),
            0xD503201F,
            encode_blr(8),
        ])
        matches = scan(blob, 0x58, 4, context_before=1)
        self.assertEqual(len(matches), 1)
        self.assertEqual(matches[0].instructions[0], "0x1000: mov w3, #1")


if __name__ == "__main__":
    unittest.main()
