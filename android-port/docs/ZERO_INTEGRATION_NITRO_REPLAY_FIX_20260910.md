# Missing Nitro on interpolation-only replay ticks

User reports double tap behaves as single tap, single tap has no effect.
Device: emulator-5554; replay game PID 28581; source v5, 3070 ticks,
1296 integration calls, fixed delta 6944 us, initial residual -0.009376917965710163.

## Confirmed evidence

Source contains nine single Nitro calls at ticks:
601, 621, 952, 1008, 1085, 1456, 1478, 1803, 1821.
Only 601 and 1803 have native integration calls. The other seven have zero.
The replay receipt reports `nitro=0,0,2` and `error=0`.

Thus the input was recorded; replay silently omitted the seven events whose
ticks never invoked Physics Interval. This is a missing sparse-tick action path,
not a missing user input or an archive phase decode error.

Evidence: `D:/AsphaltTAS/outputs/zero-tick-nitro-28581-20260910/`.
An additional APK finish/cursor error appears in state-history; it is not the
cause of missing Nitro during the earlier replay and is not claimed fixed here.

## Change

- Extract existing Nitro injection into ReplayNitroAtQualifiedBoundary.
- Keep first Interval injection unchanged on ticks with native integration.
- For zero-integration ticks, inject at the qualified FinalWriter completed-frame
  callback, before closing the logical tick. No new hook or host-thread RPC.
- EndTick verifies the planned count equals injected and recorded counts before
  consuming the tick. Missing playback can no longer appear as error-free.
- Initial physics-phase v5 metadata stays unchanged. Existing v5 source is
  suitable for retesting; no new recording is required.

## Verification

- ARM64 payload syntax and linked build passed.
- Exact payload resolver consistency build passed.
- Isolated Android x86 test: 9000 ticks at 60Hz, 18000 at 120Hz, 21600 at 144Hz;
  records/replays 0/1/2 Nitro calls, including 20001 interpolation-only ticks.
- Missing single/double events are rejected before tick consumption, then pass
  after exact accounting. These are model/bridge tests, not live game acceptance.
- Game-side effectiveness and camera visuals require manual replay validation.

Full APK build and 75 Java regression checks passed. Overwrite install to
emulator-5554 returned Success. Game was not restarted or driven by the agent.
User should prepare one fresh process and replay the same retained recording.

APK: `D:/AsphaltTAS/outputs/Asphalt9Tas-20260910-zero-tick-nitro.apk`
APK SHA256: a341ba3bce8cb7434e040be9733a50ba82afcf856e8a3dbdb1b044ece254b810
Payload SHA256: daf2b6d7e56e63df13be6123253589b07ef00caf7dc39f0bc2e28b6bbc7acd44
Build ID: 7885519e1328ebd535d5d7070ab34d9eb01f518c.
