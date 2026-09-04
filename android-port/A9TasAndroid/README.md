# A9 TAS Android (G10)

This is the Android product shell for the proven G4-G8 runtime. It does not
replace or reinterpret the replay core.

Current milestone:

- installable Android application skeleton;
- foreground session service owning the prepare transaction;
- one-time `su` process discovery;
- automatic package/process discovery by mapped `libAsphalt9.so`;
- exact SHA-256 BuildProfile selection;
- unknown builds rejected, with no arbitrary PID/address interface;
- exact port of the proven PrepareProcess boundary: atomic private staging,
  one root transaction, remote SHA verification, fresh process, early carrier,
  mapped payload/bootstrap and PID/start-ticks/native-SHA receipt.
- one persistent `su` shell per APK process, shared by scan and preparation, so
  Android's one-time grant remains valid and repeated privilege prompts are not
  used as a workflow.

PrepareProcess intentionally installs no race-session hooks. The next gate
moves the already-proven passive install, lifecycle record/replay, archive and
restore actions into this service.
