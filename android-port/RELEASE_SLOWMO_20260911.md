# Slowmo experimental release

## Changes

- Independent recording speeds: 1x, 0.9x, 0.75x, 0.5x, 0.25x, 0.125x.
- Scale the wall-time scheduling budget, not the recorded physics timestep.
  Replay speed remains separate from recording speed.
- Main settings and overlay expose the same choices. The overlay labels the
  value as next-recording speed; this is not a live update of an armed session.
- Fix replay diagnostic CSV export rejecting valid zero-physics-update ticks
  at 120/144 logic tick rates. Normal completion/source checks remain enabled.
- Match native ARM64 and x86/NativeBridge controllers to the rebuilt payload.

## Verification and limitations

Full APK build, Android lint, v2/v3 signing, scheduling/protocol regressions and
15 packaged manifest entries verified. These are offline checks, not full live
acceptance. User reported visually normal slowmo; one 645-tick replay matched
all compared physics frames before the now-fixed diagnostic export failure.
No full Retry/continuation chain has been accepted. Switching back from slowmo
was reported problematic and remains unresolved/deferred. The user withdrew
the separate Retry issue from the current work scope; do not mark it fixed.

Historical live-baseline verification still reports drift in mutable old build
outputs. Candidate builds warn rather than claiming those historical files
validate this release; historical manifests are unchanged.

Install over a same-signature build; prepare a fresh game process afterward to
load the new runtime. Existing practice-only/licensing restrictions remain.

APK SHA256:
`694acac2eeab32e23d381e72b71a195c10ec78a63df798dea838238d25131ebe`
