package dev.a9tas.android;

import android.content.Context;
import android.content.ComponentName;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Process;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Android orchestration of the frozen G4-G8 controller; no new tick logic. */
final class SessionOrchestrator {
    private static final String PROFILE = "/data/local/tmp/a9tas_g8_runtime_build_profile_v1.bin";
    private static final String ACK = "I_ACCEPT_G4_TICK_COORDINATOR_V1";
    private static final int CAPACITY = 24000;

    static boolean shouldSealExistingTicks(boolean complete, int ticks, boolean saveRequested) {
        return !complete && ticks > 0 && saveRequested;
    }

    interface OperationObserver {
        boolean cancellationRequested();
        boolean replayInterruptionRequested();
        boolean stopRecordingWhenProgressStalls();
        default boolean checkpointRequested() { return false; }
        default void onSaveStage(String message) {}
        long progressStallTimeoutMillis();
        long observationPollMillis();
        void onProgress(int ticks, int limit);
        File openCancellationSignal() throws IOException;
        void closeCancellationSignal(File signal);
        void onHandoffArmed(int remainingSeconds);
        void onHandoffResumed();
    }

    static final class OperationCancelledException extends IOException {
        OperationCancelledException() { super("operation cancelled by user"); }
    }

    static final class ReplayInterruptedException extends IOException {
        final int ticks;
        final String recordingSha256;
        final boolean automaticPause;

        ReplayInterruptedException(int ticks, String recordingSha256) {
            this(ticks, recordingSha256, false);
        }

        ReplayInterruptedException(int ticks, String recordingSha256,
                                   boolean automaticPause) {
            super("prefix replay interrupted at closed tick " + ticks);
            this.ticks = ticks;
            this.recordingSha256 = recordingSha256 == null ? "" : recordingSha256;
            this.automaticPause = automaticPause;
        }
    }

    static final class RecordingPausedException extends IOException {
        final int ticks;
        RecordingPausedException(int ticks) {
            super("recording interrupted after gameplay paused at tick " + ticks);
            this.ticks = ticks;
        }
    }

    static final class InstallReceipt {
        final int pid;
        final long owner;
        final int samples;

        InstallReceipt(int pid, long owner, int samples) {
            this.pid = pid;
            this.owner = owner;
            this.samples = samples;
        }
    }

    static final class RecordReceipt {
        final File file;
        final File archive;
        final int ticks;
        final long intervalCount;
        final String sha256;
        final String archiveSha256;
        final String archiveError;
        final boolean checkpoint;
        final boolean retryDiscarded;

        RecordReceipt(File file, File archive, int ticks, long intervalCount, String sha256,
                      String archiveSha256, String archiveError,
                      boolean checkpoint, boolean retryDiscarded) {
            this.file = file;
            this.archive = archive;
            this.ticks = ticks;
            this.intervalCount = intervalCount;
            this.sha256 = sha256;
            this.archiveSha256 = archiveSha256;
            this.archiveError = archiveError;
            this.checkpoint = checkpoint;
            this.retryDiscarded = retryDiscarded;
        }
    }

    static final class ReplayReceipt {
        final int ticks;
        final long intervalCount;
        final long nitroCalls;
        final long barrelFrames;
        final long fixedDeltaUs;
        final String recordingSha256;
        final boolean pausedAtTarget;
        final boolean hardStoppedAtTarget;
        final boolean continuationPrearmed;

        ReplayReceipt(int ticks, long intervalCount, long nitroCalls,
                      long barrelFrames, long fixedDeltaUs, String recordingSha256,
                      boolean pausedAtTarget, boolean hardStoppedAtTarget,
                      boolean continuationPrearmed) {
            this.ticks = ticks;
            this.intervalCount = intervalCount;
            this.nitroCalls = nitroCalls;
            this.barrelFrames = barrelFrames;
            this.fixedDeltaUs = fixedDeltaUs;
            this.recordingSha256 = recordingSha256;
            this.pausedAtTarget = pausedAtTarget;
            this.hardStoppedAtTarget = hardStoppedAtTarget;
            this.continuationPrearmed = continuationPrearmed;
        }
    }

    private static final class Identity {
        final int pid;
        final long startTicks;
        final long base;
        final String packageName;
        final String processName;
        final String nativeSha;
        final BuildProfileRegistry.Profile profile;
        final ArtifactRegistry.Backend backend;
        int recordDeltaUs = 16667;

        Identity(int pid, long startTicks, long base, String packageName,
                 String processName, String nativeSha,
                 BuildProfileRegistry.Profile profile,
                 ArtifactRegistry.Backend backend) {
            this.pid = pid;
            this.startTicks = startTicks;
            this.base = base;
            this.packageName = packageName;
            this.processName = processName;
            this.nativeSha = nativeSha;
            this.profile = profile;
            this.backend = backend;
        }
    }

    private SessionOrchestrator() {}

    static InstallReceipt installPassive(Context context) throws Exception {
        Identity identity = readPreparedIdentity(context);
        verifyFixedArtifacts(context);
        requirePracticeMode(context, identity);
        String prefix = "/data/local/tmp/a9tas-g10-session-" + identity.pid;
        boolean installAttempted = false;
        try {
            String observation = prefix + ".a9pio2";
            String observeScript = "rm -f " + observation + " && " +
                    devicePath(identity.backend.observerDeviceName) + " " +
                    identity.pid + " " + Long.toHexString(identity.base) + " 300 10 " +
                    observation + " 0 " + PROFILE + " && " +
                    "test -s " + observation + " && " +
                    "echo G10_OBSERVATION_HEX_BEGIN && od -An -tx1 -v " + observation +
                    " && echo G10_OBSERVATION_HEX_END";
            RootShell.Result observed = RootShell.runFixedScript(observeScript, 20L);
            require(observed.ok(), "StepOptions observer failed: " + tail(text(observed.output)));
            String hex = between(observed.output, "G10_OBSERVATION_HEX_BEGIN",
                    "G10_OBSERVATION_HEX_END");
        byte[] profileBytes = BuildProfileRegistry.readAll(identity.profile.open(context));
            PhysicsIntervalReceipt.Result parsed = PhysicsIntervalReceipt.parse(
                    PhysicsIntervalReceipt.decodeHex(hex), profileBytes,
                    identity.pid, identity.base);

            // Read-only discovery must finish before publishing any hook.
            // A missing race object must not require rollback or kill the game.
            // The controller may have published a passive hook before the host
            // observes its return or timeout. From this point every failure is
            // therefore a restore-or-terminate path.
            installAttempted = true;
            RootShell.Result installResult = runControllerFileReceipt(
                    identity, "install", 1L, prefix + ".install.txt", null,
                    CAPACITY, 35L);
            String installText = text(installResult.output);
            boolean installAccepted = installResult.ok() &&
                    installText.contains("G4_ACTION action=6 ") &&
                            hasToken(installText, "status=4") &&
                            hasToken(installText, "ticks=0") &&
                            hasToken(installText, "detached=1");
            // These controller stages occur before the payload command can
            // publish any game Hook.  When the controller also proves a clean
            // detach and zero uncertainty, a failed install must not be turned
            // into an unnecessary game termination.  Any later, ambiguous or
            // timeout failure still follows restore-or-terminate.
            if (!installAccepted && provenNoMutationInstallFailure(installText))
                installAttempted = false;
            require(installAccepted,
                    "passive install receipt rejected: " + tail(installText));

            RootShell.Result passiveResult = runControllerFileReceipt(
                    identity, "passive", 1L, prefix + ".passive.txt", null,
                    CAPACITY, 20L);
            String passiveText = text(passiveResult.output);
            require(passiveResult.ok() && passiveText.contains("G4_PASSIVE installed=1 ") &&
                            hasToken(passiveText, "error=0"),
                    "passive identity receipt rejected: " + tail(passiveText));

            SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
            boolean published = preferences.edit().putLong("session_base", identity.base)
                    .putLong("session_owner", parsed.owner)
                    .putInt("session_capacity", CAPACITY)
                    .putBoolean("session_hooks_installed", true).commit();
            require(published, "installed session ownership could not be persisted");
            return new InstallReceipt(identity.pid, parsed.owner, parsed.sampleCount);
        } catch (Exception error) {
            if (installAttempted) rollbackOrTerminate(context, identity, prefix, 1L);
            throw error;
        }
    }

