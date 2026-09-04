#!/usr/bin/env python3
"""Static fail-closed policy for the HABI-1 live runner."""

from __future__ import annotations

import argparse
import pathlib
import sys


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("runner", type=pathlib.Path)
    args = parser.parse_args()
    try:
        source = args.runner.read_text(encoding="utf-8")
        required = (
            '[ValidateSet("OfflineValidate", "ExecuteOnce")]',
            '[string]$Mode = "OfflineValidate"',
            "AcknowledgeDeviceAccessAndFixedArtifactPush",
            "AcknowledgeFreshDisposableGameProcess",
            "AcknowledgeNativeBridgeCallbackAndSinglePtraceCall",
            "AcknowledgeProcessTerminationOnSuccessOrUncertainty",
            "ExecuteExactlyOneAttempt",
            "build-hook-abi-selftest-habi1.ps1",
            "verify_known_good_900_exact_interval_v2.py",
            "habi1_manifest_f22b4083c94fd41a.json",
            'if ($Mode -eq \'OfflineValidate\')',
            "HABI1_LIVE_RUNNER_OFFLINE passed=1 device_access=0",
            "Expected ready device not found",
            '$remoteReceipt = "/data/user/0/$package/cache/a9tas-hook-abi-selftest-habi1.status"',
            '$remoteReceiptExport = "/data/local/tmp/a9tas-hook-abi-selftest-habi1.status.export"',
            'chmod 700 $remoteCarrier $remoteController; chmod 644 $remotePayload $remoteBootstrap',
            'Assert-RemoteMode $remotePayload "644"',
            'Assert-RemoteMode $remoteBootstrap "644"',
            "Assert-RemoteHash ([string]$m.carrier.libc_device_path)",
            "nohup $remoteCarrier >$remoteCarrierLog",
            "am start -n $activity",
            "$remoteController $gameProcessId $startTicks $nonce",
            "HABI1_ONE_SHOT passed=1",
            "am force-stop $package",
            "process success termination",
            "export payload receipt",
        )
        for token in required:
            require(token in source, f"runner policy token missing: {token}")
        for token in ("start-early-inject.sh", "deploy.ps1", "a9tas_injector",
                      "--wait-window", "libAsphalt9.so /data/local/tmp",
                      "chmod 600 $remotePayload", "chmod 600 $remoteBootstrap",
                      '$remoteReceipt = "/data/local/tmp/a9tas-hook-abi-selftest-habi1.status"'):
            require(token not in source, f"runner references forbidden legacy path: {token}")
        offline = source.index("if ($Mode -eq 'OfflineValidate')")
        device_check = source.index("Expected ready device not found")
        force_stop = source.index('am force-stop $package')
        require(offline < device_check < force_stop,
                "device mutation is not guarded behind OfflineValidate exit")
        require(source.count("$remoteController $gameProcessId $startTicks $nonce") == 1,
                "runner must contain exactly one controller execution")
        require("$pid" not in source.lower(),
                "runner must not shadow PowerShell's read-only PID variable")
        require(source.count("[switch]$ExecuteExactlyOneAttempt") == 1,
                "runner exact-attempt acknowledgement is not unique")
        print("HABI1_LIVE_RUNNER_POLICY passed=1 default_offline=1 "
              "attempts=1 device_access=0 deployed=0")
        return 0
    except (OSError, RuntimeError, ValueError) as error:
        print(f"HABI1_LIVE_RUNNER_POLICY passed=0 error={error}",file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
