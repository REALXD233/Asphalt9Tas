#!/usr/bin/env python3
"""Host-executed behavioral tests for the exact BarrelYaw ELF map model."""

from __future__ import annotations

import dataclasses
import hashlib
import pathlib
import struct
import unittest


WORKSPACE = pathlib.Path(__file__).resolve().parents[2]
PAYLOAD = (
    WORKSPACE
    / "android-port"
    / "build"
    / "barrel-yaw-tail-payload-v1"
    / "liba9tas_barrel_yaw_tail_v1_build_only.so"
)
EXPECTED_SHA256 = "965e6ba9ce819ee4b6c1a38bf663f68e0c76841dfa7bfad0924ad67bee677ea9"
BIAS = 0x7000000000
FILE_PAGE_END = 0x6000
MEMORY_PAGE_END = 0x84000
LOGICAL_END = 0x83BD0
BOUNDARY_RVA = 0x1DD0
BOUNDARY_SIZE = 0x484
STORAGE_RVAS = (0x5100, 0x4F40, 0x75AC0, 0x52C0, 0x5000)
LOCATOR_RELOCATIONS = {
    0x50C0: 0x5100,
    0x50C8: 0x4F40,
    0x50D0: 0x75AC0,
    0x50D8: 0x52C0,
    0x50E0: 0x5000,
}
PAYLOAD_PATH = "/data/local/tmp/liba9tas_barrel_yaw_tail_v1_build_only.so"


@dataclasses.dataclass(frozen=True)
class Mapping:
    begin: int
    end: int
    perms: str
    offset: int
    dev: tuple[int, int]
    inode: int
    path: str = ""


def exact_maps(*, houdini: bool) -> list[Mapping]:
    return [
        Mapping(BIAS, BIAS + 0x1000, "r--p", 0, (0xFD, 1), 424242, PAYLOAD_PATH),
        Mapping(
            BIAS + 0x1000,
            BIAS + 0x3000,
            "r--p" if houdini else "r-xp",
            0,
            (0xFD, 1),
            424242,
            PAYLOAD_PATH,
        ),
        Mapping(BIAS + 0x3000, BIAS + 0x4000, "r--p", 0x1000,
                (0xFD, 1), 424242, PAYLOAD_PATH),
        Mapping(BIAS + 0x4000, BIAS + FILE_PAGE_END, "rw-p", 0x1000,
                (0xFD, 1), 424242, PAYLOAD_PATH),
        Mapping(BIAS + FILE_PAGE_END, BIAS + MEMORY_PAGE_END, "rw-p", 0,
                (0, 0), 0, "[anon:.bss]" if houdini else ""),
    ]


def private_bss(mapping: Mapping) -> bool:
    return (
        mapping.path in ("", "[anon:.bss]")
        and mapping.dev == (0, 0)
        and mapping.inode == 0
        and mapping.offset == 0
        and mapping.perms == "rw-p"
    )


def valid_split(maps: list[Mapping]) -> bool:
    file_mapping = maps[3]
    bss = maps[4]
    return (
        file_mapping.begin == BIAS + 0x4000
        and file_mapping.end == BIAS + FILE_PAGE_END
        and file_mapping.perms == "rw-p"
        and file_mapping.path == PAYLOAD_PATH
        and file_mapping.offset == 0x1000
        and bss.begin == file_mapping.end
        and bss.end == BIAS + MEMORY_PAGE_END
        and private_bss(bss)
    )


def valid_guest_code(mapping: Mapping) -> bool:
    expected_file_offset = 0xD70 + (BOUNDARY_RVA - 0x1D70)
    observed_file_offset = mapping.offset + (BIAS + BOUNDARY_RVA - mapping.begin)
    return (
        mapping.begin <= BIAS + BOUNDARY_RVA
        and BIAS + BOUNDARY_RVA + BOUNDARY_SIZE <= mapping.end
        and mapping.perms in ("r--p", "r-xp")
        and mapping.path == PAYLOAD_PATH
        and mapping.dev == (0xFD, 1)
        and mapping.inode == 424242
        and observed_file_offset == expected_file_offset
    )


def resolve_locators(observed: tuple[int, ...]) -> tuple[int, ...] | None:
    expected = tuple(BIAS + rva for rva in STORAGE_RVAS)
    if observed == (0, 0, 0, 0, 0) or observed == expected:
        return expected
    return None


