from pathlib import Path
import sys


text = Path(sys.argv[1]).read_text(encoding="utf-8")

required = (
    '[ValidateSet("OfflineValidate", "ReplayGate")]',
    "AcknowledgeFiveCallSameBitsReplayGate",
    "Assert-RemoteReplayInputEntrypoint",
    '"replay 5 $RemoteInput $ProbeOutput $ack"',
    '$limit = 5',
    '$expectedInputHash = "d4f4758ab026d2b99bf356b430a449a401ecb9c6378354f32299b1c2c1a35c02"',
    '--profile car-physics',
    'Invoke-Transaction "preflight"',
    'Invoke-Transaction "install"',
    'input keyevent 111',
    'Invoke-Transaction "status"',
    'complete=1',
    'Invoke-Transaction "finalize"',
    'Invoke-Transaction "rollback"',
    'evidence.replay_calls -ne $limit',
    'evidence.overrides -ne $limit',
    '$_.flags -ne "0x3f"',
    '$_.original_bits -ne $_.requested_bits',
    'TracerPid',
)
for token in required:
    if token not in text:
        raise SystemExit(f"missing same-bits Gate invariant: {token}")

if text.count("input keyevent 111") != 1:
    raise SystemExit("same-bits Gate must contain exactly one automatic ESC")
if text.count('Invoke-Transaction "install"') != 1:
    raise SystemExit("same-bits Gate must contain exactly one install site")
for forbidden in ("fixed-delta", "steering", "nitro", "camera"):
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
    raise SystemExit("same-bits Gate phase order is invalid")

print("PHYSICS_INTERVAL_SAME_BITS_GATE_V2_POLICY passed=1 replay_only=1 limit=5 input_pinned=1 auto_esc_max=1 same_process_retry=0")
