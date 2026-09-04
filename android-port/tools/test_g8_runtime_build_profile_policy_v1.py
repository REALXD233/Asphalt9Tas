#!/usr/bin/env python3
"""Static policy guard for the G8 runtime-profile ABI and compiler."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    header = (ROOT / "src/g8_runtime_build_profile_v1.h").read_text(
        encoding="utf-8"
    )
    generator = (ROOT / "tools/generate_g8_runtime_build_profile_v1.py").read_text(
        encoding="utf-8"
    )
    required_header = (
        "struct alignas(64) Profile",
        "static_assert(sizeof(Profile) == 384",
        "kLiveObjectGraphRequired",
        "kChannelLiteralsAbsent",
        "profile.image_size",
        "ReferenceProfile",
        "barrel_random_bool_rva",
        "barrel_random_lerp_rva",
    )
    required_generator = (
        "A9_BUILD_PROFILE_RESOLUTION_V1",
        "STATIC_STAGE2_PASS_LIVE_READONLY_REQUIRED",
        'document.get("write_authorized") is not False',
        'document.get("channel_specific_literals") != 0',
        'document.get("pending") != ["live_object_graph_readback"]',
        'ANNEX_MAGIC = b"A9BPAX1\\0"',
        "VEHICLE_RVA_COUNT = 42",
        'relationships.get("lifecycle_vtables")',
        "os.replace(temporary_name, path)",
        '"logic_dispatcher"',
        'role_rva("barrel_random_bool")',
        'role_rva("barrel_random_lerp")',
        "bytes(44)",
    )
    missing = [token for token in required_header if token not in header]
    missing += [token for token in required_generator if token not in generator]
    if missing:
        raise SystemExit(f"G8 runtime build-profile policy missing: {missing}")
    forbidden = (
        "com.aligames",
        "com.huawei",
        "kuang.kybc",
        "process_vm_writev",
        "pwrite(",
        "ptrace(",
        "adb ",
    )
    present = [token for token in forbidden if token in header or token in generator]
    if present:
        raise SystemExit(f"G8 runtime profile contains channel/live coupling: {present}")
    print(
        "G8_RUNTIME_BUILD_PROFILE_POLICY passed=1 abi_size=384 "
        "channel_literals=0 device_access=0 write_authorized=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