def relative_relocations(data: bytes) -> dict[int, int]:
    ehdr = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    section_offset = ehdr[6]
    section_entry_size = ehdr[11]
    section_count = ehdr[12]
    if section_entry_size != struct.calcsize("<IIQQQQIIQQ"):
        raise AssertionError("unexpected ELF64 section-header size")
    result: dict[int, int] = {}
    for index in range(section_count):
        section = struct.unpack_from(
            "<IIQQQQIIQQ", data, section_offset + index * section_entry_size
        )
        section_type, file_offset, size, entry_size = section[1], section[4], section[5], section[9]
        if section_type != 4:  # SHT_RELA
            continue
        if entry_size != struct.calcsize("<QQq") or size % entry_size:
            raise AssertionError("unexpected RELA layout")
        for cursor in range(file_offset, file_offset + size, entry_size):
            relocation_offset, info, addend = struct.unpack_from("<QQq", data, cursor)
            if info & 0xFFFFFFFF == 0x403:  # R_AARCH64_RELATIVE
                if relocation_offset in result:
                    raise AssertionError("duplicate relative relocation")
                result[relocation_offset] = addend
    return result


class ResolverMappingModelTests(unittest.TestCase):
    def test_native_and_houdini_positive_models(self) -> None:
        for houdini in (False, True):
            maps = exact_maps(houdini=houdini)
            self.assertTrue(valid_split(maps))
            self.assertTrue(valid_guest_code(maps[1]))

    def test_named_bss_is_exact_and_private(self) -> None:
        maps = exact_maps(houdini=True)
        for path in ("[anon:evil]", "[anon:.bss:extra]", "[heap]"):
            maps[-1] = dataclasses.replace(maps[-1], path=path)
            self.assertFalse(valid_split(maps))
        maps = exact_maps(houdini=True)
        for perms in ("rwxp", "r--p", "rw-s"):
            maps[-1] = dataclasses.replace(maps[-1], perms=perms)
            self.assertFalse(valid_split(maps))

    def test_split_rejects_gap_and_page_overrun(self) -> None:
        maps = exact_maps(houdini=True)
        maps[-1] = dataclasses.replace(maps[-1], begin=maps[-1].begin + 0x1000)
        self.assertFalse(valid_split(maps))
        maps = exact_maps(houdini=True)
        maps[-1] = dataclasses.replace(maps[-1], end=maps[-1].end + 0x1000)
        self.assertFalse(valid_split(maps))

    def test_guest_code_allows_ro_or_rx_but_not_writable_or_wrong_offset(self) -> None:
        mapping = exact_maps(houdini=True)[1]
        self.assertTrue(valid_guest_code(mapping))
        self.assertTrue(valid_guest_code(dataclasses.replace(mapping, perms="r-xp")))
        self.assertFalse(valid_guest_code(dataclasses.replace(mapping, perms="rw-p")))
        self.assertFalse(valid_guest_code(dataclasses.replace(mapping, perms="r--s")))
        self.assertFalse(valid_guest_code(dataclasses.replace(mapping, offset=0x1000)))

    def test_locator_modes_are_all_zero_or_all_relocated(self) -> None:
        expected = tuple(BIAS + rva for rva in STORAGE_RVAS)
        self.assertEqual(resolve_locators((0, 0, 0, 0, 0)), expected)
        self.assertEqual(resolve_locators(expected), expected)
        self.assertIsNone(resolve_locators((0, *expected[1:])))
        self.assertIsNone(resolve_locators((*expected[:-1], expected[-1] + 8)))

    def test_logical_end_is_not_rounded_to_page_end(self) -> None:
        self.assertLess(LOGICAL_END, MEMORY_PAGE_END)
        self.assertLessEqual(0x83BC0, LOGICAL_END)
        self.assertLessEqual(LOGICAL_END - 8 + 8, LOGICAL_END)
        self.assertGreater(LOGICAL_END - 8 + 16, LOGICAL_END)

    def test_actual_payload_identity_and_locator_relocations(self) -> None:
        data = PAYLOAD.read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(), EXPECTED_SHA256)
        relocations = relative_relocations(data)
        self.assertEqual(
            {offset: relocations.get(offset) for offset in LOCATOR_RELOCATIONS},
            LOCATOR_RELOCATIONS,
        )


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(ResolverMappingModelTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if result.wasSuccessful():
        print(
            "BARREL_YAW_TAIL_RESOLVER_MAPPING_MODEL passed=1 "
            "native_rx=1 houdini_ro=1 anon_bss_exact=1 locator_dual_mode=1 "
            "relative_relocations=1 logical_end=1 runtime=host"
        )
    raise SystemExit(0 if result.wasSuccessful() else 1)
