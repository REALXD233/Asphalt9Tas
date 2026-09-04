#!/usr/bin/env python3
"""Static policy for the frozen-thread G2 install/status/restore controller."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_source(text: str) -> None:
    required = (
        '#define A9TAS_HABI1_CONTROLLER_CORE_ONLY 1',
        'kTargetRva = 0x3695474',
        'kStepOptionsThisAdjustment = -0x2A78',
        'kImplementationOwnerVtableRva = 0x7EECD88',
        'kInstallTag = 0x47324901',
        'kRestoreTag = 0x47324902',
        'FreezeStable(pid, call_tid, &frozen)',
        'PTRACE_SEIZE',
        'PTRACE_INTERRUPT',
        'stable_passes < 2',
        'CallGuest(pid, &runtime, &frozen',
        'kInstallAndArm',
        'protocol::Command::kRestore',
        'KillUncertainProcess(pid)',
        'DetachAll(&frozen)',
        'ReadPinnedRegularFile(game->path.c_str()',
        '671522d4614abcce5c4da16ff8a177423fa67f3eace7b6f0652e9754403008f0',
        'e5dd7ef24f52dff0e0040dc3b1320f267a3c3b3b',
        'runtime->owner_vptr != requested_game_base +',
        'interval::kStepOptionsVtableRva',
        'this_adjustment != kStepOptionsThisAdjustment',
        'control.expected_owner = runtime.owner',
        'I_ACCEPT_G2_PHYSICS_INTERVAL_V1',
    )
    forbidden = (
        'process_vm_writev',
        'pthread_create(',
        'std::thread(',
        'SIGCONT',
        'PTRACE_CONT, frozen->other_tids',
    )
    missing = [item for item in required if item not in text]
    present = [item for item in forbidden if item in text]
    if missing:
        raise ValueError(f"missing controller marker: {missing[0]}")
    if present:
        raise ValueError(f"forbidden controller marker: {present[0]}")
    freeze = text.find('FreezeStable(pid, call_tid, &frozen)')
    branch = text.find('if (action == Action::kInstall)', freeze)
    call = text.find('CallGuest(pid, &runtime, &frozen', branch)
    if not (0 <= freeze < branch < call):
        raise ValueError("guest mutation is not dominated by stable freeze")
    if text.count('CallGuest(pid, &runtime, &frozen') != 2:
        raise ValueError("controller must expose exactly install+restore guest calls")


def run(command: list[str]) -> str:
    return subprocess.run(command, check=True, capture_output=True,
                          text=True).stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--readelf', type=Path, required=True)
    parser.add_argument('--nm', type=Path, required=True)
    parser.add_argument('--payload-path', required=True)
    parser.add_argument('--payload-sha256', required=True)
    parser.add_argument('--payload-build-id', required=True)
    parser.add_argument('--bootstrap-path', required=True)
    parser.add_argument('--bootstrap-sha256', required=True)
    parser.add_argument('--bootstrap-build-id', required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    try:
        text = args.source.read_text(encoding='utf-8')
        verify_source(text)
        identity = run([str(args.readelf), '-h', '-n', str(args.elf)])
        if 'Machine:                           Advanced Micro Devices X86-64' not in identity or \
           'Type:                              DYN (Shared object file)' not in identity:
            raise ValueError('controller ELF identity mismatch')
        match = re.search(r'Build ID: ([0-9a-f]{40})', identity)
        if not match:
            raise ValueError('controller build-id missing')
        undefined = run([str(args.nm), '-u', str(args.elf)])
        for symbol in ('ptrace', 'pread', 'pwrite', 'waitpid'):
            if symbol not in undefined:
                raise ValueError(f'missing required host primitive: {symbol}')
        raw = args.elf.read_bytes()
        for value in (args.payload_path, args.payload_sha256,
                      args.payload_build_id, args.bootstrap_path,
                      args.bootstrap_sha256, args.bootstrap_build_id,
                      'I_ACCEPT_G2_PHYSICS_INTERVAL_V1'):
            if value.encode() not in raw:
                raise ValueError(f'missing embedded identity: {value}')
        report = {
            'schema': 'a9tas-g2-physics-interval-controller-policy-v1',
            'passed': 1,
            'source_sha256': sha256(args.source),
            'elf_sha256': sha256(args.elf),
            'build_id': match.group(1),
            'stable_thread_freeze': 1,
            'guest_calls': ['install', 'restore'],
            'status_guest_calls': 0,
            'failure_kills_uncertain_process': 1,
        }
        args.report.write_text(json.dumps(report, indent=2) + '\n',
                               encoding='utf-8')
    except (OSError, subprocess.CalledProcessError, ValueError) as exc:
        print(f'G2_CONTROLLER_POLICY passed=0 reason={exc}')
        return 1
    print('G2_CONTROLLER_POLICY passed=1 stable_freeze=1 install_calls=1 restore_calls=1')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
