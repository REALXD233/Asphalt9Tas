#!/usr/bin/env python3
"""Generate the guarded Barrel successor runner from the pinned live runner."""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path


BASE_SHA256 = "21324d72d753e0f3aa284acf753e539efeff722ebff88f05851b8690e7c30ae3"


def replace_one(source: str, old: str, new: str, label: str) -> str:
    count = source.count(old)
    if count != 1:
        raise RuntimeError(f"{label} occurrence count={count}")
    return source.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: generate_barrel_successor_runner_v1.py <base> <output>"
        )
    base = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    raw = base.read_bytes()
    if hashlib.sha256(raw).hexdigest() != BASE_SHA256:
        raise RuntimeError("live-proven runner hash drift")
    source = raw.decode("utf-8")
    replacements = (
        (
            "workspace_root",
            "$root = Split-Path -Parent $MyInvocation.MyCommand.Path",
            "$root = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) \"..\\..\"))",
        ),
        (
            "ack_parameter",
            "    [switch]$Acknowledge900PhysicsIntervalReplayCalls\n)",
            "    [switch]$Acknowledge900PhysicsIntervalReplayCalls,\n"
            "    [switch]$AcknowledgeBarrelPostNaturalWrites\n)",
        ),
        (
            "executor_build",
            '$buildScript = Join-Path $root "build-final-writer-natural-action-live-v1.ps1"',
            '$buildScript = Join-Path $root "build-barrel-successor-executor-v1.ps1"',
        ),
        (
            "executor_candidate",
            '$writerCandidate = Join-Path $root "build\\lifecycle-final-writer-natural-action-v1\\a9tas_lifecycle_final_writer_natural_action_v1_review_only"',
            '$writerCandidate = Join-Path $root "build\\barrel-successor-executor-v1\\a9tas_barrel_successor_v1_review_only"',
        ),
        (
            "runner_policy",
            '$policy = Join-Path $root "tools\\test_run_final_writer_natural_action_composite_policy_v1.py"',
            '$policy = Join-Path $root "tools\\test_run_barrel_successor_replay_policy_v1.py"',
        ),
        (
            "preload_build",
            '$tripleBuildScript = Join-Path $root "build-final-writer-natural-action-physics-interval-live-v1.ps1"',
            '$tripleBuildScript = Join-Path $root "build-barrel-successor-preload-v1.ps1"',
        ),
        (
            "preload_bootstrap",
            '$tripleBootstrap = Join-Path $root "build\\final-writer-natural-action-physics-interval-live-v1\\liba9tas_bootstrap_final_writer_natural_action_physics_interval_v1.so"',
            '$tripleBootstrap = Join-Path $root "build\\barrel-successor-preload-v1\\liba9tas_bootstrap_final_writer_barrel_successor_v1.so"\n'
            '$successorBundle = Join-Path $root "build\\barrel-successor-preload-v1\\liba9tas_payload_bundle_barrel_successor_v1.so"',
        ),
        (
            "preload_helper",
            '$tripleHelper = Join-Path $root "tools\\run_final_writer_natural_action_physics_interval_preload_v1.sh"',
            '$tripleHelper = Join-Path $root "tools\\run_barrel_successor_preload_v1.sh"',
        ),
        (
            "barrel_payload",
            '$intervalPayload = Join-Path $root "build\\physics-interval-getter-payload-v2\\liba9tas_physics_interval_getter_v2_passive.so"',
            '$intervalPayload = Join-Path $root "build\\physics-interval-getter-payload-v2\\liba9tas_physics_interval_getter_v2_passive.so"\n'
            '$barrelPayload = Join-Path $root "build\\barrel-yaw-tail-payload-v1\\liba9tas_barrel_yaw_tail_v1_build_only.so"',
        ),
        (
            "live_requires_exact_interval",
            '$intervalReplayExpectedHash = "329683a976ad7d0e3f7d326c567fbdfaf911be1fc6bca5573087de2e11bf6347"',
            '$intervalReplayExpectedHash = "329683a976ad7d0e3f7d326c567fbdfaf911be1fc6bca5573087de2e11bf6347"\n'
            'if ($Mode -ne "OfflineValidate" -and -not $EnablePhysicsIntervalReplay) {\n'
            '    throw "Barrel successor requires the exact accepted 900-value Physics Interval replay"\n'
            '}',
        ),
        (
            "bootstrap_pin",
            '"e87f2f8a7aca73f3d3cf83b7a8af6be51de1ccbdf5c4eb60e387354362e09cf5"',
            '"dcd30a2f597ada1214644f283d9632273d43d6fdfb5bdd391490295d8e9a815e"',
        ),
        (
            "candidate_pin",
            '$writerCandidate = "7ac3464bac63fe495a07c5e74f495099fe45613e0c573f32abdae12a14520f96"',
            '$writerCandidate = "4475ed1a2c136c1bdaccd0c5274634a876aeee8adc323a4911a8d903d5c0594c"\n'
            '    $successorBundle = "dd32337b5017b5187a7d0603cbb427ea2807e5787ab8a927d2227b54525e5342"\n'
            '    $barrelPayload = "965e6ba9ce819ee4b6c1a38bf663f68e0c76841dfa7bfad0924ad67bee677ea9"',
        ),
        (
            "remote_barrel",
            '$remoteIntervalPayload = "/data/local/tmp/liba9tas_physics_interval_getter_v2_passive.so"',
            '$remoteIntervalPayload = "/data/local/tmp/liba9tas_physics_interval_getter_v2_passive.so"\n'
            '$remoteBarrelPayload = "/data/local/tmp/liba9tas_barrel_yaw_tail_v1_build_only.so"\n'
            '$remoteSuccessorBundle = "/data/local/tmp/liba9tas_payload_bundle_barrel_successor_v1.so"',
        ),
        (
            "remote_names",
            '$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_final_writer_natural_action_physics_interval_v1.so"\n'
            '    $remoteInjector = "/data/local/tmp/a9tas_injector_final_writer_natural_action_physics_interval_v1"\n'
            '    $remoteHelper = "/data/local/tmp/run_final_writer_natural_action_physics_interval_preload_v1.sh"',
            '$remoteBootstrap = "/data/local/tmp/liba9tas_bootstrap_final_writer_barrel_successor_v1.so"\n'
            '    $remoteInjector = "/data/local/tmp/a9tas_injector_barrel_successor_v1"\n'
            '    $remoteHelper = "/data/local/tmp/run_barrel_successor_preload_v1.sh"',
        ),
        (
            "required_inputs",
            '$requiredInputs += @($tripleBuildScript, $intervalPayload,\n                         $intervalController, $intervalObserver,',
            '$requiredInputs += @($tripleBuildScript, $successorBundle,\n                         $intervalPayload, $barrelPayload,\n                         $intervalController, $intervalObserver,',
        ),
        (
            "preload_pairs",
            '$preloadPairs += ,@($intervalPayload, $remoteIntervalPayload)\n        $preloadPairs += ,@($intervalController, $remoteIntervalController)',
            '$preloadPairs += ,@($intervalPayload, $remoteIntervalPayload)\n'
            '        $preloadPairs += ,@($barrelPayload, $remoteBarrelPayload)\n'
            '        $preloadPairs += ,@($successorBundle, $remoteSuccessorBundle)\n'
            '        $preloadPairs += ,@($intervalController, $remoteIntervalController)',
        ),
        (
            "preload_permissions",
            '"su -c \'chmod 644 $remoteIntervalPayload; chmod 700 $remoteIntervalController $remoteIntervalObserver\'"',
            '"su -c \'chmod 644 $remoteIntervalPayload $remoteBarrelPayload $remoteSuccessorBundle; chmod 700 $remoteIntervalController $remoteIntervalObserver\'"',
        ),
        (
            "prepare_mapping",
            '$mapped = $mapped -and $maps.Contains($remoteIntervalPayload)',
            '$mapped = $mapped -and $maps.Contains($remoteIntervalPayload) -and\n'
            '                      $maps.Contains($remoteBarrelPayload) -and\n'
            '                      $maps.Contains($remoteSuccessorBundle)',
        ),
        (
            "receipt_barrel",
            'bootstrap_sha256=$pins[$bootstrap];\n        physics_interval_enabled=$physicsIntervalEnabled;',
            'bootstrap_sha256=$pins[$bootstrap];\n'
            '        successor_bundle_sha256=$pins[$successorBundle];\n'
            '        barrel_payload_sha256=$pins[$barrelPayload];\n'
            '        physics_interval_enabled=$physicsIntervalEnabled;',
        ),
        (
            "payload_count",
            '$payloadCount = if ($physicsIntervalEnabled) { 3 } else { 2 }',
            '$payloadCount = if ($physicsIntervalEnabled) { 4 } else { 2 }',
        ),
        (
            "write_ack",
            "@($Acknowledge900FrameCompositeWrites, '900 fixed-delta/control/final-writer frames plus 900 natural-action commands'),",
            "@($Acknowledge900FrameCompositeWrites, '900 fixed-delta/control/final-writer frames plus 900 natural-action commands'),\n"
            "    @($AcknowledgeBarrelPostNaturalWrites, 'source-faithful BarrelRoll and BarrelYaw post-natural writes'),",
        ),
        (
            "receipt_validation",
            '$prepared.bootstrap_sha256 -ne $pins[$bootstrap] -or\n    [bool]$prepared.physics_interval_enabled',
            '$prepared.bootstrap_sha256 -ne $pins[$bootstrap] -or\n'
            '    $prepared.successor_bundle_sha256 -ne $pins[$successorBundle] -or\n'
            '    $prepared.barrel_payload_sha256 -ne $pins[$barrelPayload] -or\n'
            '    [bool]$prepared.physics_interval_enabled',
        ),
        (
            "execute_mapping",
            '($physicsIntervalEnabled -and\n     -not $maps.Contains($remoteIntervalPayload))) {',
            '($physicsIntervalEnabled -and\n'
            '     (-not $maps.Contains($remoteIntervalPayload) -or\n'
            '      -not $maps.Contains($remoteBarrelPayload) -or\n'
            '      -not $maps.Contains($remoteSuccessorBundle)))) {',
        ),
        (
            "success_label",
            'FINAL_WRITER_NATURAL_ACTION_COMPOSITE_LIVE_PASSED',
            'BARREL_SUCCESSOR_REPLAY_LIVE_PASSED',
        ),
    )
    for label, old, new in replacements:
        source = replace_one(source, old, new, label)
    source = (
        "# Generated from the pinned live-proven composite runner.\n"
        "# The canonical runner remains immutable.\n" + source
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_text(source, encoding="utf-8", newline="\n")
    temporary.replace(output)
    print(
        "BARREL_SUCCESSOR_RUNNER_GENERATOR passed=1 replacements="
        f"{len(replacements)} base_unchanged=1 output_sha256="
        f"{hashlib.sha256(source.encode('utf-8')).hexdigest()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