    static String restoreIfNeeded(Context context) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        if (!preferences.getBoolean("session_hooks_installed", false)) return "No installed hooks";
        // A vanished or PID-reused game process cannot still own our hooks.
        // Forget that stale runtime locally instead of trapping the user in an
        // impossible restore loop. Recording-library state is preserved.
        if (reconcileTerminatedRuntimeState(context))
            return "Previous game process ended · stale session cleared";
        Identity identity = readStoredIdentity(context, true);
        long owner = preferences.getLong("session_owner", 0L);
        require(owner != 0, "stored owner is missing");
        String prefix = "/data/local/tmp/a9tas-g10-session-" + identity.pid;
        if (!restoreController(identity, owner, prefix + ".restore.txt",
                preferences.getInt("session_capacity", CAPACITY))) {
            terminate(identity.packageName);
            clearTerminatedRuntimeState(context);
            throw new IOException("restore uncertain; game process terminated");
        }
        boolean cleared = preferences.edit().putBoolean("session_hooks_installed", false)
                .putBoolean("brush_archived_record_ready", false)
                .putBoolean("branch_replay_ready", false)
                .putBoolean("branch_runtime_prearmed", false)
                .putBoolean("resident_retry_record_armed", false)
                .putBoolean("resident_retry_auto_loop", false)
                // A pending action belongs to this exact payload/PID.  Once the
                // hooks are restored there is nothing left for a later replay
                // to cancel.  Keeping these host flags used to poison the next
                // prepared process with a fictitious resident queue.
                .putBoolean("resident_pending_queued", false)
                .remove("resident_pending_frame_limit")
                .putBoolean("record_waiting_retry", false)
                .putBoolean("branch_pending", false)
                .remove("branch_pending_archive")
                .remove("branch_pending_archive_sha")
                .remove("branch_pending_target_tick")
                .remove("branch_pending_source_sha")
                .remove("branch_pending_pid")
                .remove("branch_pending_start_ticks")
                .remove("branch_pending_continuous")
                .putBoolean("game_process_hard_paused", false)
                .remove("hard_paused_pid").remove("hard_paused_start_ticks")
                .remove("session_owner").remove("session_base").commit();
        require(cleared, "restored session state could not be persisted");
        return "All session hooks restored";
    }

    static RecordReceipt recordLifecycle(Context context, OperationObserver observer) throws Exception {
        return recordLifecycle(context, observer, false);
    }

    static RecordReceipt recordLifecycle(Context context, OperationObserver observer,
                                         boolean keepBrushSession) throws Exception {
        return recordLifecycle(context, observer, keepBrushSession, false, 0, false, true);
    }

    static RecordReceipt recordLifecycle(Context context, OperationObserver observer,
                                         boolean keepBrushSession,
                                         boolean resumePausedRace) throws Exception {
        return recordLifecycle(context, observer, keepBrushSession,
                resumePausedRace, false);
    }

    static RecordReceipt recordLifecycle(Context context, OperationObserver observer,
                                         boolean keepBrushSession,
                                         boolean resumePausedRace,
                                         boolean gameAlreadyForeground) throws Exception {
        return recordLifecycle(context, observer, keepBrushSession, false, 0,
                gameAlreadyForeground, resumePausedRace);
    }

    static RecordReceipt recordFromPausedReplay(Context context,
                                                OperationObserver observer,
                                                int armedPauseSeconds,
                                                boolean gameAlreadyForeground,
                                                boolean automaticResume)
            throws Exception {
        return recordFromPausedReplay(context, observer, armedPauseSeconds,
                gameAlreadyForeground, automaticResume, false);
    }

    static RecordReceipt recordFromPausedReplay(Context context,
                                                OperationObserver observer,
                                                int armedPauseSeconds,
                                                boolean gameAlreadyForeground,
                                                boolean automaticResume,
                                                boolean keepBrushSession)
            throws Exception {
        return recordLifecycle(context, observer, keepBrushSession, true,
                armedPauseSeconds, gameAlreadyForeground, automaticResume);
    }

    private static RecordReceipt recordLifecycle(Context context,
                                         OperationObserver observer,
                                         boolean keepBrushSession,
                                          boolean pausedReplayHandoff,
                                          int armedPauseSeconds,
                                          boolean gameAlreadyForeground,
                                          boolean resumePausedRace) throws Exception {
        require(!pausedReplayHandoff ||
                        armedPauseSeconds >= 0 && armedPauseSeconds <= 600,
                "armed branch pause must be between 0 and 600 seconds");
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        require(preferences.getBoolean("session_hooks_installed", false),
                "install the race session before recording");
        Identity identity = readStoredIdentity(context, true);
        verifyFixedArtifacts(context);
        requirePracticeMode(context, identity);
        long owner = preferences.getLong("session_owner", 0L);
        require(owner != 0, "stored race owner is missing");
        boolean archivedRearm = !pausedReplayHandoff &&
                preferences.getBoolean("brush_archived_record_ready", false);
        boolean continuationAlreadyArmed = pausedReplayHandoff &&
                preferences.getBoolean("branch_runtime_prearmed", false);
        boolean residentRetryAlreadyArmed = !pausedReplayHandoff &&
                preferences.getBoolean("resident_retry_record_armed", false);
        String prefix = "/data/local/tmp/a9tas-g10-record-" + identity.pid + "-" +
                System.currentTimeMillis();
        boolean armAttempted = false;
        boolean armConfirmed = continuationAlreadyArmed || residentRetryAlreadyArmed;
        boolean armRejectedCleanly = false;
        boolean restored = false;
        int branchPrefixTicks = 0;
        if (pausedReplayHandoff) {
            long targetTick = preferences.getLong("branch_pending_target_tick", -1L);
            require(targetTick >= 0 && targetTick < CAPACITY - 1L,
                    "paused branch target is invalid");
            branchPrefixTicks = Math.toIntExact(targetTick + 1L);
        }
        try {
            if (!continuationAlreadyArmed && !residentRetryAlreadyArmed) {
                String armAction = pausedReplayHandoff ? "rearm-branch" :
                        archivedRearm ? "rearm-record" : "record-life";
                int expectedAction = pausedReplayHandoff ? 19 : archivedRearm ? 17 : 11;
                armAttempted = true;
                RootShell.Result armed = runControllerFileReceipt(
                        identity, armAction, owner, prefix + ".arm.txt", null,
                        CAPACITY, 35L);
                String armText = text(armed.output);
                armRejectedCleanly = armText.contains(
                                "G4_TICK_COORDINATOR_CONTROLLER passed=0") &&
                        hasToken(armText, "uncertain=0") &&
                        hasToken(armText, "detached=1") &&
                        hasToken(armText, "process_killed=0");
                String armFailure = pausedReplayHandoff
                        ? "续录断点运行时已失效；请重新回放前缀到目标 Tick 后再续录：" +
                        tail(armText)
                        : armText.contains("G4_OBJECT_DIAG main_candidates=0")
                        ? "未检测到当前比赛对象；请进入练习模式比赛，在比赛画面暂停后重试。" +
                        "本次未开始录制，常驻会话仍保留"
                        : armText.contains("G4_OBJECT_DIAG lifecycle=0") &&
                        armText.contains("countdown=0")
                        ? "未检测到倒计时 3 锚点；请在 Retry 新局显示 3 时暂停，然后重试"
                        : "lifecycle record arm rejected: " + objectFailureDetails(armText);
                require(armed.ok() && armText.contains(
                                "G4_ACTION action=" + expectedAction + " ") &&
                                hasToken(armText, "status=1") &&
                                hasToken(armText, "ticks=" +
                                        (pausedReplayHandoff ? branchPrefixTicks : 0)) &&
                                hasToken(armText, "detached=1"),
                        armFailure);
                armConfirmed = true;
            } else if (continuationAlreadyArmed) {
                require(preferences.edit().putBoolean(
                                "branch_runtime_prearmed", false).commit(),
                        "unable to consume exact branch handoff receipt");
            } else {
                require(preferences.edit().putBoolean(
                                "resident_retry_record_armed", false).commit(),
                        "unable to consume resident Retry arm receipt");
            }
            if (archivedRearm)
                preferences.edit().putBoolean(
                        "brush_archived_record_ready", false).apply();

            // Keep the next record generation pre-armed while this attempt is
            // live.  If the user presses Retry without saving, the game-side
            // lifecycle hook closes this attempt and starts the next one at
            // Tick 0 before Java could possibly observe the UI transition.
            if (keepBrushSession && !pausedReplayHandoff &&
                    !preferences.getBoolean("continuous_load_selected", false) &&
                    !preferences.getBoolean("resident_retry_auto_loop", false)) {
                queueDirectRetryRecord(identity, owner, prefix, CAPACITY);
                preferences.edit().putBoolean(
                        "resident_retry_auto_loop", true)
                        .putBoolean("resident_pending_queued", true)
                        .putInt("resident_pending_frame_limit", CAPACITY).apply();
            }

            if (pausedReplayHandoff) {
                if (resumePausedRace) waitAfterHandoffArm(observer, armedPauseSeconds);
                else if (observer != null) observer.onHandoffArmed(-1);
            }

            // A branch started from the main Activity must return the already
            // paused game to the foreground, but manual handoff deliberately
            // must not synthesize ESC.  The first real tick after the user
            // resumes is therefore the first suffix tick.
            if (pausedReplayHandoff && !gameAlreadyForeground) {
                String launcher = resolveLauncher(context, identity.packageName);
                RootShell.Result foregrounded = RootShell.runFixedScript(
                        "am start -n " + launcher + " >/dev/null; sleep 1; " +
                                "echo G10_RECORD_GAME_FOREGROUND foreground=1", 15L);
                require(foregrounded.ok() && text(foregrounded.output).contains(
                                "G10_RECORD_GAME_FOREGROUND foreground=1"),
                        "unable to foreground paused race");
            }

            if (resumePausedRace) {
                String launcher = resolveLauncher(context, identity.packageName);
                RootShell.Result resumed = RootShell.runFixedScript(
                        (gameAlreadyForeground ? "" :
                                "am start -n " + launcher + " >/dev/null; sleep 1; ") +
                        "input keyevent 111; echo G10_RECORD_GAME_FOREGROUND esc=1", 15L);
                require(resumed.ok() && text(resumed.output).contains(
                                "G10_RECORD_GAME_FOREGROUND esc=1"),
                        "unable to resume paused race");
            }
            if (pausedReplayHandoff && resumePausedRace && observer != null)
                observer.onHandoffResumed();

            String status;
            boolean checkpoint = false;
            try {
                status = waitForCompletion(identity, owner, prefix, CAPACITY,
                        0, observer, pausedReplayHandoff && !resumePausedRace);
                // The exact checkpoint transaction completes the record from
                // another service thread while this method is observing it.
                // Derive that outcome from the authoritative terminal reason.
                checkpoint = hasToken(status, "completion=1,3");
            } catch (RecordingPausedException paused) {
                RootShell.Result sealed = runControllerFileReceipt(
                        identity, "seal-record", owner, prefix + ".seal.txt",
                        null, CAPACITY, 35L);
                String sealedText = text(sealed.output);
                boolean checkpointSealed = sealed.ok() &&
                        sealedText.contains("G4_ACTION action=18 ") &&
                        hasToken(sealedText, "status=2") &&
                        hasToken(sealedText, "ticks=" + paused.ticks);
                boolean retryWonPauseRace = sealedText.contains("stage=seal_precondition") &&
                        hasToken(sealedText, "uncertain=0") &&
                        hasToken(sealedText, "detached=1") &&
                        hasToken(sealedText, "process_killed=0");
                require(checkpointSealed || retryWonPauseRace,
                        "paused recording checkpoint rejected: " + tail(sealedText));
                RootShell.Result checked = runControllerFileReceipt(
                        identity, "status", owner, prefix + ".sealed-status.txt",
                        null, CAPACITY, 35L);
                status = text(checked.output);
                require(checked.ok(), "paused checkpoint status transport failed");
                boolean sealedCheckpoint = hasToken(status, "completion=1,3");
                boolean sealedRaceEnd = hasToken(status, "completion=1,2");
                require(sealedCheckpoint || sealedRaceEnd,
                        "pause/Retry resolution has no terminal reason: " + tail(status));
                // If Retry replaced the paused race before seal-record acquired
                // its stop, completion=1,2 is authoritative: discard the old
                // attempt and let the normal Retry path rearm the next race.
                checkpoint = checkpointSealed && sealedCheckpoint;
            }
            throwIfCancelled(observer);
            Matcher count = Pattern.compile("(?:^|\\s)complete=1\\s+ticks=([0-9]+)(?:\\s|$)")
                    .matcher(status);
            require(count.find(), "lifecycle receipt has no actual tick count");
            int ticks = Integer.parseInt(count.group(1));
            require(ticks > 0 && ticks < CAPACITY, "race did not complete before capacity");
            for (String token : new String[]{"complete=1", "ticks=" + ticks,
                    "begin=" + ticks, "final=" + ticks,
                    "end=" + ticks, "error=0",
                    checkpoint ? "completion=1,3" : "completion=1,2"})
                require(hasToken(status, token), "lifecycle receipt missing " + token);
            validateIntegrationReceipt(status, ticks, identity.recordDeltaUs);

            Matcher lifecycleMatch = Pattern.compile(
                    "(?:^|\\s)lifecycle=([0-9]+)(?:\\s|$)").matcher(status);
            require(lifecycleMatch.find(), "lifecycle receipt has no terminal state");
            int terminalLifecycle = Integer.parseInt(lifecycleMatch.group(1));
            Matcher completionLifecycleMatch = Pattern.compile(
                    "(?:^|\\s)terminal_lifecycle=([0-9]+)(?:\\s|$)").matcher(status);
            require(completionLifecycleMatch.find(),
                    "lifecycle receipt has no immutable completion state");
            int completionLifecycle = Integer.parseInt(
                    completionLifecycleMatch.group(1));
            // Match upstream's OnRaceEnded finalization point: decide from the
            // lifecycle captured at completion, never from a later live sample
            // that a quick Retry can overwrite after a valid finish.  On this
            // exact A9 core, a natural finish leaves racing through state 9.
            // Direct Retry was observed to leave through state 22 before the
            // next lifecycle object reaches countdown state 2; accepting only
            // state 2 made those attempts look like completed laps and caused
            // Java to enqueue a second generation over the resident auto-loop.
            boolean retryDiscarded = !checkpoint &&
                    (completionLifecycle == 2 || completionLifecycle == 22);
            if (retryDiscarded) {
                restored = true;
                boolean retryPublished = preferences.edit()
                        .putBoolean("session_hooks_installed", true)
                        .putBoolean("brush_archived_record_ready", true)
                        .remove("attempt_draft_path").remove("attempt_draft_sha")
                        .remove("attempt_draft_ticks").commit();
                require(retryPublished,
                        "Retry session ownership could not be persisted");
                return new RecordReceipt(null, null, ticks, 0L, "", "", "",
                        false, true);
            }

            String remoteRecording = prefix + ".a9g4r2";
            if (observer != null) observer.onSaveStage("正在导出已封存的 " + ticks + " Tick");
            RootShell.Result dumped = dumpCompletedRecordingAfterHelpersSettle(
                    identity, owner, remoteRecording, CAPACITY,
                    prefix + ".dump.stdout.txt");
            String dumpText = text(dumped.output);
            require(dumped.ok() && dumpText.contains(
                    "G4_RECORD_DUMP passed=1 frames=" + ticks + " "),
                    "recording dump rejected: " + tail(dumpText));
            throwIfCancelled(observer);

            if (observer != null) observer.onSaveStage("正在复制录像到应用存储");
            File local = checkpoint
                    ? copyRecordingIntoApp(context, remoteRecording, ticks,
                            "drafts", "attempt-")
                    : copyRecordingIntoApp(context, remoteRecording, ticks);
            if (observer != null) observer.onSaveStage("正在检查录像并写入存档");
            A9TasArchive.SourceSummary source = A9TasArchive.inspectSource(local);
            require(source.frameCount == ticks, "saved recording tick count changed");
            String hash = sha256(local);
            A9TasLibrary.Metadata metadata = A9TasLibrary.metadataFromSession(context);

            // Match AluTasV2's resident manager: successful operations switch
            // mode but do not uninstall the game hooks.  The completed runtime
            // remains disabled and can be rearmed for the next race.  Explicit
            // Restore, process replacement and genuine failures are the only
            // paths that remove the session hooks.
            final boolean retainRuntime = true;
            restored = true;
            if (checkpoint) {
                SharedPreferences.Editor checkpointState = preferences.edit()
                        .putBoolean("session_hooks_installed", true)
                        .putBoolean("brush_archived_record_ready", true)
                        .remove("latest_recording")
                        .remove("latest_recording_sha")
                        .remove("latest_recording_ticks")
                        .putString("attempt_draft_path", local.getAbsolutePath())
                        .putString("attempt_draft_sha", hash)
                        .putInt("attempt_draft_ticks", ticks);
                A9TasLibrary.persistRawMetadataSnapshot(checkpointState, metadata);
                require(checkpointState.commit(),
                        "recording checkpoint state could not be persisted");
                restored = true;
                return new RecordReceipt(local, null, ticks, source.intervalCount,
                        hash, "", "", true, false);
            }
            SharedPreferences.Editor savedRecording = preferences.edit()
                    .putBoolean("session_hooks_installed", retainRuntime)
                    .putBoolean("brush_archived_record_ready", retainRuntime)
                    .putString("latest_recording", local.getAbsolutePath())
                    .putString("latest_recording_sha", hash)
                    .putInt("latest_recording_ticks", ticks);
            A9TasLibrary.persistRawMetadataSnapshot(savedRecording, metadata);
            require(savedRecording.commit(),
                    "recording/session ownership state could not be persisted");
            File archive = null;
            String archiveSha = "";
            String archiveError = "";
            try {
                A9TasLibrary.Entry packaged = A9TasLibrary.pack(context, local, hash, metadata);
                archive = packaged.file;
                archiveSha = packaged.archiveSha256;
                preferences.edit().putString("latest_archive", archive.getAbsolutePath())
                        .putString("latest_archive_sha", archiveSha)
                        .putString("latest_archive_id",
                                packaged.summary.manifest.getString("recording_id")).apply();
            } catch (Exception packagingFailure) {
                archiveError = packagingFailure.getMessage() == null ?
                        packagingFailure.getClass().getSimpleName() : packagingFailure.getMessage();
                preferences.edit().putString("latest_archive_error", archiveError).apply();
            }
            return new RecordReceipt(local, archive, ticks, source.intervalCount, hash,
                    archiveSha, archiveError, false, false);
        } catch (Exception error) {
            if (!restored && !(armAttempted && !armConfirmed && armRejectedCleanly))
                rollbackOrTerminate(context, identity, prefix, owner);
            throw error;
        }
    }

    /**
     * Queues the next record generation inside the already resident payload.
     * This is the upstream-style path: Retry is observed by the game-side
     * lifecycle callback, so Java neither scans the heap nor guesses when the
     * countdown reached 3.
     */
    static void queueNextRaceRecord(Context context, OperationObserver observer)
            throws Exception {
        SharedPreferences preferences = context.getSharedPreferences(
                "session", Context.MODE_PRIVATE);
        require(preferences.getBoolean("session_hooks_installed", false) &&
                        preferences.getBoolean("brush_archived_record_ready", false),
                "completed brush session is not ready for resident rearm");
        Identity identity = readStoredIdentity(context, true);
        long owner = preferences.getLong("session_owner", 0L);
        require(owner != 0, "stored race owner is missing");
        String prefix = "/data/local/tmp/a9tas-g10-resident-rearm-" +
                identity.pid + "-" + System.currentTimeMillis();
        RootShell.Result queued = runControllerFileReceipt(
                identity, "queue-record", owner, prefix + ".queue.txt",
                null, CAPACITY, 35L);
        String queuedText = text(queued.output);
        require(queued.ok() && queuedText.contains("G4_ACTION action=24 ") &&
                        hasToken(queuedText, "status=5") &&
                        hasToken(queuedText, "detached=1"),
                "resident next-race queue rejected: " + tail(queuedText));
        preferences.edit().putBoolean("resident_retry_auto_loop", false)
                .putBoolean("resident_pending_queued", true)
                .putInt("resident_pending_frame_limit", CAPACITY).apply();

        File cancellationSignal = observer == null ? null :
                observer.openCancellationSignal();
        try {
            String cancellationPath = cancellationSignal == null ? null :
                    cancellationSignal.getCanonicalPath();
            if (cancellationPath != null)
                require(safePath(cancellationPath), "unsafe cancellation signal path");
            RootShell.Result waited = runControllerFileReceipt(
                    identity, "wait-activation", owner,
                    prefix + ".activation.txt", null, CAPACITY, 330L,
                    1, cancellationPath);
            String waitText = text(waited.output);
            if (hasToken(waitText, "cancelled=1")) {
                boolean archived = cancelResidentPending(
                        identity, owner, prefix + ".cancel-pending.txt", CAPACITY);
                preferences.edit()
                        .putBoolean("resident_pending_queued", false)
                        .remove("resident_pending_frame_limit")
                        .putBoolean("brush_archived_record_ready", archived)
                        .apply();
                throw new OperationCancelledException();
            }
            require(waited.ok() && waitText.contains("G4_PENDING_WAIT passed=1") &&
                            waitText.contains(
                                    "G10_CONTROLLER_FILE_RECEIPT action=wait-activation") &&
                            hasToken(waitText, "activated=1"),
                    "resident Retry activation failed: " + tail(waitText));
            require(preferences.edit()
                            .putBoolean("resident_retry_record_armed", true)
                            .putBoolean("resident_pending_queued", false)
                            .remove("resident_pending_frame_limit")
                            .putBoolean("brush_archived_record_ready", false)
                            .commit(),
                    "resident Retry activation receipt could not be persisted");
        } finally {
            if (observer != null && cancellationSignal != null)
                observer.closeCancellationSignal(cancellationSignal);
        }
    }

    /**
     * Stages and queues the selected immutable prefix while the previous race
     * receipt is still resident.  The payload-owned lifecycle callback starts
     * that replay at the next real 2 -> 3 race transition, so Retry does not
     * depend on a Java poll, a countdown scan or a synthetic resume key.
     *
     * On return the prefix has reached its exact completion barrier, the game
     * has been paused once, and the same resident runtime is already armed for
     * suffix recording.  The caller only has to publish branch metadata and
     * wait for the user's manual resume.
     */
    static ReplayReceipt queueNextRacePrefix(Context context,
                                              OperationObserver observer)
            throws Exception {
        SharedPreferences preferences = context.getSharedPreferences(
                "session", Context.MODE_PRIVATE);
        require(preferences.getBoolean("session_hooks_installed", false) &&
                        preferences.getBoolean("brush_archived_record_ready", false),
                "completed brush session is not ready for resident prefix replay");
        Identity identity = readStoredIdentity(context, true);
        verifyFixedArtifacts(context);
        requirePracticeMode(context, identity);
        long owner = preferences.getLong("session_owner", 0L);
        require(owner != 0, "stored race owner is missing");

        A9TasLibrary.Entry selected = A9TasLibrary.selected(context);
        A9TasArchive.Summary archive = selected.summary;
        org.json.JSONObject game = archive.manifest.getJSONObject("game");
        boolean experimental = preferences.getBoolean(
                "prepared_experimental_bypass", false);
        boolean unknownBuildFallback = experimental &&
                BuildProfileRegistry.load(context).find(identity.nativeSha) == null;
        require((unknownBuildFallback ||
                        identity.nativeSha.equals(game.getString("native_sha256"))) &&
                        identity.profile.buildId.equals(game.getString("build_id")),
                "selected recording belongs to a different game build; channel package names may differ");
        long targetTick = archive.targetTick;
        String targetBinding = preferences.getString(
                "replay_target_archive_sha", "");
        if (selected.archiveSha256.equals(targetBinding))
            targetTick = preferences.getLong("replay_target_tick", archive.targetTick);
        require(targetTick >= 0 && targetTick < archive.frameCount,
                "selected replay target tick is invalid");
        require(archive.frameCount > 0 && archive.frameCount <= CAPACITY,
                "selected recording exceeds runtime capacity");

        File replaySource = A9TasLibrary.materializeReplaySource(
                context, selected, targetTick);
        A9TasArchive.SourceSummary replaySummary =
                A9TasArchive.inspectSource(replaySource);
        int ticks = Math.toIntExact(targetTick + 1L);
        require(replaySummary.frameCount == ticks,
                "materialized target replay length changed");
        require(replaySummary.fixedDeltaUs == archive.fixedDeltaUs,
                "materialized replay fixed-delta changed");
        int replaySpeed = preferences.getInt("replay_speed_factor", 1);
        require(replaySpeed == 1 || replaySpeed == 2 || replaySpeed == 4 ||
                        replaySpeed == 8,
                "selected replay speed is invalid");

        String sourceSha = A9TasLibrary.sha256(replaySource);
        String remoteSource = "/data/local/tmp/a9tas-g10-replay-" +
                sourceSha + ".a9g4r2";
        String prefix = "/data/local/tmp/a9tas-g10-resident-prefix-" +
                identity.pid + "-" + System.currentTimeMillis();
        String localPath = replaySource.getCanonicalPath();
        require(safePath(localPath) && safePath(remoteSource),
                "unsafe replay staging path");
        ArtifactRegistry.IdentityHelper replayHelper = ArtifactRegistry
                .loadAndVerify(context).identityHelperFor(identity.backend.hostMachine);
        require(replayHelper != null, "replay identity helper unavailable");
        String replayProbe = devicePath(replayHelper.deviceName);
        RootShell.Result staged = RootShell.runFixedScript(
                "set -eu; rm -f " + remoteSource + "; cp " + localPath + " " +
                        remoteSource + "; chmod 0600 " + remoteSource + "; " +
                        replayProbe + " --expect " + sourceSha + " " + remoteSource +
                        "; echo G10_REPLAY_SOURCE_STAGED bytes=$(wc -c < " +
                        remoteSource + ") sha256=" + sourceSha, 45L);
        String stageText = text(staged.output);
        require(staged.ok() && stageText.contains("G10_REPLAY_SOURCE_STAGED") &&
                        stageText.contains("sha256=" + sourceSha),
                "resident prefix staging failed: " + tail(stageText));

        RootShell.Result queued = runControllerFileReceipt(
                identity, "queue-replay", owner, prefix + ".queue.txt",
                remoteSource, ticks, 45L, replaySpeed, 2);
        String queuedText = text(queued.output);
        require(queued.ok() && queuedText.contains("G4_ACTION action=25 ") &&
                        hasToken(queuedText, "status=5") &&
                        hasToken(queuedText, "detached=1"),
                "resident prefix queue rejected: " + tail(queuedText));
        require(preferences.edit()
                        .putBoolean("brush_archived_record_ready", false)
                        .putBoolean("resident_retry_record_armed", false)
                        .putBoolean("resident_retry_auto_loop", false)
                        .putBoolean("resident_pending_queued", true)
                        .putInt("resident_pending_frame_limit", ticks)
                        .commit(),
                "resident prefix queue state could not be persisted");

        File cancellationSignal = observer == null ? null :
                observer.openCancellationSignal();
        try {
            String cancellationPath = cancellationSignal == null ? null :
                    cancellationSignal.getCanonicalPath();
            if (cancellationPath != null)
                require(safePath(cancellationPath),
                        "unsafe cancellation signal path");
            RootShell.Result activated = runControllerFileReceipt(
                    identity, "wait-activation", owner,
                    prefix + ".activation.txt", null, ticks, 330L,
                    1, cancellationPath);
            String activationText = text(activated.output);
            if (hasToken(activationText, "cancelled=1")) {
                boolean archived = cancelResidentPending(
                        identity, owner, prefix + ".cancel-pending.txt", ticks);
                preferences.edit()
                        .putBoolean("resident_pending_queued", false)
                        .remove("resident_pending_frame_limit")
                        .putBoolean("brush_archived_record_ready", archived)
                        .apply();
                throw new OperationCancelledException();
            }
            require(activated.ok() && activationText.contains(
                            "G4_PENDING_WAIT passed=1") &&
                            hasToken(activationText, "activated=1"),
                    "resident prefix activation failed: " + tail(activationText));
        } finally {
            if (observer != null && cancellationSignal != null)
                observer.closeCancellationSignal(cancellationSignal);
        }

        waitForCompletionPauseAndRearm(identity, owner, prefix, ticks, observer);
        boolean published = preferences.edit()
                .putBoolean("session_hooks_installed", true)
                .putBoolean("resident_pending_queued", false)
                .remove("resident_pending_frame_limit")
                .putBoolean("branch_replay_ready", true)
                .putBoolean("branch_runtime_prearmed", true)
                .putString("last_replay_archive", selected.file.getAbsolutePath())
                .putString("last_replay_archive_sha", selected.archiveSha256)
                .putString("last_replay_recording_sha", sourceSha)
                .putLong("last_replay_target_tick", targetTick)
                .putLong("last_replay_fixed_delta_us", replaySummary.fixedDeltaUs)
                .putInt("last_replay_ticks", ticks)
                .commit();
        require(published, "resident prefix handoff state could not be persisted");
        return new ReplayReceipt(ticks, replaySummary.intervalCount,
                replaySummary.nitroCalls, replaySummary.barrelFrames,
                replaySummary.fixedDeltaUs, sourceSha,
                true, false, true);
    }

    static void waitForDirectRetryRecord(Context context, OperationObserver observer)
            throws Exception {
        SharedPreferences preferences = context.getSharedPreferences(
                "session", Context.MODE_PRIVATE);
        Identity identity = readStoredIdentity(context, true);
        long owner = preferences.getLong("session_owner", 0L);
        require(owner != 0, "stored race owner is missing");
        String receiptPath = "/data/local/tmp/a9tas-g10-direct-retry-" +
                identity.pid + "-" + System.currentTimeMillis() + ".txt";
        File cancellationSignal = observer == null ? null :
                observer.openCancellationSignal();
        try {
            String cancellationPath = cancellationSignal == null ? null :
                    cancellationSignal.getCanonicalPath();
            RootShell.Result waited = runControllerFileReceipt(
                    identity, "wait-activation", owner, receiptPath, null,
                    CAPACITY, 330L, 1, cancellationPath);
            String waitText = text(waited.output);
            if (hasToken(waitText, "cancelled=1")) {
                boolean archived = cancelResidentPending(identity, owner,
                        receiptPath + ".cancel.txt", CAPACITY);
                require(preferences.edit()
                                .putBoolean("resident_pending_queued", false)
                                .remove("resident_pending_frame_limit")
                                .putBoolean("resident_retry_auto_loop", false)
                                // If Retry won just before cancellation, its
                                // current generation remains a valid recording
                                // and can be rejoined by the next Record action.
                                .putBoolean("resident_retry_record_armed", !archived)
                                .putBoolean("brush_archived_record_ready", archived)
                                .commit(),
                        "cancelled direct Retry state could not be persisted");
                throw new OperationCancelledException();
            }
            require(waited.ok() && waitText.contains("G4_PENDING_WAIT passed=1") &&
                            hasToken(waitText, "activated=1"),
                    "direct Retry activation failed: " + tail(waitText));
            require(preferences.edit()
                            .putBoolean("resident_retry_record_armed", true)
                            .putBoolean("resident_retry_auto_loop", true)
                            .putBoolean("resident_pending_queued", false)
                            .remove("resident_pending_frame_limit")
                            .putBoolean("brush_archived_record_ready", false)
                            .commit(),
                    "direct Retry activation receipt could not be persisted");
        } finally {
            if (observer != null && cancellationSignal != null)
                observer.closeCancellationSignal(cancellationSignal);
        }
    }

    private static void queueDirectRetryRecord(Identity identity, long owner,
                                               String prefix, int capacity)
            throws Exception {
        RootShell.Result queued = runControllerFileReceipt(
                identity, "queue-active-record", owner,
                prefix + ".auto-retry.txt", null, capacity, 35L);
        String receipt = text(queued.output);
        require(queued.ok() && receipt.contains("G4_ACTION action=27 ") &&
                        hasToken(receipt, "status=1") &&
                        hasToken(receipt, "detached=1"),
                "direct Retry resident queue rejected: " + tail(receipt));
    }

    /** Removes only the payload-owned pending next-race action. The installed
     * hooks and the current/archived run stay resident. Returns true when the
     * predecessor is an archived session ready for immediate rearm. */
    private static boolean cancelResidentPending(
            Identity identity, long owner, String receiptPath, int frameLimit)
            throws Exception {
        RootShell.Result cancelled = runControllerFileReceipt(
                identity, "cancel-pending", owner, receiptPath,
                null, frameLimit, 35L);
        String text = text(cancelled.output);
        boolean archived = hasToken(text, "status=2");
        require(cancelled.ok() && text.contains("G4_ACTION action=28 ") &&
                        (archived || hasToken(text, "status=1")) &&
                        hasToken(text, "detached=1"),
                "resident pending action could not be cancelled: " + tail(text));
        return archived;
    }

    /** Waits inside one native controller process for the next lifecycle-2
     * Retry object and immediately queues ESC.  Keeping one /proc/PID/mem fd
     * open removes the old repeated-root polling delay; the only side effect is
     * the same ordinary pause key that the user could press. */
    static void waitForRetryCountdown(Context context, OperationObserver observer)
            throws Exception {
        SharedPreferences preferences = context.getSharedPreferences(
                "session", Context.MODE_PRIVATE);
        require(preferences.getBoolean("session_hooks_installed", false) &&
                        preferences.getBoolean("brush_archived_record_ready", false),
                "completed brush session is not ready for Retry");
        Identity identity = readStoredIdentity(context, true);
        long owner = preferences.getLong("session_owner", 0L);
        require(owner != 0, "stored race owner is missing");
        String receiptPath = "/data/local/tmp/a9tas-g10-retry-wait-" +
                identity.pid + ".txt";
        File cancellationSignal = observer == null ? null :
                observer.openCancellationSignal();
        try {
            String cancellationPath = cancellationSignal == null ? null :
                    cancellationSignal.getCanonicalPath();
            if (cancellationPath != null)
                require(safePath(cancellationPath), "unsafe cancellation signal path");
            RootShell.Result waited = runControllerFileReceipt(
                    identity, "wait-retry-pause", owner, receiptPath, null,
                    preferences.getInt("session_capacity", CAPACITY), 250L,
                    1, cancellationPath);
            String status = text(waited.output);
            if (hasToken(status, "cancelled=1"))
                throw new OperationCancelledException();
            require(waited.ok() && status.contains(
                            "G10_CONTROLLER_FILE_RECEIPT action=wait-retry-pause") &&
                            status.contains("G8_RETRY_WAIT passed=1") &&
                            hasToken(status, "lifecycle=2") &&
                            hasToken(status, "valid=1") &&
                            hasToken(status, "pause=1"),
                    "Retry countdown could not be detected and paused: " + tail(status));
        } finally {
            if (observer != null && cancellationSignal != null)
                observer.closeCancellationSignal(cancellationSignal);
        }
    }

    /** Compatibility boundary for callers written before waitForRetryCountdown
     * also performed the pause.  No second ESC is sent: doing so would unpause
     * the newly detected race and recreate the visible pause/resume bounce. */
    static void pauseRetryCountdown(Context context) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences(
                "session", Context.MODE_PRIVATE);
        long owner = preferences.getLong("session_owner", 0L);
        require(owner != 0, "stored race owner is missing");
        require(preferences.getBoolean("brush_archived_record_ready", false),
                "Retry pause receipt is not attached to an archived run");
    }

    private static void waitAfterHandoffArm(OperationObserver observer,
                                            int seconds) throws Exception {
        long deadline = android.os.SystemClock.elapsedRealtime() + seconds * 1000L;
        int lastRemaining = -1;
        while (true) {
            throwIfCancelled(observer);
            long remainingMillis = deadline - android.os.SystemClock.elapsedRealtime();
            int remaining = remainingMillis <= 0 ? 0 :
                    (int) ((remainingMillis + 999L) / 1000L);
            if (remaining != lastRemaining && observer != null) {
                observer.onHandoffArmed(remaining);
                lastRemaining = remaining;
            }
            if (remainingMillis <= 0) return;
            Thread.sleep(Math.min(200L, remainingMillis));
        }
    }

    static ReplayReceipt replaySelected(Context context, OperationObserver observer) throws Exception {
        return replaySelected(context, observer, false, false, false);
    }

    static ReplayReceipt replaySelected(Context context, OperationObserver observer,
                                        boolean retainAtTarget) throws Exception {
        return replaySelected(context, observer, retainAtTarget, false, false);
    }

    static ReplayReceipt replaySelected(Context context, OperationObserver observer,
                                        boolean retainAtTarget,
                                        boolean gameAlreadyForeground) throws Exception {
        return replaySelected(context, observer, retainAtTarget,
                gameAlreadyForeground, false);
    }

    static ReplayReceipt replaySelected(Context context, OperationObserver observer,
                                        boolean retainAtTarget,
                                        boolean gameAlreadyForeground,
                                        boolean prearmContinuation) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        require(preferences.getBoolean("session_hooks_installed", false),
                "install the paused race session before replay");
        Identity identity = readStoredIdentity(context, true);
        verifyFixedArtifacts(context);
        requirePracticeMode(context, identity);
        long owner = preferences.getLong("session_owner", 0L);
        require(owner != 0, "stored race owner is missing");
        A9TasLibrary.Entry selected = A9TasLibrary.selected(context);
        A9TasArchive.Summary archive = selected.summary;
        JSONObject game = archive.manifest.getJSONObject("game");
        boolean experimental = preferences.getBoolean("prepared_experimental_bypass", false);
        boolean unknownBuildFallback = experimental &&
                BuildProfileRegistry.load(context).find(identity.nativeSha) == null;
        // A profile SHA identifies the tool-generated address/configuration file,
        // not the underlying game build.  Extending that file (for example with
        // replay-speed RVAs) must not invalidate recordings made against the same
        // native binary.  Keep the archived profile SHA for audit, but bind replay
        // compatibility to the native image and its ELF Build ID.
        require((unknownBuildFallback ||
                        identity.nativeSha.equals(game.getString("native_sha256"))) &&
                        identity.profile.buildId.equals(game.getString("build_id")),
                "selected recording belongs to a different game build; channel package names may differ");
        long targetTick = archive.targetTick;
        String targetBinding = preferences.getString("replay_target_archive_sha", "");
        if (selected.archiveSha256.equals(targetBinding))
            targetTick = preferences.getLong("replay_target_tick", archive.targetTick);
        require(targetTick >= 0 && targetTick < archive.frameCount,
                "selected replay target tick is invalid");
        require(archive.frameCount > 0 && archive.frameCount <= CAPACITY,
                "selected recording exceeds runtime capacity");
        File replaySource = A9TasLibrary.materializeReplaySource(
                context, selected, targetTick);
        A9TasArchive.SourceSummary replaySummary = A9TasArchive.inspectSource(replaySource);
        int ticks = Math.toIntExact(targetTick + 1);
        boolean pauseAtTarget = preferences.getBoolean("replay_pause_at_target", false);
        require(!prearmContinuation || retainAtTarget && pauseAtTarget,
                "continuation prearm requires an exact retained target pause");
        // New branch operations deliberately use an in-game pause request.  The
        // legacy wait-stop/resume-stop actions remain available only to recover
        // a session left frozen by an older APK; they are never armed here.
        boolean hardStopAtTarget = false;
        int replaySpeed = preferences.getInt("replay_speed_factor", 1);
        require(replaySpeed == 1 || replaySpeed == 2 || replaySpeed == 4 || replaySpeed == 8,
                "selected replay speed is invalid");
        require(replaySummary.frameCount == ticks,
                "materialized target replay length changed");
        require(replaySummary.fixedDeltaUs == archive.fixedDeltaUs,
                "materialized replay fixed-delta changed");
        String sourceSha = A9TasLibrary.sha256(replaySource);
        String remoteSource = "/data/local/tmp/a9tas-g10-replay-" + sourceSha + ".a9g4r2";
        String prefix = "/data/local/tmp/a9tas-g10-replay-session-" + identity.pid + "-" +
                System.currentTimeMillis();
        boolean armAttempted = false;
        boolean armConfirmed = false;
        boolean armRejectedCleanly = false;
        boolean restored = false;
        boolean processHardStopped = false;
        boolean continuationPrearmed = false;
        boolean archivedRearm = preferences.getBoolean(
                "brush_archived_record_ready", false);
        try {
            String localPath = replaySource.getCanonicalPath();
            require(safePath(localPath) && safePath(remoteSource), "unsafe replay staging path");
            ArtifactRegistry.IdentityHelper replayHelper = ArtifactRegistry
                    .loadAndVerify(context).identityHelperFor(identity.backend.hostMachine);
            require(replayHelper != null, "replay identity helper unavailable");
            String replayProbe = devicePath(replayHelper.deviceName);
            String stage = "set -eu; rm -f " + remoteSource + "; cp " + localPath + " " +
                    remoteSource + "; chmod 0600 " + remoteSource + "; " + replayProbe + " --expect " +
                    sourceSha + " " + remoteSource + "; " +
                    "echo G10_REPLAY_SOURCE_STAGED bytes=$(wc -c < " + remoteSource +
                    ") sha256=" + sourceSha;
            RootShell.Result staged = RootShell.runFixedScript(stage, 45L);
            String stageText = text(staged.output);
            require(staged.ok() && stageText.contains("G10_REPLAY_SOURCE_STAGED") &&
                            stageText.contains("sha256=" + sourceSha),
                    "replay source staging failed: " + tail(stageText));

            // A cancelled continuous-brush waiter may leave a valid next-race
            // action queued in the resident payload.  Switching to an explicit
            // replay means replacing that choice, not restoring/reinstalling
            // every hook or trying to resolve the retired outer owner.
            if (preferences.getBoolean("resident_pending_queued", false)) {
                archivedRearm = cancelResidentPending(identity, owner,
                        prefix + ".cancel-pending.txt",
                        preferences.getInt("resident_pending_frame_limit",
                                preferences.getInt("session_capacity", CAPACITY)));
                require(archivedRearm,
                        "the current recording is still active; pause or Retry before replaying the selected recording");
                require(preferences.edit()
                                .putBoolean("resident_pending_queued", false)
                                .putBoolean("resident_retry_record_armed", false)
                                .putBoolean("resident_retry_auto_loop", false)
                                .putBoolean("brush_archived_record_ready", true)
                                .remove("resident_pending_frame_limit")
                                .commit(),
                        "cancelled resident queue state could not be persisted");
            }

            armAttempted = true;
            String armAction = archivedRearm ? "rearm-replay" : "replay";
            int expectedAction = archivedRearm ? 13 : 9;
            int replayBarrierMode = prearmContinuation ? 2 : pauseAtTarget ? 1 : 0;
            RootShell.Result armed = runControllerFileReceipt(
                    identity, armAction, owner, prefix + ".arm.txt",
                    remoteSource, ticks, 45L, replaySpeed, replayBarrierMode);
            String armText = text(armed.output);
            armRejectedCleanly = armText.contains(
                            "G4_TICK_COORDINATOR_CONTROLLER passed=0") &&
                    hasToken(armText, "uncertain=0") &&
                    hasToken(armText, "detached=1") &&
                    hasToken(armText, "process_killed=0");
            require(armed.ok() && armText.contains(
                            "G4_ACTION action=" + expectedAction + " ") &&
                            hasToken(armText, "status=1") && hasToken(armText, "ticks=0"),
                    "replay arm receipt rejected: " + tail(armText));
            armConfirmed = true;
            if (archivedRearm)
                preferences.edit().putBoolean(
                        "brush_archived_record_ready", false).apply();

            String launcher = resolveLauncher(context, identity.packageName);
            RootShell.Result resumed = RootShell.runFixedScript(
                    (gameAlreadyForeground ? "" :
                            "am start -n " + launcher + " >/dev/null; sleep 1; ") +
                            "input keyevent 111; echo G10_REPLAY_GAME_FOREGROUND esc=1", 15L);
            require(resumed.ok() && text(resumed.output).contains(
                            "G10_REPLAY_GAME_FOREGROUND esc=1"),
                    "unable to resume paused replay");

            String status = prearmContinuation ?
                    waitForCompletionPauseAndRearm(identity, owner, prefix, ticks,
                            observer) : hardStopAtTarget ?
                    waitForCompletionAndStop(identity, owner, prefix, ticks, observer) :
                    pauseAtTarget ?
                    waitForCompletionAndPause(identity, owner, prefix, ticks, observer) :
                    waitForCompletion(identity, owner, prefix, ticks, ticks, observer, false);
            continuationPrearmed = prearmContinuation;
            processHardStopped = hardStopAtTarget;
            if (!hardStopAtTarget) throwIfCancelled(observer);
            if (!prearmContinuation) {
                for (String token : new String[]{"complete=1", "ticks=" + ticks,
                        "begin=" + ticks, "final=" + ticks,
                        "end=" + ticks, "error=0", "completion=0,1"})
                    require(hasToken(status, token), "replay receipt missing " + token);
                validateIntegrationReceipt(status, ticks, replaySummary.fixedDeltaUs);
            }

            if (!prearmContinuation) {
                RootShell.Result diagnostic = runControllerStdoutReceipt(
                        identity, "diff", owner, prefix + ".a9g5d1", null, ticks,
                        prefix + ".diff.stdout.txt", 60L);
                String diagnosticText = text(diagnostic.output);
                require(diagnostic.ok() && diagnosticText.contains(
                                "G5_REPLAY_DIAGNOSTIC_DUMP passed=1 frames=" + ticks + " "),
                        "replay diagnostic receipt rejected: " + tail(diagnosticText));
                throwIfCancelled(observer);
            }

            // Keep the completed runtime resident, mirroring the upstream DLL
            // manager.  A later record/replay operation reuses these hooks and
            // only rearms mode plus lifecycle ownership.
            restored = true;
            SharedPreferences.Editor replayState = preferences.edit()
                    .putBoolean("session_hooks_installed", true)
                     .putBoolean("brush_archived_record_ready", true)
                     .putBoolean("branch_replay_ready", retainAtTarget)
                     .putString("last_replay_archive", selected.file.getAbsolutePath())
                     .putString("last_replay_recording_sha", sourceSha)
                     .putLong("last_replay_target_tick", targetTick)
                     .putLong("last_replay_fixed_delta_us", replaySummary.fixedDeltaUs)
                     .putInt("last_replay_ticks", ticks)
                     .putBoolean("branch_runtime_prearmed", continuationPrearmed);
            if (hardStopAtTarget) replayState
                    .putBoolean("game_process_hard_paused", true)
                    .putInt("hard_paused_pid", identity.pid)
                    .putLong("hard_paused_start_ticks", identity.startTicks);
            boolean replayStatePublished = replayState.commit();
            require(replayStatePublished,
                    "replay/session ownership state could not be persisted");
            return new ReplayReceipt(ticks, replaySummary.intervalCount,
                    replaySummary.nitroCalls, replaySummary.barrelFrames,
                    replaySummary.fixedDeltaUs, sourceSha,
                    pauseAtTarget, hardStopAtTarget, continuationPrearmed);
        } catch (Exception error) {
            if (processHardStopped) {
                try {
                    RootShell.Result release = runControllerFileReceipt(
                            identity, "resume-stop", owner, prefix + ".failed-resume.txt",
                            null, ticks, 20L);
                    if (hasToken(text(release.output), "resumed=1")) processHardStopped = false;
                } catch (Exception ignored) {}
            }
            if (error instanceof ReplayInterruptedException && retainAtTarget &&
                    (((ReplayInterruptedException) error).automaticPause ||
                            (observer != null && observer.replayInterruptionRequested()))) {
                ReplayInterruptedException interrupted =
                        (ReplayInterruptedException) error;
                // The user paused the game and explicitly chose to branch at
                // the last complete replay tick.  Keep the already-installed
                // runtime intact; the next transaction will rearm it directly
                // from replay to record without restoring or replaying again.
                restored = true;
                boolean retained = preferences.edit()
                        .putBoolean("session_hooks_installed", true)
                        .putBoolean("branch_replay_ready", true)
                        .putBoolean("branch_runtime_prearmed", false)
                        .putString("last_replay_archive",
                                selected.file.getAbsolutePath())
                        .putString("last_replay_recording_sha", sourceSha)
                        .putLong("last_replay_target_tick", interrupted.ticks - 1L)
                        .putLong("last_replay_fixed_delta_us",
                                replaySummary.fixedDeltaUs)
                        .putInt("last_replay_ticks", interrupted.ticks).commit();
                require(retained,
                        "interrupted replay ownership state could not be persisted");
                throw new ReplayInterruptedException(
                        interrupted.ticks, sourceSha, interrupted.automaticPause);
            }
            if (!restored) {
                if (armAttempted && !armConfirmed) {
                    // A controller receipt with uncertain=0/detached=1 proves
                    // that no uncertain game mutation remains.  Preserve the
                    // archived runtime for another Retry instead of killing a
                    // healthy game solely because the countdown/object gate
                    // was missed.
                    if (!armRejectedCleanly) {
                        try {
                            terminate(identity.packageName);
                            clearTerminatedRuntimeState(context);
                        } catch (Exception ignored) {}
                    }
                } else {
                    rollbackOrTerminate(context, identity, prefix, owner,
                            armConfirmed ? ticks : CAPACITY);
                }
            }
            throw error;
        }
    }

    /**
     * Requests a checkpoint at the next fully closed authoritative game Tick.
     * The native controller holds that exact dispatcher boundary, queues ESC,
     * seals the record, and releases the game only after publishing a receipt.
     * A clean timeout means the game was probably already paused, so the caller
     * may use the legacy paused-state fallback without resuming it.
     */
    static boolean checkpointAtNextClosedTick(Context context) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences(
                "session", Context.MODE_PRIVATE);
        require(preferences.getBoolean("session_hooks_installed", false),
                "install the race session before checkpointing");
        Identity identity = readStoredIdentity(context, true);
        long owner = preferences.getLong("session_owner", 0L);
        require(owner != 0, "stored race owner is missing");
        int capacity = preferences.getInt("session_capacity", CAPACITY);
        require(capacity > 0 && capacity <= CAPACITY,
                "stored session capacity is invalid");
        String output = "/data/local/tmp/a9tas-g10-checkpoint-" + identity.pid + "-" +
                System.currentTimeMillis() + ".txt";
        RootShell.Result checkpoint = runControllerFileReceipt(
                identity, "checkpoint-pause", owner, output, null, capacity, 20L);
        String receipt = text(checkpoint.output);
        if (checkpoint.ok() && receipt.contains("G4_ACTION action=29 ") &&
                hasToken(receipt, "status=2") && hasToken(receipt, "detached=1") &&
                hasToken(receipt, "checkpoint=1") && hasToken(receipt, "pause=1") &&
                hasToken(receipt, "release=1"))
            return true;
        if (receipt.contains("stage=checkpoint_boundary_timeout") &&
                hasToken(receipt, "uncertain=0") && hasToken(receipt, "detached=1") &&
                hasToken(receipt, "process_killed=0"))
            return false;
        throw new IOException("exact checkpoint rejected: " + tail(receipt));
    }

    private static Identity readPreparedIdentity(Context context) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        require(preferences.getBoolean("prepared_ready", false),
                "fresh process is not PREPARED");
        require(!preferences.getBoolean("session_hooks_installed", false),
                "session hooks are already installed");
        return readStoredIdentity(context, false);
    }

    /**
     * Clears only process-bound state when its exact PID/start-time identity is gone.
     * Recording-library and selected-archive preferences are deliberately preserved.
     */
    static boolean reconcileTerminatedRuntimeState(Context context) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences(
                "session", Context.MODE_PRIVATE);
        // A prepare-only process has no installed hooks yet, but its PID-bound
        // metadata can become stale for the same reason as a resident session
        // (for example, a replacement Prepare transaction force-stopped the
        // old process and then failed).  Reconcile both shapes by exact
        // PID/start-time identity; recording-library state is never removed.
        if (!preferences.getBoolean("session_hooks_installed", false) &&
                !preferences.getBoolean("prepared_ready", false)) return false;
        int pid = preferences.getInt("prepared_pid", 0);
        long startTicks = preferences.getLong("prepared_start_ticks", 0L);
        require(pid > 0 && startTicks > 0,
                "installed-session identity is incomplete; do not discard a possibly live hook");
        String script = "state=stale; if [ -r /proc/" + pid + "/stat ]; then " +
                "st=$(sed 's/^[^)]*) //' /proc/" + pid +
                "/stat); set -- $st; s=${20}; " +
                "[ \"$s\" = \"" + startTicks + "\" ] && state=live; fi; " +
                "echo G10_RUNTIME_RECONCILE state=$state";
        RootShell.Result checked = RootShell.runFixedScript(script, 10L);
        String output = text(checked.output);
        require(checked.ok() && output.contains("G10_RUNTIME_RECONCILE state="),
                "unable to reconcile stored session identity");
        if (output.contains("G10_RUNTIME_RECONCILE state=live")) return false;
        require(output.contains("G10_RUNTIME_RECONCILE state=stale"),
                "invalid runtime reconciliation receipt");
        clearTerminatedRuntimeState(context);
        return true;
    }

    private static void clearTerminatedRuntimeState(Context context) {
        context.getSharedPreferences("session", Context.MODE_PRIVATE).edit()
                .putBoolean("session_hooks_installed", false)
                .putBoolean("prepared_ready", false)
                .putBoolean("brush_archived_record_ready", false)
                .putBoolean("branch_replay_ready", false)
                .putBoolean("branch_runtime_prearmed", false)
                .putBoolean("resident_retry_record_armed", false)
                .putBoolean("resident_retry_auto_loop", false)
                .putBoolean("resident_pending_queued", false)
                .remove("resident_pending_frame_limit")
                .putBoolean("record_waiting_retry", false)
                .remove("prepared_pid").remove("prepared_start_ticks")
                .remove("prepared_profile_id").remove("prepared_process")
                .remove("prepared_package").remove("prepared_native_sha")
                .remove("prepared_host_machine").remove("prepared_native_bridge")
                .remove("prepared_bridge_set").remove("prepared_libc_sha")
                .remove("prepared_runtime_backend")
                .remove("prepared_experimental_bypass")
                .remove("session_owner").remove("session_base")
                .remove("session_capacity")
                .remove("practice_proof_pid")
                .remove("practice_proof_start_ticks")
                .remove("practice_proof_object")
                .remove("practice_proof_profile")
                .putBoolean("branch_pending", false)
                .remove("branch_pending_archive")
                .remove("branch_pending_archive_sha")
                .remove("branch_pending_target_tick")
                .remove("branch_pending_source_sha")
                .remove("branch_pending_pid")
                .remove("branch_pending_start_ticks")
                .remove("branch_pending_continuous")
                .putBoolean("game_process_hard_paused", false)
                .remove("hard_paused_pid").remove("hard_paused_start_ticks").apply();
    }

    private static void verifyFixedArtifacts(Context context) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        boolean experimental = preferences.getBoolean("prepared_experimental_bypass", false);
        ArtifactRegistry registry = ArtifactRegistry.loadAndVerify(context);
        ArtifactRegistry.Backend backend = experimental ? registry.findAnyById(
                preferences.getString("prepared_runtime_backend", "")) : registry.findById(
                preferences.getString("prepared_runtime_backend", ""));
        require(backend != null, "prepared runtime backend is unavailable");
        ArtifactRegistry.IdentityHelper helper = registry.identityHelperFor(backend.hostMachine);
        require(helper != null, "prepared identity helper is unavailable");
        String probe = devicePath(helper.deviceName);
        BuildProfileRegistry profiles = BuildProfileRegistry.load(context);
        String nativeSha = preferences.getString("prepared_native_sha", "");
        String profileId = preferences.getString("prepared_profile_id", "");
        BuildProfileRegistry.Profile profile = profiles.resolve(nativeSha, profileId, experimental);
        require(profile != null && profile.id.equals(profileId),
                "prepared BuildProfile does not match the current game build; prepare the game again");
        StringBuilder script = new StringBuilder("set -eu;");
        script.append(probe).append(" --expect ").append(helper.sha256).append(' ')
                .append(probe).append(';');
        for (ArtifactRegistry.Artifact artifact : backend.artifacts) {
            script.append(probe).append(" --expect ").append(artifact.sha256)
                    .append(" /data/local/tmp/").append(artifact.deviceName).append(';');
        }
        script.append(probe).append(" --expect ").append(profile.profileSha256)
                .append(' ').append(PROFILE).append(';');
        script.append("echo G10_SESSION_ARTIFACTS verified=")
                .append(backend.artifacts.size() + 1);
        RootShell.Result result = RootShell.runFixedScript(script.toString(), 30L);
        String output = text(result.output);
        require(result.ok() && output.contains("G10_SESSION_ARTIFACTS verified=" +
                (backend.artifacts.size() + 1)), "fixed runtime artifacts changed");
    }

    /**
     * Read-only authorization boundary for all TAS state transitions.
     *
     * Build-profile bypasses intentionally do not bypass this check.  The
     * dedicated PracticeRace object is a 0x750-byte, five-vptr lifecycle object
     * owned by the active CN practice race.  Menus and the practice picker do
     * not contain this complete shape.  Exactly one live object is required.
     */
    private static long requirePracticeMode(Context context, Identity identity)
            throws Exception {
        if (DeveloperBuild.enabled(context)) {
            boolean persisted = context.getSharedPreferences("session", Context.MODE_PRIVATE)
                    .edit().putInt("practice_proof_pid", identity.pid)
                    .putLong("practice_proof_start_ticks", identity.startTicks)
                    .putLong("practice_proof_object", 0L)
                    .putString("practice_proof_profile", identity.profile.id)
                    .putBoolean("practice_proof_bypassed_devtest", true).commit();
            require(persisted, "开发者测试模式回执无法持久化");
            return 0L;
        }
        String probe = devicePath(identity.backend.practiceProbeDeviceName);
        String command = probe + " " + identity.pid + " " +
                Long.toUnsignedString(identity.base, 16) + " " +
                Long.toUnsignedString(identity.profile.practiceVptr0Rva, 16) + " " +
                Long.toUnsignedString(identity.profile.practiceVptr588Rva, 16) + " " +
                Long.toUnsignedString(identity.profile.practiceVptr6c0Rva, 16) + " " +
                Long.toUnsignedString(identity.profile.practiceVptr718Rva, 16) + " " +
                Long.toUnsignedString(identity.profile.practiceVptr748Rva, 16) + " 2";
        RootShell.Result checked = RootShell.runFixedScript(command, 20L);
        String output = text(checked.output);
        boolean unique = output.contains("A9PRACTICE_DISCOVERY_V1 passed=1 ") &&
                hasToken(output, "candidates=1") &&
                hasToken(output, "capped=0");
        Matcher candidate = Pattern.compile(
                "(?m)^A9PRACTICE_CANDIDATE_V1 index=0 address=(0x[0-9a-fA-F]+) ")
                .matcher(output);
        require(checked.ok() && unique && candidate.find() &&
                        !output.contains("A9PRACTICE_CANDIDATE_V1 index=1 "),
                "仅允许在国服独立“练习”模式的比赛中启用 TAS；" +
                        "当前未检测到唯一练习赛生命周期对象：" + tail(output));
        long object = Long.parseUnsignedLong(candidate.group(1).substring(2), 16);
        boolean persisted = context.getSharedPreferences("session", Context.MODE_PRIVATE)
                .edit().putInt("practice_proof_pid", identity.pid)
                .putLong("practice_proof_start_ticks", identity.startTicks)
                .putLong("practice_proof_object", object)
                .putString("practice_proof_profile", identity.profile.id).commit();
        require(persisted, "练习模式回执无法持久化");
        return object;
    }

    private static Identity readStoredIdentity(Context context, boolean useStoredBase) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        int pid = preferences.getInt("prepared_pid", 0);
        long startTicks = preferences.getLong("prepared_start_ticks", 0L);
        String packageName = preferences.getString("prepared_package", "");
        String processName = preferences.getString("prepared_process", "");
        String nativeSha = preferences.getString("prepared_native_sha", "");
        String profileId = preferences.getString("prepared_profile_id", "");
        String hostMachine = preferences.getString("prepared_host_machine", "");
        String bridgeSet = preferences.getString("prepared_bridge_set", "");
        String libcSha = preferences.getString("prepared_libc_sha", "");
        String backendId = preferences.getString("prepared_runtime_backend", "");
        boolean experimental = preferences.getBoolean("prepared_experimental_bypass", false);
        require(pid > 0 && startTicks > 0 && safePackage(packageName) && safeProcess(processName),
                "stored fresh-process identity is incomplete");
        BuildProfileRegistry profiles = BuildProfileRegistry.load(context);
        BuildProfileRegistry.Profile profile = profiles.resolve(nativeSha, profileId, experimental);
        require(profile != null && profile.id.equals(profileId),
                "stored BuildProfile differs from the measured game build; prepare the game again");
        ArtifactRegistry runtimeRegistry = ArtifactRegistry.loadAndVerify(context);
        ArtifactRegistry.Backend backend = experimental ? runtimeRegistry.findAnyById(backendId) :
                runtimeRegistry.findById(backendId);
        require(backend != null && backend.hostMachine.equals(hostMachine) &&
                        backend.bridgeSet.equals(bridgeSet) &&
                        (experimental || backend.matches(hostMachine, bridgeSet, libcSha)),
                "stored runtime backend changed");
        ArtifactRegistry.IdentityHelper helper = runtimeRegistry.identityHelperFor(hostMachine);
        require(helper != null, "stored identity helper changed");
        String probe = devicePath(helper.deviceName);

        String script = "if [ ! -d /proc/" + pid + " ]; then echo G10_SESSION_PROCESS_GONE; exit 72; fi; " +
                "[ -r /proc/" + pid + "/maps ]; " +
                "st=$(sed 's/^[^)]*) //' /proc/" + pid + "/stat) || exit 73; set -- $st; s=${20:-}; " +
                "[ -n \"$s\" ] || exit 73; if [ \"$s\" != \"" + startTicks +
                "\" ]; then echo G10_SESSION_PROCESS_GONE; exit 72; fi; " +
                "grep -q '^TracerPid:[[:space:]]*0$' /proc/" + pid + "/status; " +
                "bases=; while read range perms offset dev inode mapped rest; do " +
                "if [ \"$offset\" = 00000000 ]; then case \"$mapped\" in *libAsphalt9.so*) " +
                "base=${range%-*}; case \" $bases \" in *\" $base \"*) :;; *) bases=\"$bases $base\";; esac;; esac; fi; " +
                "done </proc/" + pid + "/maps; " +
                "set -- $bases; [ $# -eq 1 ]; b=$1; " +
                "lib_line=$(grep -m1 'libAsphalt9[.]so' /proc/" + pid + "/maps); " +
                "lib=${lib_line#* /}; [ -n \"$lib\" ] && [ \"$lib\" != \"$lib_line\" ]; " +
                (experimental ? "" : probe + " --expect " + nativeSha + " \"$lib\"; ") +
                "grep -q '/" + backend.payloadDeviceName + "' /proc/" + pid + "/maps; " +
                (backend.bootstrapDeviceName == null ? "" :
                        "grep -q '" + devicePath(backend.bootstrapDeviceName) +
                                "' /proc/" + pid + "/maps; ") +
                "echo G10_SESSION_IDENTITY base=$b";
        RootShell.Result checked = RootShell.runFixedScript(script, 25L);
        String output = text(checked.output);
        if (output.contains("G10_SESSION_PROCESS_GONE")) {
            clearTerminatedRuntimeState(context);
            throw new IOException("原游戏进程已退出，已清理失效会话；录像仍保留。请重新扫描并准备游戏。");
        }
        Matcher match = Pattern.compile("(?m)^G10_SESSION_IDENTITY base=([0-9a-fA-F]+)$").matcher(output);
        require(checked.ok() && match.find(), "fresh process identity verification failed: " + tail(output));
        long base = Long.parseUnsignedLong(match.group(1), 16);
        if (useStoredBase)
            require(base == preferences.getLong("session_base", 0L), "stored game base changed");
        Identity identity = new Identity(pid, startTicks, base, packageName, processName,
                nativeSha, profile, backend);
        identity.recordDeltaUs = stableRecordDeltaUs(preferences);
        return identity;
    }

    // Only an invalid new-recording setting is migrated. Replay/continuation keeps
    // the archive's own delta; silently relabelling an existing file is wrong.
    static void validateIntegrationReceipt(String status, int ticks, long deltaUs)
            throws java.io.IOException {
        Matcher integrated = Pattern.compile("(?:^|\\s)interval=([0-9]+)(?:\\s|$)")
                .matcher(status);
        Matcher calls = Pattern.compile("(?:^|\\s)interval_calls=([0-9]+)(?:\\s|$)")
                .matcher(status);
        require(ticks > 0 && integrated.find() && calls.find(),
                "completion receipt has no integration counters");
        long frames, count;
        try {
            frames = Long.parseLong(integrated.group(1));
            count = Long.parseLong(calls.group(1));
        } catch (NumberFormatException invalid) {
            throw new java.io.IOException("completion integration counter overflow", invalid);
        }
        boolean sparse = deltaUs == 8333 || deltaUs == 6944;
        require(frames <= ticks && count >= frames
                        && (frames != 0 || count == 0)
                        && (sparse || frames == ticks),
                "completion integration counters disagree: ticks=" + ticks
                        + " integrated=" + frames + " calls=" + count);
    }

    static int stableRecordDeltaUs(android.content.SharedPreferences preferences) {
        int requested = preferences.getInt("record_tick_hz", 60);
        if (requested == 120) return 8333;
        if (requested == 144) return 6944;
        if (requested != 60) preferences.edit()
                .putInt("record_tick_hz_before_compatibility_reset", requested)
                .putInt("record_tick_hz", 60).commit();
        return 16667;
    }

    private static String waitForCompletion(Identity identity, long owner, String prefix,
                                            int capacity, int progressLimit,
                                            OperationObserver observer,
                                            boolean waitIndefinitelyAtTickZero) throws Exception {
        long deadline = System.nanoTime() + 240_000_000_000L;
        int lastTicks = -1;
        long lastProgressNanos = System.nanoTime();
        while (System.nanoTime() < deadline) {
            if (observer != null && observer.cancellationRequested())
                throw new OperationCancelledException();
            String progressPath = prefix + ".progress.txt";
            RootShell.Result polled = runControllerFileReceipt(
                    identity, "status", owner, progressPath, null, capacity, 35L);
            String status = text(polled.output);
            require(polled.ok() && status.contains(
                            "G10_CONTROLLER_FILE_RECEIPT action=status"),
                    "progress receipt transport failed: " + tail(status));
            Matcher ticksMatch = Pattern.compile(
                    "G4_STATUS\\s+complete=([01])\\s+ticks=([0-9]+)")
                    .matcher(status);
            require(ticksMatch.find(), "progress receipt is malformed: " + tail(status));
            boolean complete = "1".equals(ticksMatch.group(1));
            int ticks = Integer.parseInt(ticksMatch.group(2));
            require(ticks >= lastTicks && ticks <= capacity,
                    "progress tick cursor is invalid: " + tail(status));
            Matcher errorMatch = Pattern.compile("(?:^|\\s)error=([0-9]+)(?:\\s|$)")
                    .matcher(status);
            require(errorMatch.find(), "progress receipt has no error field");
            require("0".equals(errorMatch.group(1)),
                    "runtime reported an error: " + runtimeFailureDetails(status));
            Matcher checks = Pattern.compile(
                    "(?:^|\\s)checks=1,1,([01]),([01]),1,1\\s+reject=([0-9]+)")
                    .matcher(status);
            require(checks.find(), "progress identity checks failed: " + tail(status));
            if (complete)
                require("1".equals(checks.group(1)) && "1".equals(checks.group(2)),
                        "terminal progress receipt is incomplete: " + tail(status));
            else if (hasToken(status, "control=0,1") &&
                    status.contains(" evidence=2,") &&
                    hasToken(status, "reject=1")) {
                // PublishCompletion runs inside the final hook.  For a very
                // short interval the completion flags are visible while the
                // outer dispatcher still owns active_helpers=1.  This is a
                // terminal-settling receipt, not an unarmed runtime.
                Thread.sleep(10L);
                continue;
            } else
                require(status.contains(" control=1,0 "),
                        "active progress control is not armed: " + tail(status));
            if (ticks != lastTicks) {
                lastProgressNanos = System.nanoTime();
                // This is an inactivity limit, not a maximum recording length.
                // Advance it even when the caller has no UI observer.
                deadline = lastProgressNanos + 240_000_000_000L;
                if (observer != null) observer.onProgress(ticks, progressLimit);
                lastTicks = ticks;
            }
            if (complete) return status;
            // An explicit save uses the already published prefix immediately.
            // Keep race-end completion above it so a simultaneous finish wins.
            if (shouldSealExistingTicks(complete, ticks,
                    observer != null && observer.checkpointRequested()))
                throw new RecordingPausedException(ticks);
            // A manually resumed branch is already armed while the game is
            // paused.  Do not expire or classify that intentional tick-zero
            // wait as a recording stall.  Once the first authoritative tick
            // arrives, the ordinary bounded completion deadline applies.
            if (waitIndefinitelyAtTickZero && ticks == 0)
                deadline = System.nanoTime() + 240_000_000_000L;
            // The Android build currently has no exact IsPaused hook.  In the
            // opt-in brush-lap workflow, a race that has already advanced and
            // then stops producing authoritative tick receipts is treated as
            // a user pause.  Countdown tick 0 is deliberately excluded.
            if (observer != null && ticks > 0 &&
                    observer.stopRecordingWhenProgressStalls() &&
                    System.nanoTime() - lastProgressNanos >=
                            observer.progressStallTimeoutMillis() * 1_000_000L)
                throw new RecordingPausedException(ticks);
            // This is an observation transaction, not the TAS clock.  Polling
            // at display-like frequency only adds periodic root/controller
            // stalls; authoritative ticks continue inside the payload.
            // The payload is authoritative and keeps every tick while Java is
            // asleep.  Slow the purely observational root poll while waiting
            // for manual resume to reduce Superuser notices and frame hitches.
            long observationPoll = observer == null ? 750L :
                    Math.max(100L, observer.observationPollMillis());
            Thread.sleep(waitIndefinitelyAtTickZero && ticks == 0 ?
                    Math.max(500L, observationPoll * 4L) : observationPoll);
        }
        throw new IOException("operation made no tick progress for 240 seconds");
    }

    private static String waitForCompletionAndPause(
            Identity identity, long owner, String prefix, int ticks,
            OperationObserver observer) throws Exception {
        String receiptPath = prefix + ".wait-pause.txt";
        File cancellationSignal = observer == null ? null :
                observer.openCancellationSignal();
        try {
            String cancellationPath = cancellationSignal == null ? null :
                    cancellationSignal.getCanonicalPath();
            if (cancellationPath != null)
                require(safePath(cancellationPath), "unsafe cancellation signal path");
            RootShell.Result waited = runControllerFileReceipt(
                    identity, "wait-pause", owner, receiptPath, null, ticks, 210L,
                    1, cancellationPath);
            String status = text(waited.output);
            require(waited.ok() && status.contains(
                            "G10_CONTROLLER_FILE_RECEIPT action=wait-pause"),
                    "target-tick pause transport rejected: " + tail(status));
            Matcher ticksMatch = Pattern.compile(
                    "G4_STATUS\\s+complete=([01])\\s+ticks=([0-9]+)").matcher(status);
            require(ticksMatch.find(),
                    "target-tick pause has no replay cursor: " + tail(status));
            int observedTicks = Integer.parseInt(ticksMatch.group(2));
            if (hasToken(status, "cancelled=1")) {
                if (observer != null && observer.replayInterruptionRequested() &&
                        observedTicks > 0 && observedTicks < ticks)
                    throw new ReplayInterruptedException(observedTicks, "");
                throw new OperationCancelledException();
            }
            if (hasToken(status, "early_pause=1")) {
                require(hasToken(status, "target_pause=1") && observedTicks > 0 &&
                                observedTicks < ticks,
                        "early target pause has an invalid replay cursor: " + tail(status));
                throw new ReplayInterruptedException(observedTicks, "", true);
            }
            require(hasToken(status, "target_barrier=1") &&
                            hasToken(status, "target_pause=1") &&
                            hasToken(status, "target_release=1"),
                    "target-tick pause receipt rejected: " + tail(status));
            require("1".equals(ticksMatch.group(1)) && observedTicks == ticks,
                    "target-tick pause completed at an unexpected cursor: " + tail(status));
            if (observer != null) observer.onProgress(ticks, ticks);
            throwIfCancelled(observer);
            return status;
        } finally {
            if (observer != null && cancellationSignal != null)
                observer.closeCancellationSignal(cancellationSignal);
        }
    }

    /**
     * Exact branch handoff used by the normal brush-lap path.  One native
     * controller remains resident from the replay completion barrier through
     * the pause request and retained-runtime rearm.  The returned receipt is
     * already an armed lifecycle-record session; Java must not launch a second
     * rearm-branch transaction for it.
     */
    private static String waitForCompletionPauseAndRearm(
            Identity identity, long owner, String prefix, int replayTicks,
            OperationObserver observer) throws Exception {
        String receiptPath = prefix + ".wait-pause-rearm.txt";
        File cancellationSignal = observer == null ? null :
                observer.openCancellationSignal();
        try {
            String cancellationPath = cancellationSignal == null ? null :
                    cancellationSignal.getCanonicalPath();
            if (cancellationPath != null)
                require(safePath(cancellationPath), "unsafe cancellation signal path");
            RootShell.Result handedOff = runControllerFileReceipt(
                    identity, "wait-pause-rearm", owner, receiptPath, null,
                    CAPACITY, 210L, 1, cancellationPath);
            String status = text(handedOff.output);
            if (hasToken(status, "cancelled=1"))
                throw new OperationCancelledException();
            require(handedOff.ok() && status.contains(
                            "G10_CONTROLLER_FILE_RECEIPT action=wait-pause-rearm") &&
                            status.contains("G4_ACTION action=23 ") &&
                            hasToken(status, "limit=" + CAPACITY) &&
                            hasToken(status, "status=1") &&
                            hasToken(status, "prefix_ticks=" + replayTicks) &&
                            hasToken(status, "handoff=1") &&
                            hasToken(status, "detached=1"),
                    "exact replay-to-record handoff rejected: " + tail(status));
            if (observer != null) observer.onProgress(replayTicks, replayTicks);
            return status;
        } finally {
            if (cancellationSignal != null && cancellationSignal.exists())
                cancellationSignal.delete();
        }
    }

    private static String waitForCompletionAndStop(
            Identity identity, long owner, String prefix, int ticks,
            OperationObserver observer) throws Exception {
        String receiptPath = prefix + ".wait-stop.txt";
        File cancellationSignal = observer == null ? null :
                observer.openCancellationSignal();
        try {
            String cancellationPath = cancellationSignal == null ? null :
                    cancellationSignal.getCanonicalPath();
            if (cancellationPath != null)
                require(safePath(cancellationPath), "unsafe cancellation signal path");
            RootShell.Result waited = runControllerFileReceipt(
                    identity, "wait-stop", owner, receiptPath, null, ticks, 210L,
                    1, cancellationPath);
            String status = text(waited.output);
            require(waited.ok() && status.contains(
                            "G10_CONTROLLER_FILE_RECEIPT action=wait-stop"),
                    "target-tick hard stop transport rejected: " + tail(status));
            Matcher ticksMatch = Pattern.compile(
                    "G4_STATUS\\s+complete=([01])\\s+ticks=([0-9]+)").matcher(status);
            require(ticksMatch.find(),
                    "target-tick hard stop has no replay cursor: " + tail(status));
            int observedTicks = Integer.parseInt(ticksMatch.group(2));
            if (hasToken(status, "cancelled=1")) {
                if (observer != null && observer.replayInterruptionRequested() &&
                        observedTicks > 0 && observedTicks < ticks)
                    throw new ReplayInterruptedException(observedTicks, "");
                throw new OperationCancelledException();
            }
            require(hasToken(status, "target_pause=1") &&
                            hasToken(status, "target_stop=1") &&
                            hasToken(status, "target_resume=0"),
                    "target-tick hard stop receipt rejected: " + tail(status));
            require("1".equals(ticksMatch.group(1)) && observedTicks == ticks,
                    "target-tick hard stop completed at an unexpected cursor: " + tail(status));
            if (observer != null) observer.onProgress(ticks, ticks);
            return status;
        } finally {
            if (observer != null && cancellationSignal != null)
                observer.closeCancellationSignal(cancellationSignal);
        }
    }

    /**
     * Release the exact process frozen by wait-stop.  The native controller
     * first verifies the retained replay receipt and /proc identity, queues one
     * ESC while the game is stopped, and only then sends SIGCONT.  The caller
     * may safely rearm the branch after this returns because the game is in its
     * pause menu rather than advancing past the splice boundary.
     */
    static String resumeHardStoppedAtPauseMenu(Context context) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        require(preferences.getBoolean("game_process_hard_paused", false),
                "no hard-paused game process is waiting");
        Identity identity = readStoredIdentity(context, true);
        require(identity.pid == preferences.getInt("hard_paused_pid", 0) &&
                        identity.startTicks == preferences.getLong("hard_paused_start_ticks", 0L),
                "hard-paused process identity changed");
        long owner = preferences.getLong("session_owner", 0L);
        int ticks = preferences.getInt("last_replay_ticks", 0);
        require(owner != 0 && ticks > 0, "hard-paused replay ownership is incomplete");
        String output = "/data/local/tmp/a9tas-g10-hard-resume-" + identity.pid + "-" +
                System.currentTimeMillis() + ".txt";
        RootShell.Result resumed = runControllerFileReceipt(
                identity, "resume-stop", owner, output, null, ticks, 20L);
        String status = text(resumed.output);
        boolean processResumed = hasToken(status, "resumed=1");
        if (processResumed) {
            preferences.edit().putBoolean("game_process_hard_paused", false)
                    .remove("hard_paused_pid").remove("hard_paused_start_ticks").apply();
        }
        require(resumed.ok() && status.contains("G8_HARD_RESUME passed=1") &&
                        status.contains("G10_CONTROLLER_FILE_RECEIPT action=resume-stop"),
                "hard-pause release rejected: " + tail(status));
        return status;
    }

    private static void throwIfCancelled(OperationObserver observer)
            throws OperationCancelledException {
        if (observer != null && observer.cancellationRequested())
            throw new OperationCancelledException();
    }

    private static String controller(Identity identity, String action, long owner,
                                     String output, String replayInput, int capacity,
                                     int replaySpeed) {
        return controller(identity, action, owner, output, replayInput, capacity,
                replaySpeed, 0, null);
    }

    private static String controller(Identity identity, String action, long owner,
                                     String output, String replayInput, int capacity,
                                     int replaySpeed, int replayCompletionMode,
                                     String cancellationSignal) {
        StringBuilder command = new StringBuilder("A9TAS_RECORD_DELTA_US=")
                .append(identity.recordDeltaUs).append(' ')
                .append(devicePath(identity.backend.controllerDeviceName))
                .append(' ').append(action).append(' ')
                .append(identity.pid).append(' ').append(identity.startTicks).append(' ')
                .append(Long.toHexString(identity.base)).append(' ')
                .append(Long.toHexString(owner)).append(' ').append(capacity).append(' ');
        if (replayInput != null && !replayInput.isEmpty()) command.append(replayInput).append(' ');
        command.append(output).append(' ');
        if ("replay".equals(action) || "rearm-replay".equals(action) ||
                "queue-replay".equals(action))
            command.append(replaySpeed).append(' ')
                    .append(replayCompletionMode).append(' ');
        if (("wait-pause".equals(action) ||
                "wait-pause-rearm".equals(action) || "wait-stop".equals(action) ||
                "wait-retry-pause".equals(action) ||
                "wait-activation".equals(action)) &&
                cancellationSignal != null)
            command.append(cancellationSignal).append(' ');
        return command.append(ACK).toString();
    }

    private static String devicePath(String deviceName) {
        if (deviceName == null || !deviceName.matches("[A-Za-z0-9_.-]+"))
            throw new IllegalArgumentException("unsafe backend artifact name");
        return "/data/local/tmp/" + deviceName;
    }

    /**
     * The controller's stdout crosses a persistent root/NativeBridge boundary and is not an
     * authoritative completion receipt. Actions in this path write their semantic receipt to
     * {@code output}; read that exact file back in the same root script before returning to Java.
     */
    private static RootShell.Result runControllerFileReceipt(
            Identity identity, String action, long owner, String output,
            String replayInput, int capacity, long timeoutSeconds) throws Exception {
        return runControllerFileReceipt(identity, action, owner, output,
                replayInput, capacity, timeoutSeconds, 1);
    }

    private static RootShell.Result runControllerFileReceipt(
            Identity identity, String action, long owner, String output,
            String replayInput, int capacity, long timeoutSeconds,
            int replaySpeed) throws Exception {
        return runControllerFileReceipt(identity, action, owner, output, replayInput,
                capacity, timeoutSeconds, replaySpeed, false, null);
    }

    private static RootShell.Result runControllerFileReceipt(
            Identity identity, String action, long owner, String output,
            String replayInput, int capacity, long timeoutSeconds,
            int replaySpeed, boolean replayCompletionBarrier) throws Exception {
        return runControllerFileReceipt(identity, action, owner, output, replayInput,
                capacity, timeoutSeconds, replaySpeed, replayCompletionBarrier, null);
    }

    private static RootShell.Result runControllerFileReceipt(
            Identity identity, String action, long owner, String output,
            String replayInput, int capacity, long timeoutSeconds,
            int replaySpeed, String cancellationSignal) throws Exception {
        return runControllerFileReceipt(identity, action, owner, output, replayInput,
                capacity, timeoutSeconds, replaySpeed, false, cancellationSignal);
    }

    private static RootShell.Result runControllerFileReceipt(
            Identity identity, String action, long owner, String output,
            String replayInput, int capacity, long timeoutSeconds,
            int replaySpeed, boolean replayCompletionBarrier,
            String cancellationSignal) throws Exception {
        return runControllerFileReceipt(identity, action, owner, output, replayInput,
                capacity, timeoutSeconds, replaySpeed,
                replayCompletionBarrier ? 1 : 0, cancellationSignal);
    }

    private static RootShell.Result runControllerFileReceipt(
            Identity identity, String action, long owner, String output,
            String replayInput, int capacity, long timeoutSeconds,
            int replaySpeed, int replayCompletionMode) throws Exception {
        return runControllerFileReceipt(identity, action, owner, output, replayInput,
                capacity, timeoutSeconds, replaySpeed, replayCompletionMode, null);
    }

    private static RootShell.Result runControllerFileReceipt(
            Identity identity, String action, long owner, String output,
            String replayInput, int capacity, long timeoutSeconds,
            int replaySpeed, int replayCompletionMode,
            String cancellationSignal) throws Exception {
        require(action.matches("[a-z-]+"), "unsafe controller action");
        require(replayCompletionMode >= 0 && replayCompletionMode <= 2,
                "invalid replay completion mode");
        require(safePath(output), "unsafe controller receipt path");
        if (cancellationSignal != null)
            require(safePath(cancellationSignal), "unsafe cancellation signal path");
        String transport = output + ".transport.txt";
        require(safePath(transport), "unsafe controller transport path");
        String script = "rm -f " + output + " " + transport + "; " +
                controller(identity, action, owner, output, replayInput, capacity,
                        replaySpeed, replayCompletionMode, cancellationSignal) +
                " >" + transport + " 2>&1; a9tas_controller_rc=$?; " +
                "if [ ! -s " + output + " ]; then " +
                "cat " + transport + "; " +
                "echo G10_CONTROLLER_FILE_RECEIPT action=" + action +
                " rc=$a9tas_controller_rc missing=1; exit 71; fi; " +
                "cat " + output + "; echo G10_CONTROLLER_FILE_RECEIPT action=" +
                action + " rc=$a9tas_controller_rc missing=0; rm -f " + transport;
        return RootShell.runFixedScript(script, timeoutSeconds, "controller-" + action);
    }

    private static boolean provenNoMutationInstallFailure(String receipt) {
        if (receipt == null) return false;
        Matcher failure = Pattern.compile(
                "(?m)^G4_TICK_COORDINATOR_CONTROLLER passed=0 " +
                "stage=(artifact_runtime|open_mem|freeze|native_stopped_identity|" +
                "build_profile_publish|clean_original_precondition) code=[0-9]+ uncertain=0 detached=1 " +
                "process_killed=0 errno=[0-9]+$").matcher(receipt);
        return failure.find();
    }

    /**
     * Dump/diff actions use {@code output} for binary data, so preserve stdout in a separate,
     * bounded receipt file and read that file back instead of trusting the direct pipe.
     */
    private static RootShell.Result runControllerStdoutReceipt(
            Identity identity, String action, long owner, String output,
            String replayInput, int capacity, String receipt,
            long timeoutSeconds) throws Exception {
        require(action.matches("[a-z-]+"), "unsafe controller action");
        require(safePath(output) && safePath(receipt), "unsafe controller output path");
        String script = "rm -f " + receipt + "; " +
                controller(identity, action, owner, output, replayInput, capacity, 1) +
                " >" + receipt + " 2>&1; a9tas_controller_rc=$?; " +
                "if [ ! -s " + receipt + " ]; then " +
                "echo G10_CONTROLLER_STDOUT_RECEIPT action=" + action +
                " rc=$a9tas_controller_rc missing=1; exit 72; fi; " +
                "cat " + receipt + "; echo G10_CONTROLLER_STDOUT_RECEIPT action=" +
                action + " rc=$a9tas_controller_rc missing=0";
        return RootShell.runFixedScript(script, timeoutSeconds, "controller-" + action);
    }

    /**
     * A race can publish its immutable completion receipt from inside the final
     * lifecycle/dispatcher wrapper.  The wrapper releases active_helpers only
     * while returning to the game.  If the dump controller freezes the process
     * in that tiny interval, every recording count is already final but the
     * strict dump gate correctly reports G4_COMPLETE_REJECT code=1 active=1.
     *
     * Keep the native validation strict.  Retry only this fully identified
     * terminal-settling receipt; any missing evidence, runtime error, or other
     * rejection is returned immediately to the caller.
     */
    private static RootShell.Result dumpCompletedRecordingAfterHelpersSettle(
            Identity identity, long owner, String output, int capacity,
            String receipt) throws Exception {
        RootShell.Result result = null;
        for (int attempt = 0; attempt < 4; attempt++) {
            result = runControllerStdoutReceipt(identity, "dump", owner, output,
                    null, capacity, receipt, 45L);
            String dumpText = text(result.output);
            if (dumpText.contains("G4_RECORD_DUMP passed=1 ")) return result;
            if (!isCompletedHelperSettleRace(dumpText)) return result;
            Thread.sleep(25L * (attempt + 1L));
        }
        return result;
    }

    private static boolean isCompletedHelperSettleRace(String receipt) {
        if (receipt == null ||
                !receipt.contains("G4_COMPLETE_REJECT code=1 ") ||
                !receipt.contains("G4_RECORD_DUMP passed=0 ") ||
                !hasToken(receipt, "status=2") ||
                !hasToken(receipt, "error=0") ||
                !hasToken(receipt, "missing=0x0") ||
                !hasToken(receipt, "completion=2/2")) return false;
        Matcher active = Pattern.compile(
                "(?:^|\\s)active=([1-9][0-9]*)(?:\\s|$)").matcher(receipt);
        return active.find();
    }

    private static void rollbackOrTerminate(Context context, Identity identity,
                                             String prefix, long owner) {
        rollbackOrTerminate(context, identity, prefix, owner, CAPACITY);
    }

    private static void rollbackOrTerminate(Context context, Identity identity,
                                             String prefix, long owner, int capacity) {
        try {
            if (restoreController(identity, owner, prefix + ".rollback.txt", capacity)) {
                context.getSharedPreferences("session", Context.MODE_PRIVATE).edit()
                        .putBoolean("session_hooks_installed", false)
                        .putBoolean("brush_archived_record_ready", false)
                        .putBoolean("resident_retry_record_armed", false)
                        .putBoolean("resident_retry_auto_loop", false)
                        .putBoolean("resident_pending_queued", false)
                        .remove("resident_pending_frame_limit")
                        .putBoolean("record_waiting_retry", false)
                        .putBoolean("branch_replay_ready", false)
                        .putBoolean("branch_runtime_prearmed", false)
                        .putBoolean("branch_pending", false)
                        .remove("branch_pending_archive")
                        .remove("branch_pending_archive_sha")
                        .remove("branch_pending_target_tick")
                        .remove("branch_pending_source_sha")
                        .remove("branch_pending_pid")
                        .remove("branch_pending_start_ticks")
                        .remove("branch_pending_continuous")
                        .putBoolean("game_process_hard_paused", false)
                        .remove("hard_paused_pid").remove("hard_paused_start_ticks")
                        .remove("session_owner").remove("session_base").apply();
                return;
            }
        } catch (Exception ignored) {
            // The only permitted fallback for an unproven restore is termination.
        }
        try {
            terminate(identity.packageName);
            clearTerminatedRuntimeState(context);
        } catch (Exception ignored) {}
    }

    private static boolean restoreController(Identity identity, long owner,
                                             String output, int capacity) throws Exception {
        RootShell.Result restored = runControllerFileReceipt(
                identity, "restore", owner, output, null, capacity, 35L);
        String receipt = text(restored.output);
        return restored.ok() && receipt.contains("G4_ACTION action=3 ") &&
                hasToken(receipt, "status=3") && hasToken(receipt, "detached=1");
    }

    private static File copyRecordingIntoApp(Context context, String source,
                                             int ticks) throws Exception {
        return copyRecordingIntoApp(context, source, ticks,
                "recordings", "recording-");
    }

    private static File copyRecordingIntoApp(Context context, String source,
                                             int ticks, String folder,
                                             String prefix) throws Exception {
        File directory = new File(context.getFilesDir(), folder);
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("unable to create recording library");
        File output = new File(directory, prefix + System.currentTimeMillis() +
                "-" + ticks + ".a9g4r2");
        File pending = new File(output.getAbsolutePath() + ".pending");
        if (!safePath(source) || !safePath(output.getAbsolutePath()) ||
                !safePath(pending.getAbsolutePath()))
            throw new IOException("unsafe recording path");
        int uid = Process.myUid();
        String script = "set -eu; [ -f " + source + " ]; rm -f " + pending.getAbsolutePath() +
                "; cp " + source + " " + pending.getAbsolutePath() +
                "; chown " + uid + ":" + uid + " " + pending.getAbsolutePath() +
                "; chmod 0600 " + pending.getAbsolutePath() + "; mv " +
                pending.getAbsolutePath() + " " + output.getAbsolutePath() +
                "; echo G10_RECORDING_COPY bytes=$(wc -c < " + output.getAbsolutePath() + ")";
        RootShell.Result copied = RootShell.runFixedScript(script, 45L);
        String receipt = text(copied.output);
        require(copied.ok() && receipt.matches(
                        "(?s).*G10_RECORDING_COPY bytes=[0-9]+.*"),
                "recording copy receipt rejected: " + tail(receipt));
        require(output.isFile() && output.length() > A9TasArchive.A9G4R2_HEADER_SIZE,
                "recording is not readable by the app");
        return output;
    }

    private static void terminate(String packageName) throws Exception {
        if (!safePackage(packageName)) throw new IOException("unsafe package identity");
        RootShell.Result stopped = RootShell.runFixedScript("am force-stop " + packageName, 10L);
        if (!stopped.ok()) throw new IOException("unable to terminate uncertain game process");
    }

    private static String resolveLauncher(Context context, String packageName) throws IOException {
        Intent launch = context.getPackageManager().getLaunchIntentForPackage(packageName);
        ComponentName component = launch == null ? null : launch.getComponent();
        String flattened = component == null ? "" : component.flattenToString();
        if (!flattened.matches("[A-Za-z0-9_.]+/[A-Za-z0-9_.$]+") ||
                !flattened.startsWith(packageName + "/"))
            throw new IOException("verified game launcher unavailable");
        return flattened;
    }

    private static String between(List<String> lines, String begin, String end) throws IOException {
        boolean active = false;
        StringBuilder output = new StringBuilder();
        for (String line : lines) {
            if (line.equals(begin)) { active = true; continue; }
            if (line.equals(end)) {
                if (!active) break;
                return output.toString();
            }
            if (active) output.append(line);
        }
        throw new IOException("observation payload markers missing");
    }

    private static String text(List<String> lines) { return String.join("\n", lines); }
    static String objectFailureDetails(String value) {
        StringBuilder diagnostics = new StringBuilder();
        for (String line : value.split("\n")) {
            if (line.startsWith("G4_OBJECT_DIAG ") && diagnostics.length() < 1200)
                diagnostics.append(line).append('\n');
        }
        return diagnostics.toString() + tail(value);
    }
    // Preserve the callback counters and fault origin, not just the last UI
    // fields. A tail-only exception made remote first-tick faults impossible
    // to distinguish from identity failures and late frame callbacks.
    static String runtimeFailureDetails(String value) {
        StringBuilder result = new StringBuilder();
        for (String key : new String[]{"error", "fault_context", "fault_tick",
                "interval_probe", "tick_config", "idle_final",
                "coordinator_result", "coordinator_intervals", "coordinator",
                "core_results", "begin", "interval", "interval_calls",
                "final", "end", "ticks", "records", "checks", "reject"}) {
            Matcher field = Pattern.compile("(?:^|\\s)" + Pattern.quote(key)
                    + "=([^\\s]+)").matcher(value);
            if (field.find()) result.append(key).append('=').append(field.group(1)).append(' ');
        }
        return result.append('\n').append(value.length() <= 4096
                ? value : value.substring(0, 3072) + "\n[truncated]\n" + tail(value)).toString();
    }
    private static String tail(String value) {
        return value.length() <= 480 ? value : value.substring(value.length() - 480);
    }
    private static boolean safePackage(String value) {
        return value != null && value.matches("[A-Za-z0-9_]+(?:\\.[A-Za-z0-9_]+)+");
    }
    private static boolean safeProcess(String value) {
        return value != null && value.matches("[A-Za-z0-9._:-]{1,191}");
    }
    private static boolean safePath(String value) {
        return value != null && value.matches("/[A-Za-z0-9_./-]{1,511}") &&
                !value.contains("..") && !value.contains("//");
    }
    private static boolean hasToken(String text, String token) {
        return Pattern.compile("(?:^|\\s)" + Pattern.quote(token) + "(?:\\s|$)")
                .matcher(text).find();
    }
    private static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) >= 0) digest.update(buffer, 0, count);
        }
        StringBuilder output = new StringBuilder(64);
        for (byte value : digest.digest())
            output.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return output.toString();
    }
    private static void require(boolean value, String message) throws IOException {
        if (!value) throw new IOException(message);
    }
}
