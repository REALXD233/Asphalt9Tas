# Approved optimization results

Scope: the eight items approved after the source review. No native game hook,
Tick coordinator, physics correction, or replay-to-record transition was changed.

## Delivered changes

1. Save current segment requests sealing of existing completed Ticks. It no
   longer calls `checkpointAtNextClosedTick` or sends ESC. Explicit save is
   handled at the next status receipt without the 900/2500 ms auto-pause stall
   delay. Natural race completion takes priority over a simultaneous save.
   Use this button while paused. A status/controller transport delay still
   exists; this change does not claim zero-latency saving.
2. APK assets are staged from the runtime manifest, including both supported
   architectures and the ARM64 Profile generator. Historical unreferenced
   carriers remain on the development machine but are absent from the APK.
3. Added a versioned runtime manifest template, early initialization, explicit
   x86 observer build, SOURCE_BUILD.md, reference-input importer, signing runbook
   in the publishing repository, and LF checkout attributes. A clean source
   build passed with SDK/JDK/NDK and 26 declared reference inputs supplied.
   Cloning alone does not supply those external inputs.
4. Packaging hashes the source and archive while copying, keeping final archive
   decoding. Metadata edits reuse the verified summary after atomic replacement.
   Saving now reports export, copy, and validation/storage stages.
5. Selected/verified recording lookup reads the requested archive directly.
   A bounded 64-entry cache accelerates listing; playback bypasses this cache.
   Changed metadata refreshes the entry and deleted paths are not listed.
6. Preference notifications are coalesced over 16 ms; unchanged TextView text is
   not reapplied. Save labels now describe sealing without resuming the game.
7. Service destruction requests cancellation and closes Root on a cleanup
   thread after the worker releases its lock. Shell ownership is checked after
   taking the Root lock, preventing old cleanup from closing a new service's
   shell. Destroyed services stop scheduling further attempts and UI updates.
8. Updated the legacy branch test to continuous adoption; added production Java
   tests for archive transactions, cache freshness, exact source preservation,
   save/finish priority, and Root ownership; added actual asset-staging tests.

## Verification

- Production Java host regression: 25 checks passed.
- Existing branch regression: 5 tests passed.
- Asset-staging test: both normal and no-built-in-Profile variants passed,
  including incorrect hash rejection and preservation of source assets.
- Product static policy and Android Lint passed.
- Clean source build passed with documented external inputs.
- APK size: 12,942,803 -> 2,465,932 bytes (about 81% smaller).
- Packaged x86 carrier count: 34 -> 1.
- All retained runtime executable/library bytes match the previously validated
  APK. ARM64 and x86_64 runtime paths are both retained.
- Signing certificate matches the previous installed APK.
- APK SHA-256: `2457e3252c8c2f2a1050bd252f9f8420846ec75c3ab4f708e1dedc2c5f0cfbfb`.

## Manual acceptance

Pause midway, save the current segment, and confirm there is no automatic
resume. Then retry with the selected prefix, continue at the breakpoint, and
save a second segment. Finally verify complete-lap replay and repeated Retry.
Test rename/target-Tick editing in the overlay with several library entries.
This turn performed no live game test; observed game timing is not inferred
from the host checks. The previously validated GitHub release remains available.
