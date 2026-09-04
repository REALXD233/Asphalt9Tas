from pathlib import Path
import sys


text = Path(sys.argv[1]).read_text(encoding="utf-8")

required = (
    '[ValidateSet("OfflineValidate", "RecordGate")]',
    "AcknowledgeFiveCallRecordGate",
    '$limit = 5',
    'Assert-RemoteControllerEntrypoint',
    'stage=process_identity',
    'Invoke-Transaction "preflight"',
    'game_writes=0',
    '--profile car-physics',
    'Invoke-Transaction "install"',
    'input keyevent 111',
    'Invoke-Transaction "status"',
    'complete=1',
    'Invoke-Transaction "finalize"',
    'Invoke-Transaction "rollback"',
    'evidence.overrides -ne 0',
    'TracerPid',
)
for token in required:
    if token not in text:
        raise SystemExit(f"missing Gate invariant: {token}")

if text.count("input keyevent 111") != 1:
    raise SystemExit("Gate must contain exactly one automatic ESC site")
if text.count('Invoke-Transaction "install"') != 1:
    raise SystemExit("Gate must contain exactly one install site")
for forbidden in ("replay", "fixed-delta", "steering", "nitro", "camera"):
    if forbidden in text.lower():
        raise SystemExit(f"forbidden combined-Gate marker: {forbidden}")

order = [
    text.index('--profile car-physics'),
    text.index('Invoke-Transaction "preflight"'),
    text.index('Invoke-Transaction "install"'),
    text.index('input keyevent 111'),
    text.index('Invoke-Transaction "status"'),
    text.index('Invoke-Transaction "finalize"'),
]
if order != sorted(order):
    raise SystemExit("Gate phase order is invalid")

print("PHYSICS_INTERVAL_SHADOW_GATE_V2_POLICY passed=1 record_only=1 limit=5 auto_esc_max=1 same_process_retry=0")
