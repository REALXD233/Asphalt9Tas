# High-refresh replay judder: measured integration phase mismatch

## Facts from retained source and replay buffers

Read-only snapshot, emulator-5554, PID 18255, payload SHA beginning 766f2f98.
No new hook, native method call, game state write or restart was performed.
The source archive, natural pre-correction frames, and per-tick native receipts
are preserved locally under outputs/replay-judder-18255-20260910 (not public).

- 3106 logical ticks, delta 6944us; 1306 native interval calls in both runs.
- 1922 ticks have different source/replay integration call counts.
- First phase mismatch: tick 1. First physical-state difference: tick 3.
- 847 ticks were naturally equal; 2259 required transform/velocity correction.
- Source zero-integration ticks: 1800; 960 of those required correction.
- Largest measured natural/source position difference approximately 1.258 game units.

First tick sequence (0..11):

| | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Source interval calls |0|1|0|1|0|0|1|0|1|0|1|0|
| Replay interval calls |0|0|1|0|1|0|1|0|0|1|0|1|

The source's first 120 ticks use 1/60 native intervals. A real-number residual
constraint fit places its pre-tick-zero residual in (-9.6693,-8.3307]ms.
If replay uses the same interval outputs during that prefix, its fitted range
is (-15.2266,-13.8880]ms. This second result is conditional: retained replay
scratch stores call counts, not the actual returned intervals. Neither fit
models float rounding, nor determines an exact original residual bit pattern.
Do not write fitted values into the game.

## Interpretation

The current replay reproduces logical inputs but not the native integration
schedule. Physical-state corrections then repeatedly conceal simulation
divergence. Existing static analysis places the correction after the native
interpolation operation, so the display may retain an interpolation based on
the pre-correction state. This is a supported explanation for replay-only
judder, not yet a visual A/B proof or a claim of the sole cause.

Equal total interval counts do not establish deterministic phase. Disabling
corrections alone or changing the camera could mask the symptom while reducing
trajectory accuracy. Arbitrarily applying the full native step on every 144Hz
logical tick would alter game speed and is not an acceptable fix.

## Next implementation boundary

1. Resolve the actual current world through the proven PhysicsContext update
   chain using build-profile/signature proof, not a saved object address.
2. Capture initial residual (+0x188) and last interval (+0x18c) at the first
   authoritative Submit boundary before physics runs. Restore those source
   bits at the corresponding replay boundary; retain dynamic interval policy.
3. Persist exact initial phase in a versioned, optional recording extension.
   Update import/export/cut/splice and archived Retry semantics together.
   Existing v4 files lack this phase; do not fabricate it or claim a lossless
   upgrade. Their behavior must remain explicit.
4. Compare per-tick integration schedule and corrections before judging visuals.
   If phase matches but judder remains, address correction/interpolation order
   separately while preserving the recording path already accepted by the user.

Status: diagnosis and reproducible analysis implemented. No new repair APK has
been installed or published in this turn. Recording visual acceptance remains
passed; replay visual acceptance remains failed.

## Follow-up implementation: native phase binding

Added `src/physics_initial_phase_v1.h` and
`tests/physics_initial_phase_test.cpp`. These are not yet included by the game
payload. The resolver follows current context/world pointers and recognizes
the virtual forwarder plus actual ARM64 residual-add and residual-subtract/
last-interval-store instruction blocks. It does not choose RVAs by channel or
reuse a saved world address. Snapshot carries the exact two float bit patterns.

Verified on current emulator process 18255 with O_RDONLY /proc access:

- context 0x7fff43b4d000 -> world 0x7fff2d0bac00;
- update 0x7fff4b2f741c;
- residual bits 0xbb933ae0, last interval bits 0x3c888889;
- repeated read agreed, residual -0.00449310243, interval 0.0166666675;
- game writes=0, ptrace=0, methods invoked=0.

Mock-memory tests passed resolver, exact restore, invalid float, stale chain
and wrong-code rejection. An isolated native-style float simulation used
24000 ticks at each of 16667/8333/6944us with dynamic 60/120 integration steps.
Different initial residuals produced 4/54/10974 call-schedule mismatches;
matching both initial phase values produced zero mismatches and identical
interpolation factors at every modeled tick. This is NOT a game replay test.
ARM64 compile and Android x86_64 test execution passed.

Still required before an APK: versioned phase metadata in archive and control,
capture/restore at the qualified initial Submit, legacy behavior, and prefix/
continuation metadata propagation. The Restore helper is a low-level bit-write
and readback primitive, not a synchronization or rollback mechanism; payload
integration must provide the actual physics-owner boundary and failure handling.
