#!/usr/bin/env python3
"""Negative source-policy tests for the G2 controller."""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path


def rejected(module, text: str) -> bool:
    try:
        module.verify_source(text)
    except ValueError:
        return True
    return False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('--verifier', type=Path, required=True)
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('g2_controller_policy',
                                                  args.verifier)
    if spec is None or spec.loader is None:
        raise SystemExit('cannot load verifier')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    source = args.source.read_text(encoding='utf-8')
    module.verify_source(source)
    cases = (
        rejected(module, source.replace('PTRACE_SEIZE', 'PTRACE_ATTACH_ONLY')),
        rejected(module, source.replace('stable_passes < 2', 'false')),
        rejected(module, source.replace('KillUncertainProcess(pid)', 'false')),
        rejected(module, source.replace('kTargetRva = 0x3695474',
                                        'kTargetRva = 0')),
        rejected(module, source.replace(
            'kStepOptionsThisAdjustment = -0x2A78',
            'kStepOptionsThisAdjustment = 0')),
        rejected(module, source.replace(
            'kImplementationOwnerVtableRva = 0x7EECD88',
            'kImplementationOwnerVtableRva = 0')),
        rejected(module, source + '\nprocess_vm_writev(0,0,0,0,0,0);\n'),
    )
    if not all(cases):
        print(f'G2_CONTROLLER_POLICY_SELFTEST passed=0 cases={sum(cases)}/{len(cases)}')
        return 1
    print(f'G2_CONTROLLER_POLICY_SELFTEST passed=1 negative_cases={len(cases)}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
