# Initial physics phase replay candidate

## Evidence and scope

The retained 3106-tick replay has 1306 native integrations in both runs, but
1922 logical ticks have different integration counts. 2259 ticks require vehicle
correction. See `REPLAY_JUDDER_PHASE_MISMATCH_20260910.md` for evidence paths.
This implicates accumulator phase; it does not prove that all visual judder has
one cause. Recording visuals were accepted; replay visuals remain unverified.

## Implementation

- Control ABI 14 appends exact residual/last-interval float bits and presence.
- At first authoritative Physics Submit of tick zero, high-frequency recording
  resolves the native world through the live context/proxy vtable chain and
  records its two float fields. Instruction patterns prove the field offsets.
- A v5 replay restores these bits once at the equivalent pre-submit boundary.
  Later ticks retain native accumulator evolution and dynamic 60/120 Hz steps.
- No added hook, camera override, per-tick phase writes, or forced 60 Hz render cap.
- A9G4R2 v5 uses flags 0x7f and the existing 8 reserved header bytes for phase.
  Header/frame sizes remain 64/144. Versions 2/3/4 retain their old semantics.
- Unknown/unproven phase layouts produce v4, not fabricated phase data. A v5
  replay cannot silently claim exact restoration if its layout cannot resolve.
- Prefix trimming copies the header. Splicing retains the prefix header. Atomic
  replay-to-record keeps the prefix tick count and initial phase, so it does not
  capture or restore a second starting phase at the handoff.
- New-race record Retry starts at tick zero and captures the new world phase.
  A standalone mid-race suffix records its own phase; it must not replace the
  initial phase of the combined archive.

## Offline verification

- Java library regression: 75 checks passed (including v5, zero-integration
  prefix, splice, invalid float and legacy-reserved-byte cases).
- Python archive regression: 5 tests passed, including bit-exact v4/v5 container
  roundtrip with empty integration streams.
- ARM64 payload and native controller syntax checks passed.
- ARM64 payload linked successfully; exact ELF resolver build passed.
- Isolated x86 Android model: 24000 ticks each at 16667/8333/6944 us; restoring
  identical phase eliminated model integration-count and interpolation-alpha
  differences. This is not game replay acceptance.

## Remaining acceptance

All bundled controllers/loaders were built together with ABI 14. The resulting
APK was overwrite-installed to emulator-5554 (ADB Success); the Java suite also
passed against the final APK's compiled classes. No game replay was run.

APK: `D:/AsphaltTAS/outputs/Asphalt9Tas-20260910-initial-phase-test.apk`
SHA256: `45970628b27c9debd85c615100d6bc69a53eaffa67cee3fa565776293fab7a7b`
Payload SHA256: `1b5442c09638a9febe134fd45750febf1810a377838d5a1b37f154b1267444fc`
Payload Build ID: `14a11f95cb89b08044c406849b3adf221fffc4a7`
Build log: `build/initial-phase-apk-build.log`.

Never mix with an already mapped ABI 13 payload. Prepare one new process, record a new short
144 Hz run and verify native export says `format_version=5 initial_phase=1`.
Replay it at normal speed, without manual inputs; compare native integration
counts per tick, natural equality/correction counts, and user-observed visuals.
Old v4 recordings lack the exact initial phase: do not estimate, upgrade, or
promise that their replay judder is corrected by this candidate.

The former diagnostic reader hardcodes ABI 13 payload storage RVAs and is not
valid for the new payload. Re-derive storage/receipt addresses from the new ELF
before taking another memory snapshot.
