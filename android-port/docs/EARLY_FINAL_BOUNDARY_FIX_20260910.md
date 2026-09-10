# Early player-final callback qualification

Diagnostic evidence: Android 7 ARM64 recording reported error 13, fault context
0x100000001 (FinalWriter hook / Begun phase), coordinator result -6
(WrongOrder), tick 0, one submit and zero qualified physics intervals.

The adapter previously treated every qualified player return during an open
tick as the final physics boundary. A return before the first interval now
returns WaitingForTick instead. The payload already treats that result as
Ignored, so it neither captures nor corrects physics from this early callback.
No tick is advanced, no input packet is consumed again, and no interval is
fabricated. A subsequent real interval and player return still complete the
same tick. End-of-frame without an interval remains an error: this patch does
not assert that zero-interval frames are valid recording packets.

Validation:
- Full ARM64/x86-controller APK build and signature checks passed.
- Production Java regression: 50 checks passed.
- Standalone x86_64 Android regression executed on emulator: two complete ticks,
  six ignored early returns, two missing-interval negative checks passed.
- ARM64 regression binary compiled; not executed on a native ARM64 device.
- Historical g4_g3_adapter_selftest reports 3/4 failures both before and after
  this change; these pre-existing test discrepancies are not claimed as passes.
- Affected-device end-to-end recording is still pending. If a frame-end fault
  follows the ignored early return, additional interval ownership/call-order
  evidence is needed; do not silently drop that tick.

APK: Asphalt9Tas-early-final-fix-20260910.apk
SHA256: 65d776e5e9a71ec1f2f60a9bf5e7a96ad045fa5beda3d5a29928a1f9d58f7905
Payload SHA256: 9748c23bc68e53b8bc05cf97f81c12a3de94ef816a5ed11b78da2c8ff62efff9
No game process was restarted, resumed, or hooked during the standalone tests.
