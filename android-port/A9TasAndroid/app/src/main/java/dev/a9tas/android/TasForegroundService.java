package dev.a9tas.android;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.IBinder;

import java.io.File;
import java.io.IOException;
import java.util.concurrent.atomic.AtomicBoolean;

public final class TasForegroundService extends Service {
    private static volatile TasForegroundService liveInstance;
    static final String ACTION_START = "dev.a9tas.android.START";
    static final String ACTION_INSTALL_SESSION = "dev.a9tas.android.INSTALL_SESSION";
    static final String ACTION_RECORD_LIFECYCLE = "dev.a9tas.android.RECORD_LIFECYCLE";
    static final String ACTION_REPLAY_SELECTED = "dev.a9tas.android.REPLAY_SELECTED";
    static final String ACTION_QUICK_RECORD = "dev.a9tas.android.QUICK_RECORD";
    static final String ACTION_QUICK_REPLAY = "dev.a9tas.android.QUICK_REPLAY";
    static final String ACTION_BRANCH_RECORD = "dev.a9tas.android.BRANCH_RECORD";
    static final String ACTION_CHECKPOINT_BRANCH =
            "dev.a9tas.android.CHECKPOINT_BRANCH";
    static final String ACTION_INTERRUPT_REPLAY =
            "dev.a9tas.android.INTERRUPT_REPLAY";
    static final String ACTION_CANCEL = "dev.a9tas.android.CANCEL";
    static final String ACTION_RESTORE = "dev.a9tas.android.RESTORE";
    static final String ACTION_STOP = "dev.a9tas.android.STOP";
    static final String ACTION_SHOW_OVERLAY = "dev.a9tas.android.SHOW_OVERLAY";
    static final String ACTION_HIDE_OVERLAY = "dev.a9tas.android.HIDE_OVERLAY";
    static final String ACTION_OVERLAY_CHECKPOINT =
            "dev.a9tas.android.OVERLAY_CHECKPOINT";
    static final String ACTION_RESUME_HARD_PAUSE =
            "dev.a9tas.android.RESUME_HARD_PAUSE";
    static final String EXTRA_PID = "pid";
    static final String EXTRA_START_TICKS = "start_ticks";
    static final String EXTRA_PROFILE_ID = "profile_id";
    static final String EXTRA_PROCESS = "process";
    static final String EXTRA_PACKAGE = "package";
    static final String EXTRA_NATIVE_SHA = "native_sha";
    static final String EXTRA_HOST_MACHINE = "host_machine";
    static final String EXTRA_NATIVE_BRIDGE = "native_bridge";
    static final String EXTRA_BRIDGE_SET = "bridge_set";
    static final String EXTRA_RUNTIME_BACKEND = "runtime_backend";
    static final String EXTRA_LIBC_SHA = "libc_sha";
    static final String EXTRA_EXPERIMENTAL_BYPASS = "experimental_bypass";
    static final String EXTRA_GAME_ALREADY_FOREGROUND = "game_already_foreground";
    private static final String CHANNEL_ID = "a9tas_session";
    private static final int NOTIFICATION_ID = 9001;
    private final Object operationLock = new Object();
    private final AtomicBoolean operationActive = new AtomicBoolean(false);
    private final AtomicBoolean cancelRequested = new AtomicBoolean(false);
    // One-shot fallback for a game that was already paused before an exact
    // dispatcher-boundary checkpoint could be requested. This remains separate
    // from the persistent "save whenever gameplay pauses" preference.
    private final AtomicBoolean checkpointRequested = new AtomicBoolean(false);
    // While true, the native dispatcher-boundary transaction owns checkpoint
    // detection. The older progress-stall fallback must not race it.
    private final AtomicBoolean exactCheckpointActive = new AtomicBoolean(false);
    // One-shot request to convert the currently replayed immutable prefix into
    // a branch point at its last complete authoritative tick.
    private final AtomicBoolean replayInterruptRequested = new AtomicBoolean(false);
    private volatile File activeCancellationSignal;
    private volatile boolean stopAfterOperation;
    private volatile boolean branchAfterWaitingCancellation;
    private volatile boolean branchAfterCleanWaitingCancellation;
    private volatile boolean freshRecordAfterCleanOperation;
    private TasOverlayController overlayController;

    @Override public void onCreate() {
        super.onCreate();
        DiagnosticBundle.installCrashHandler(this);
        liveInstance = this;
        android.content.SharedPreferences session =
                getSharedPreferences("session", MODE_PRIVATE);
        if (!session.getBoolean("branch_handoff_mode_v2", false))
            session.edit().putBoolean("branch_handoff_mode_v2", true)
                    .putBoolean("branch_auto_handoff", false)
                    .putBoolean("branch_manual_resume", true).apply();
        overlayController = new TasOverlayController(this);
        // A WindowManager view cannot survive this service process.  Never
        // expose a stale "visible" toggle after an update or process reclaim.
        getSharedPreferences("session", MODE_PRIVATE).edit()
                .putBoolean("overlay_visible", false).apply();
        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel channel = new NotificationChannel(CHANNEL_ID,
                    getString(R.string.service_channel), NotificationManager.IMPORTANCE_LOW);
            getSystemService(NotificationManager.class).createNotificationChannel(channel);
        }
        if (getSharedPreferences("session", MODE_PRIVATE)
                .getBoolean("operation_active", false)) {
            boolean hooks = getSharedPreferences("session", MODE_PRIVATE)
                    .getBoolean("session_hooks_installed", false);
            boolean prepared = getSharedPreferences("session", MODE_PRIVATE)
                    .getBoolean("prepared_ready", false);
            getSharedPreferences("session", MODE_PRIVATE).edit()
                    .putBoolean("operation_active", false)
                    .putBoolean("cancel_pending", false)
                    .putString("state", hooks ? "RECOVERY_REQUIRED" :
                            prepared ? "PREPARED" : "READY")
                    .putString("detail", hooks ?
                            "上次操作中断且游戏 Hook 仍可能存在；请恢复一次" :
                            "上次界面操作已自动清理 · 可以直接重试")
                    .apply();
        }
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        String action = intent == null ? "" : intent.getAction();
        final boolean gameAlreadyForeground = intent != null &&
                intent.getBooleanExtra(EXTRA_GAME_ALREADY_FOREGROUND, false);
        // MainActivity starts this component with startForegroundService() on
        // Android 8+.  Promotion must therefore happen before diagnostics,
        // preference I/O, or any of the one-shot action early returns below.
        // Otherwise RESTORE/CANCEL/HIDE/STOP can make Android kill the process
        // with "did not then call Service.startForeground".
        int pid = intent == null ? 0 : intent.getIntExtra(EXTRA_PID, 0);
        long startTicks = intent == null ? 0L : intent.getLongExtra(EXTRA_START_TICKS, 0L);
        String profile = intent == null ? null : intent.getStringExtra(EXTRA_PROFILE_ID);
        String detail = pid > 0 && startTicks > 0 && profile != null
                ? "Bound to PID " + pid + " · " + profile
                : getString(R.string.service_idle);
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL_ID) : new Notification.Builder(this);
        Notification notification = notification(builder, detail, false);
        startForeground(NOTIFICATION_ID, notification);
        // CANCEL/checkpoint/interrupt and duplicate taps are signals belonging
        // to the operation already in flight.  Giving every incoming Intent a
        // new diagnostic id made the remaining root receipts and any failure
        // appear under the wrong button press.  Main-thread dispatch sets
        // operationActive before returning, so the accepted operation keeps
        // one stable diagnostic identity until its worker finishes.
        if (action != null && !action.isEmpty() && !operationActive.get())
            DiagnosticBundle.beginOperation(this, action);
        if (ACTION_HIDE_OVERLAY.equals(action)) {
            overlayController.hide();
            return START_NOT_STICKY;
        }
        if (ACTION_CANCEL.equals(action)) {
            requestCancellation(false);
            return START_NOT_STICKY;
        }
        if (ACTION_RESTORE.equals(action)) {
            if (operationActive.get()) requestCancellation(false);
            else startRestore(false);
            return START_NOT_STICKY;
        }
        if (ACTION_STOP.equals(action)) {
            if (operationActive.get()) requestCancellation(true);
            else startRestore(true);
            return START_NOT_STICKY;
        }
        // Releasing an already-frozen game is recovery, not a newly privileged
        // TAS operation.  Never leave the user locked out of SIGCONT because a
        // research license expired while the exact breakpoint was retained.
        if (ACTION_RESUME_HARD_PAUSE.equals(action)) {
            runExclusive("branch", this::resumeHardPausedBranch);
            return START_NOT_STICKY;
        }
        try {
            LicenseManager.requireValid(this);
        } catch (Exception licenseError) {
            setState("LICENSE_REQUIRED", licenseError.getMessage() == null ?
                    "A valid research license is required" : licenseError.getMessage());
            updateNotification("Research license required · open A9 TAS", false);
            return START_NOT_STICKY;
        }
        if (ACTION_SHOW_OVERLAY.equals(action)) {
            if (!overlayController.show()) {
                setState("OVERLAY_PERMISSION_REQUIRED",
                        "请允许 A9 TAS 显示在其他应用上层");
                updateNotification("悬浮窗权限尚未授予 · 打开 A9 TAS", false);
            }
        } else if (ACTION_OVERLAY_CHECKPOINT.equals(action)) {
            requestOverlayCheckpoint();
        } else if (ACTION_CHECKPOINT_BRANCH.equals(action)) {
            requestCheckpointBranch();
        } else if (ACTION_INTERRUPT_REPLAY.equals(action)) {
            requestReplayInterruption();
        } else if (ACTION_START.equals(action)) {
            final String processName = intent.getStringExtra(EXTRA_PROCESS);
            final String packageName = intent.getStringExtra(EXTRA_PACKAGE);
            final String nativeSha = intent.getStringExtra(EXTRA_NATIVE_SHA);
            final String hostMachine = intent.getStringExtra(EXTRA_HOST_MACHINE);
            final boolean nativeBridge = intent.getBooleanExtra(EXTRA_NATIVE_BRIDGE, false);
            final String bridgeSet = intent.getStringExtra(EXTRA_BRIDGE_SET);
            final String runtimeBackend = intent.getStringExtra(EXTRA_RUNTIME_BACKEND);
            final String libcSha = intent.getStringExtra(EXTRA_LIBC_SHA);
            final boolean experimental = intent.getBooleanExtra(
                    EXTRA_EXPERIMENTAL_BYPASS, false);
            // Do not clear a live session before runExclusive has accepted
            // this request.  A duplicate Prepare tap used to be ignored by
            // runExclusive only after these flags had already been erased,
            // leaving real resident hooks/queues with fictitious host state.
            // prepare() publishes the complete replacement state only after
            // ArtifactDeployer returns a verified receipt.
            runExclusive("prepare", () -> prepare(pid, startTicks, processName,
                    packageName, nativeSha, hostMachine, nativeBridge, bridgeSet,
                    libcSha, profile, runtimeBackend, experimental));
        } else if (ACTION_INSTALL_SESSION.equals(action)) {
            runExclusive("install", this::installSession);
        } else if (ACTION_QUICK_RECORD.equals(action)) {
            boolean loadPrefix = getSharedPreferences("session", MODE_PRIVATE)
                    .getBoolean("continuous_load_selected", false);
            if (loadPrefix)
                runExclusive("branch", () -> branchRecord(true, true,
                        gameAlreadyForeground));
            else runExclusive("record", () -> recordLifecycle(true,
                    gameAlreadyForeground));
        } else if (ACTION_QUICK_REPLAY.equals(action)) {
            runExclusive("replay", () -> replaySelected(true, true,
                    gameAlreadyForeground));
        } else if (ACTION_BRANCH_RECORD.equals(action)) {
            runExclusive("branch", () -> branchRecord(false, true,
                    gameAlreadyForeground));
        } else if (ACTION_RECORD_LIFECYCLE.equals(action)) {
            runExclusive("record", () -> recordLifecycle(false));
        } else if (ACTION_REPLAY_SELECTED.equals(action)) {
            runExclusive("replay", () -> replaySelected(false));
        }
        return START_NOT_STICKY;
    }

    private interface ServiceOperation { void run() throws Exception; }

    private void runExclusive(String kind, ServiceOperation operation) {
        if (!operationActive.compareAndSet(false, true)) {
            // A double tap is not a session failure and must not overwrite the
            // state/progress of the operation that is actually running.
            preferences().edit().putString("overlay_feedback",
                    "上一项操作仍在执行，本次重复点击已忽略").apply();
            return;
        }
        cancelRequested.set(false);
        checkpointRequested.set(false);
        exactCheckpointActive.set(false);
        replayInterruptRequested.set(false);
        getSharedPreferences("session", MODE_PRIVATE).edit()
                .putBoolean("operation_active", true)
                .putBoolean("cancel_pending", false)
                .putString("operation_kind", kind)
                .putInt("operation_ticks", 0)
                .putInt("operation_limit", 0).apply();
        updateNotification(initialOperationLabel(kind),
                "record".equals(kind) || "replay".equals(kind) || "branch".equals(kind));
        new Thread(() -> {
            try {
                synchronized (operationLock) { operation.run(); }
            } catch (Exception error) {
                reportFailure("操作", error);
            } finally {
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putBoolean("operation_active", false)
                        .putBoolean("cancel_pending", false).apply();
                operationActive.set(false);
                cancelRequested.set(false);
                checkpointRequested.set(false);
                exactCheckpointActive.set(false);
                replayInterruptRequested.set(false);
                if (stopAfterOperation) {
                    stopAfterOperation = false;
                    branchAfterWaitingCancellation = false;
                    branchAfterCleanWaitingCancellation = false;
                    freshRecordAfterCleanOperation = false;
                    startRestore(true);
                } else if (branchAfterCleanWaitingCancellation) {
                    branchAfterCleanWaitingCancellation = false;
                    runExclusive("branch", () -> branchRecord(true));
                } else if (freshRecordAfterCleanOperation) {
                    freshRecordAfterCleanOperation = false;
                    runExclusive("record", () -> recordLifecycle(false));
                }
            }
        }, "a9tas-" + kind).start();
    }

    private SessionOrchestrator.OperationObserver observer(String kind) {
        return new SessionOrchestrator.OperationObserver() {
            private long lastNotification;
            private long lastProgressPublish;
            @Override public boolean cancellationRequested() {
                return cancelRequested.get();
            }
            @Override public boolean replayInterruptionRequested() {
                return replayInterruptRequested.get();
            }
            @Override public boolean stopRecordingWhenProgressStalls() {
                // A branch suffix is a real recording attempt too.  Pausing it
                // must seal the suffix so the immutable prefix can be spliced
                // with the newly accepted ticks.  Restricting this to the
                // initial "record" path made mid-branch checkpoints wait until
                // race end instead of following the brush-lap workflow.
                return ("record".equals(kind) || "branch".equals(kind)) &&
                        !exactCheckpointActive.get() &&
                        (checkpointRequested.get() ||
                         getSharedPreferences("session", MODE_PRIVATE)
                         .getBoolean("record_pause_interrupt", true));
            }
            @Override public long progressStallTimeoutMillis() {
                return getSharedPreferences("session", MODE_PRIVATE)
                        .getBoolean("control_low_latency", true) ? 900L : 2500L;
            }
            @Override public long observationPollMillis() {
                return getSharedPreferences("session", MODE_PRIVATE)
                        .getBoolean("control_low_latency", true) ? 250L : 750L;
            }
            @Override public void onProgress(int ticks, int limit) {
                long now = System.currentTimeMillis();
                android.content.SharedPreferences session =
                        getSharedPreferences("session", MODE_PRIVATE);
                if ("record".equals(kind) &&
                        "RECORD_ARMING".equals(session.getString("state", ""))) {
                    setState("RECORDING",
                            "录制运行时已开始接收完整 Tick · 直接 Retry 将丢弃本段并自动拦停下一局");
                }
                if ("branch".equals(kind) && ticks > 0 &&
                        "BRANCH_ARMED_PAUSED".equals(session.getString("state", ""))) {
                    setState("BRANCH_RECORDING",
                            "检测到手动继续 · 仅记录断点后的新 Tick");
                }
                long publishInterval = session.getBoolean(
                        "control_low_latency", true) ? 250L : 1000L;
                if (lastProgressPublish == 0L || now - lastProgressPublish >= publishInterval ||
                        limit > 0 && ticks >= limit) {
                    session.edit()
                            .putInt("operation_ticks", ticks)
                            .putInt("operation_limit", limit).apply();
                    lastProgressPublish = now;
                }
                if (now - lastNotification >= 3000L) {
                    String progress = limit > 0 ? ticks + " / " + limit + " ticks" :
                            ticks + " ticks · waiting for race completion";
                    updateNotification(kindLabel(kind) + " · " + progress, true);
                    lastNotification = now;
                }
            }
            @Override public File openCancellationSignal() throws IOException {
                File cache = getCacheDir().getCanonicalFile();
                File signal = new File(cache, "a9tas-cancel-" +
                        android.os.Process.myPid() + "-" +
                        Long.toHexString(System.nanoTime()) + ".signal").getCanonicalFile();
                if (!cache.equals(signal.getParentFile()) ||
                        !signal.getName().matches("a9tas-cancel-[0-9]+-[0-9a-f]+\\.signal"))
                    throw new IOException("invalid cancellation signal path");
                if (signal.exists() && !signal.delete())
                    throw new IOException("unable to clear stale cancellation signal");
                activeCancellationSignal = signal;
                if (cancelRequested.get()) publishCancellationSignal(signal);
                return signal;
            }
            @Override public void closeCancellationSignal(File signal) {
                if (activeCancellationSignal != null &&
                        activeCancellationSignal.equals(signal))
                    activeCancellationSignal = null;
                if (signal != null && signal.exists()) signal.delete();
            }
            @Override public void onHandoffArmed(int remainingSeconds) {
                if (!"branch".equals(kind)) return;
                setState("BRANCH_ARMED_PAUSED", remainingSeconds < 0 ?
                        "续录已武装 · 请在游戏中手动继续；第一个真实 Tick 开始录制" :
                        remainingSeconds > 0 ?
                        "续录已武装 · 游戏保持暂停 · " + remainingSeconds + " 秒后自动恢复" :
                        "续录已武装 · 正在自动恢复比赛");
                updateNotification(remainingSeconds < 0 ?
                        "续录已武装 · 等待你手动继续游戏" : remainingSeconds > 0 ?
                        "续录已武装 · " + remainingSeconds + " 秒后恢复" :
                        "续录已武装 · 正在恢复", true);
            }
            @Override public void onHandoffResumed() {
                if (!"branch".equals(kind)) return;
                setState("BRANCH_RECORDING", "续录已武装并恢复 · 仅记录断点后的新 Tick");
                updateNotification("正在录制续录后段", true);
            }
        };
    }

    private void requestOverlayCheckpoint() {
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        String kind = preferences.getString("operation_kind", "");
        if (!operationActive.get() ||
                !("record".equals(kind) || "branch".equals(kind))) {
            preferences.edit().putString("overlay_feedback",
                    "当前没有可封存的录制").apply();
            return;
        }
        // A continuous branch already owns one native Tick timeline and the
        // game is paused by the user at this point. Asking it to produce a new
        // boundary cannot succeed while paused and used to hold RootShell for
        // roughly ten seconds before falling back. Seal the last completely
        // published Tick directly; the native exporter excludes any open Tick.
        if ("branch".equals(kind) &&
                preferences.getBoolean("branch_pending_continuous", false)) {
            if (!checkpointRequested.compareAndSet(false, true)) {
                preferences.edit().putString("overlay_feedback",
                        "保存请求已经在处理中").apply();
                return;
            }
            preferences.edit().putString("overlay_feedback",
                    "正在保存最后一个完整 Tick").apply();
            updateNotification("游戏保持暂停 · 正在保存", true);
            return;
        }
        if (!exactCheckpointActive.compareAndSet(false, true)) {
            preferences.edit().putString("overlay_feedback",
                    "精确保存请求已经在处理中").apply();
            return;
        }
        preferences.edit().putString("overlay_feedback",
                "正在等待下一个完整 Tick · 将自动暂停并保存").apply();
        updateNotification("正在对齐完整 Tick 并保存", true);
        new Thread(() -> {
            try {
                boolean exact = SessionOrchestrator.checkpointAtNextClosedTick(this);
                if (exact) {
                    preferences.edit().putString("overlay_feedback",
                            "已在完整 Tick 边界暂停 · 正在导出录像").apply();
                } else {
                    // An already-paused game cannot produce the next boundary.
                    // Fall back to sealing its last fully closed Tick.
                    exactCheckpointActive.set(false);
                    checkpointRequested.set(true);
                    preferences.edit().putString("overlay_feedback",
                            "游戏已暂停 · 正在保存最后一个完整 Tick").apply();
                    updateNotification("游戏保持暂停 · 正在保存", true);
                    return;
                }
            } catch (Exception error) {
                preferences.edit().putString("overlay_feedback",
                        "精确保存失败 · " + (error.getMessage() == null ?
                                error.getClass().getSimpleName() : error.getMessage())).apply();
            } finally {
                exactCheckpointActive.set(false);
            }
        }, "a9tas-exact-checkpoint").start();
    }

    private void requestCheckpointBranch() {
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        boolean waiting = operationActive.get() &&
                preferences.getBoolean("record_waiting_retry", false) &&
                "record_waiting".equals(preferences.getString("operation_kind", ""));
        String selected = preferences.getString("selected_archive", "");
        if (!waiting || selected.isEmpty() || !new File(selected).isFile()) {
            preferences.edit().putString("overlay_feedback",
                    "当前没有等待续录的已保存片段").apply();
            return;
        }
        branchAfterWaitingCancellation = true;
        preferences.edit().putBoolean("record_waiting_retry", false)
                .putString("operation_kind", "branch_switching").commit();
        requestCancellation(false);
        setState("BRANCH_SWITCHING",
                "正在结束旧 Retry 等待并切换到已保存前缀回放；请保持新局倒计时暂停");
        updateNotification("正在切换到断点回放与续录", true);
    }

    private void requestReplayInterruption() {
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        String state = preferences.getString("state", "");
        if (!operationActive.get() ||
                !"branch".equals(preferences.getString("operation_kind", "")) ||
                !("BRANCH_REPLAYING".equals(state) ||
                  "BRANCH_INTERRUPTING".equals(state))) {
            setState("INTERRUPT_UNAVAILABLE",
                    "当前没有正在加载的存档前缀；仅可在前缀回放途中暂停后使用");
            return;
        }
        replayInterruptRequested.set(true);
        requestCancellation(false);
        setState("BRANCH_INTERRUPTING",
                "正在停止前缀加载 · 将以最后一个完整 Tick 作为分支点并武装续录");
        updateNotification("正在把当前回放位置转换为续录断点", true);
    }

    static boolean isOverlayVisible() {
        TasForegroundService service = liveInstance;
        return service != null && service.overlayController != null &&
                service.overlayController.isShowing();
    }

    static boolean isLive() { return liveInstance != null; }

    private static String kindLabel(String kind) {
        if ("record".equals(kind)) return "Recording";
        if ("replay".equals(kind)) return "Replay";
        if ("branch".equals(kind)) return "Branch recording";
        if ("prepare".equals(kind)) return "Preparing";
        if ("install".equals(kind)) return "Installing session";
        return "A9 TAS";
    }

    private static String initialOperationLabel(String kind) {
        if ("record".equals(kind)) return "正在准备录制";
        if ("replay".equals(kind)) return "正在准备回放";
        if ("branch".equals(kind)) return "正在准备分段续录";
        if ("prepare".equals(kind)) return "正在准备游戏";
        if ("install".equals(kind)) return "正在安装比赛会话";
        return kindLabel(kind);
    }

    private void prepare(int pid, long startTicks, String processName, String packageName,
                         String nativeSha, String hostMachine, boolean nativeBridge,
                         String bridgeSet, String libcSha, String profileId,
                         String runtimeBackendId, boolean experimental) {
        try {
            BuildProfileRegistry profiles = BuildProfileRegistry.load(this);
            BuildProfileRegistry.Profile profile = profiles.resolve(
                    nativeSha, profileId, experimental);
            if (profile == null)
                throw new IllegalArgumentException("BuildProfile identity changed");
            ArtifactRegistry runtimes = ArtifactRegistry.loadAndVerify(this);
            // A known game build and an unclassified host runtime are independent.
            // Keep the exact Profile selected above while still allowing the user-
            // requested runtime compatibility backend on an otherwise unknown host.
            ArtifactRegistry.Backend backend = experimental ?
                    runtimes.findAnyById(runtimeBackendId) :
                    runtimes.find(hostMachine, bridgeSet, libcSha);
            if (backend == null || !backend.id.equals(runtimeBackendId) ||
                    !backend.hostMachine.equals(hostMachine) ||
                    !backend.bridgeSet.equals(bridgeSet))
                throw new IllegalArgumentException("runtime backend identity changed");
            GameProcessScanner.Candidate selected = new GameProcessScanner.Candidate(pid,
                    startTicks, processName, packageName, "", nativeSha, hostMachine,
                    nativeBridge, bridgeSet, libcSha, profile, backend, false);
            ArtifactDeployer.Receipt receipt = ArtifactDeployer.prepare(
                    this, selected, profile, experimental);
            getSharedPreferences("session", MODE_PRIVATE).edit()
                    .putInt("prepared_pid", receipt.pid)
                    .putLong("prepared_start_ticks", receipt.startTicks)
                    .putString("prepared_profile_id", profile.id)
                    .putString("prepared_process", processName)
                    .putString("prepared_package", packageName)
                    .putString("prepared_native_sha", nativeSha)
                    .putString("prepared_host_machine", hostMachine)
                    .putBoolean("prepared_native_bridge", nativeBridge)
                    .putString("prepared_bridge_set", bridgeSet)
                    .putString("prepared_runtime_backend", backend.id)
                    .putString("prepared_libc_sha", libcSha)
                    .putBoolean("prepared_experimental_bypass", experimental)
                     .putBoolean("prepared_ready", true)
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
                     .putBoolean("game_process_hard_paused", false)
                    .remove("branch_pending_archive")
                    .remove("branch_pending_archive_sha")
                    .remove("branch_pending_target_tick")
                    .remove("branch_pending_source_sha")
                    .remove("branch_pending_pid")
                    .remove("branch_pending_start_ticks")
                    .remove("branch_pending_continuous")
                    .remove("hard_paused_pid").remove("hard_paused_start_ticks").apply();
            setState("PREPARED", (experimental ? "EXPERIMENTAL RUNTIME · " : "") +
                    "Fresh PID " + receipt.pid + " is preloaded; enter a race to continue.");
            updateNotification("Prepared PID " + receipt.pid + " · " + profile.label);
            // Preparing the game is the user's explicit entry into a TAS
            // session.  Present the lightweight bubble immediately so the
            // next record/replay action does not require another trip back to
            // the Activity.  WindowManager views must be created on the main
            // thread; a missing overlay permission does not invalidate the
            // successfully prepared game process.
            new android.os.Handler(getMainLooper()).post(() -> {
                if (overlayController != null && !overlayController.show()) {
                    getSharedPreferences("session", MODE_PRIVATE).edit()
                            .putString("overlay_feedback",
                                    "游戏已准备；授予悬浮窗权限后可直接显示快捷控制")
                            .apply();
                }
            });
        } catch (Exception error) {
            // Preparation can fail either before touching the old process or
            // after ArtifactDeployer has already force-stopped it.  Preserve
            // a genuinely live resident session, but clear process-bound
            // metadata when its exact PID/start-time identity is now gone.
            // This prevents both destructive pre-clearing and a stale
            // PREPARED/installed state after a mid-transaction failure.
            try {
                SessionOrchestrator.reconcileTerminatedRuntimeState(this);
            } catch (Exception ignored) {}
            DiagnosticBundle.recordFailure(this, "PREPARE", error);
            setState("RETRY_READY", error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage());
            updateNotification("Prepare failed · open A9 TAS for details");
        }
    }

    private void installSession() {
        synchronized (operationLock) {
            try {
                if (!getSharedPreferences("session", MODE_PRIVATE)
                        .getBoolean("prepared_ready", false))
                    throw new IllegalStateException("请先准备当前游戏进程");
                setState("INSTALLING_SESSION",
                        "Installing proven pass-through hooks and locating race owner…");
                SessionOrchestrator.InstallReceipt receipt = SessionOrchestrator.installPassive(this);
                setState("SESSION_INSTALLED", "PID " + receipt.pid + " · race owner 0x" +
                        Long.toHexString(receipt.owner) + " · " + receipt.samples +
                        " stable read-only samples");
                updateNotification("Session installed · PID " + receipt.pid);
            } catch (Exception error) {
                DiagnosticBundle.recordFailure(this, "INSTALL_SESSION", error);
                setState(requiresExplicitRecovery(error) ? "RECOVERY_REQUIRED" : "RETRY_READY",
                        error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage());
                updateNotification("Session install failed · unchanged, restored, or stopped if uncertain");
            }
        }
    }

    private void requestCancellation(boolean stopAfter) {
        if (!operationActive.get()) {
            if (stopAfter) startRestore(true);
            else setState("IDLE", "No recording or replay is currently running");
            return;
        }
        stopAfterOperation = stopAfter;
        cancelRequested.set(true);
        File signal = activeCancellationSignal;
        if (signal != null) {
            try {
                publishCancellationSignal(signal);
            } catch (IOException error) {
                setState("CANCEL_SIGNAL_FAILED",
                        "Cancellation requested, but the immediate signal failed; restoring at the next boundary");
            }
        }
        getSharedPreferences("session", MODE_PRIVATE).edit()
                .putBoolean("cancel_pending", true).apply();
        setState("CANCEL_REQUESTED",
                "Cancellation requested · restoring at the next verified progress boundary");
        updateNotification("Cancelling · waiting for a verified restore boundary", true);
    }

    private static void publishCancellationSignal(File signal) throws IOException {
        if (!signal.createNewFile() && !signal.isFile())
            throw new IOException("unable to publish cancellation signal");
    }

    private void startRestore(boolean stopService) {
        setState("RESTORING", "Restoring the original game hooks…");
        new Thread(() -> restoreSession(stopService), "a9tas-restore").start();
    }

    private void restoreSession(boolean stopService) {
        synchronized (operationLock) {
            try {
                android.content.SharedPreferences preferences =
                        getSharedPreferences("session", MODE_PRIVATE);
                if (preferences.getBoolean("game_process_hard_paused", false))
                    SessionOrchestrator.resumeHardStoppedAtPauseMenu(this);
                String result = SessionOrchestrator.restoreIfNeeded(this);
                // RESTORE is an explicit abandonment of any in-flight branch
                // handoff.  The immutable recording remains in the library,
                // while stale same-process metadata must not block preparing a
                // fresh PID after an APK update or process restart.
                clearPendingBranch(preferences);
                preferences.edit()
                        .putBoolean("resident_retry_record_armed", false)
                        .putBoolean("resident_retry_auto_loop", false)
                        .putBoolean("resident_pending_queued", false)
                        .remove("resident_pending_frame_limit")
                        .putBoolean("record_waiting_retry", false)
                        .putBoolean("cancel_pending", false).apply();
                boolean remainsPrepared = preferences.getBoolean("prepared_ready", false);
                setState(stopService ? "STOPPED" : "RESTORED", result +
                        (stopService ? "" : remainsPrepared ?
                                " · 当前游戏进程仍已准备" : " · 请重新扫描并准备当前游戏"));
                updateNotification(stopService ? "Session stopped" : "Original game state restored", false);
            } catch (Exception error) {
                DiagnosticBundle.recordFailure(this, "RESTORE", error);
                setState("RECOVERY_REQUIRED", error.getMessage() == null ?
                        error.getClass().getSimpleName() : error.getMessage());
            } finally {
                if (stopService) {
                    RootShell.closePersistent();
                    stopForeground(true);
                    stopSelf();
                }
            }
        }
    }

    private void ensureSessionInstalled() throws Exception {
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        if (preferences.getBoolean("session_hooks_installed", false)) return;
        if (!preferences.getBoolean("prepared_ready", false))
            throw new IllegalStateException("请先扫描并准备所选游戏，再开始连续刷圈");
        setState("INSTALLING_SESSION", "Locating the paused race and installing the proven session runtime…");
        SessionOrchestrator.InstallReceipt receipt = SessionOrchestrator.installPassive(this);
        setState("SESSION_INSTALLED", "PID " + receipt.pid + " · race owner 0x" +
                Long.toHexString(receipt.owner) + " · ready");
    }

    private void ensureReplaySessionInstalled() throws Exception {
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        // A completed brush attempt deliberately leaves the proven runtime in
        // its archived state.  The controller owns an exact rearm-replay
        // transition for that state; restoring and locating/installing a new
        // runtime here both wastes the Retry countdown and loses the stable
        // lifecycle ownership that the completed attempt already proved.
        if (preferences.getBoolean("session_hooks_installed", false) &&
                preferences.getBoolean("brush_archived_record_ready", false)) {
            return;
        }
        ensureSessionInstalled();
    }

    private void cancelBeforeArmIfRequested() throws Exception {
        if (!cancelRequested.get()) return;
        // No session mutation has happened yet.  Upstream keeps its hooks
        // resident when an operation is cancelled between runs, so cancelling
        // here must not turn into an uninstall/reinstall cycle.
        throw new SessionOrchestrator.OperationCancelledException();
    }

    private void recordLifecycle(boolean autoInstall) {
        recordLifecycle(autoInstall, false);
    }

    private void recordLifecycle(boolean autoInstall, boolean gameAlreadyForeground) {
        boolean waitingForRetry = false;
        try {
            if (autoInstall) ensureSessionInstalled();
            cancelBeforeArmIfRequested();
            android.content.SharedPreferences preferences =
                    getSharedPreferences("session", MODE_PRIVATE);
            boolean brushSession = preferences.getBoolean("brush_session_enabled", true);
            boolean automaticRetry = brushSession &&
                    preferences.getBoolean("auto_retry_record", true);
            boolean resumePausedRace = true;
            SessionOrchestrator.OperationObserver recordObserver = observer("record");
            while (true) {
                preferences.edit().putBoolean("record_waiting_retry", false)
                        .putString("operation_kind", "record").apply();
                waitingForRetry = false;
                setState("RECORD_ARMING", resumePausedRace ?
                        "正在安装并武装录制 · 完成后将返回游戏" :
                        "Retry 已识别 · 正在为下一局武装 Tick 0");
                SessionOrchestrator.RecordReceipt receipt =
                        SessionOrchestrator.recordLifecycle(
                                this, recordObserver, brushSession, resumePausedRace,
                                gameAlreadyForeground);
                if (receipt.retryDiscarded) {
                    if (automaticRetry) {
                        if (preferences.getBoolean("continuous_load_selected", false)) {
                            setState("RETRY_INTERCEPTING",
                                    "已丢弃未保留片段 · 正在拦停 Retry 新局倒计时");
                            updateNotification("Retry 已识别 · 正在自动暂停新局", true);
                            SessionOrchestrator.waitForRetryCountdown(
                                    this, recordObserver);
                            SessionOrchestrator.pauseRetryCountdown(this);
                            scheduleConfiguredNextAttempt(preferences,
                                    "Retry 已识别 · 正在加载所选前缀");
                            return;
                        }
                        setState("RETRY_REARMING",
                                "已丢弃未保留片段 · 等待常驻生命周期入口确认下一局");
                        updateNotification("Retry 已识别 · 正在无扫描切换下一局", true);
                        waitingForRetry = true;
                        preferences.edit().putBoolean("record_waiting_retry", true)
                                .putString("operation_kind", "record_waiting").apply();
                        SessionOrchestrator.waitForDirectRetryRecord(
                                this, recordObserver);
                        waitingForRetry = false;
                        resumePausedRace = false;
                        continue;
                    }
                    setState("ATTEMPT_DISCARDED",
                            "检测到直接 Retry · 本次片段未保存 · 自动连录已关闭");
                    updateNotificationReadyForNextAttempt("本次已丢弃");
                    return;
                }
                if (receipt.checkpoint) {
                    archiveCheckpoint(receipt);
                    setState("CHECKPOINT_SAVED",
                            "暂停片段已直接保存 · " + receipt.ticks +
                                    " Tick · 可在录像库反复修改回放长度");
                    updateNotification("已保存 " + receipt.ticks +
                            " Tick · Retry 后将自动录制下一局", true);
                } else {
                    String detail = receipt.ticks + " ticks · " + receipt.intervalCount +
                            " intervals · " + (receipt.archive == null ?
                            receipt.file.getName() + " · A9TAS packaging failed: " + receipt.archiveError :
                            receipt.archive.getName() + " · A9TAS1 verified") +
                            (brushSession ? " · 刷圈会话已保留" : "");
                    setState("RECORDING_SAVED", detail);
                    updateNotification("录像已保存 · " + receipt.ticks +
                            " Tick · 等待 Retry", true);
                }
                if (!automaticRetry) return;
                waitingForRetry = true;
                preferences.edit().putBoolean("record_waiting_retry", true)
                        .putString("operation_kind", "record_waiting").apply();
                if (preferences.getBoolean("continuous_load_selected", false)) {
                    setState("RETRY_QUEUED",
                            "所选前缀已开始常驻排队 · 回到游戏直接 Retry");
                    updateNotification("前缀已排队 · 直接 Retry", true);
                    SessionOrchestrator.ReplayReceipt replay =
                            SessionOrchestrator.queueNextRacePrefix(
                                    this, recordObserver);
                    waitingForRetry = false;
                    publishQueuedBranchAndContinue(preferences, replay, true);
                    return;
                }
                // recordLifecycle keeps a direct-Retry generation queued inside
                // the resident payload while the current attempt is live.  Do
                // not issue queue-record again after a finish: the native loop
                // already owns the next generation and will activate it at the
                // next lifecycle entry.  Re-queuing here races that activation
                // and is rejected with archive_precondition.
                if (preferences.getBoolean("resident_retry_auto_loop", false)) {
                    setState("WAITING_RETRY",
                            "当前片段已处理 · 下一局已常驻武装，回到游戏直接 Retry");
                    updateNotification("下一局已预排 · 等待 Retry", true);
                    SessionOrchestrator.waitForDirectRetryRecord(
                            this, recordObserver);
                    waitingForRetry = false;
                    resumePausedRace = false;
                    continue;
                }
                setState("WAITING_RETRY",
                        "当前片段已保存 · 可修改下一局设置；回到游戏直接 Retry 后自动执行");
                setState("RETRY_QUEUED",
                        "下一局录制已常驻排队 · 回到游戏直接 Retry，无需倒计时暂停");
                updateNotification("下一局已排队 · 直接 Retry", true);
                SessionOrchestrator.queueNextRaceRecord(this, recordObserver);
                waitingForRetry = false;
                preferences.edit().putBoolean("record_waiting_retry", false)
                        .putString("operation_kind", "record").apply();
                setState("RETRY_REARMING",
                        "新比赛已在生命周期入口自动武装 · 从 Tick 0 开始录制");
                resumePausedRace = false;
            }
        } catch (SessionOrchestrator.RecordingPausedException paused) {
            setState("ATTEMPT_DISCARDED", "比赛暂停但封存未完成 · 本次片段未保存");
            updateNotificationReadyForNextAttempt("本次未保存");
        } catch (SessionOrchestrator.OperationCancelledException cancelled) {
            if (waitingForRetry && branchAfterWaitingCancellation) {
                branchAfterWaitingCancellation = false;
                branchAfterCleanWaitingCancellation = true;
                setState("BRANCH_SWITCHING",
                        "旧 Retry 等待已结束 · 正在启动已保存前缀回放");
                updateNotification("正在启动断点回放与续录", true);
            } else if (waitingForRetry) {
                boolean activeAttempt = getSharedPreferences("session", MODE_PRIVATE)
                        .getBoolean("resident_retry_record_armed", false);
                setState("CONTINUOUS_RECORD_STOPPED", activeAttempt
                        ? "已结束自动循环 · Retry 已抢先进入新局，当前局录制仍保留，可再次点录制接管"
                        : "已结束连续刷圈 · 已保存录像不变 · 下一局排队已取消");
                updateNotificationReadyForNextAttempt(activeAttempt
                        ? "自动循环已结束 · 当前局仍在录制"
                        : "连续刷圈已结束 · 排队已取消");
            } else {
                setState("CANCELLED_RESTORED",
                        "Recording cancelled · original hooks restored · this process remains prepared");
                updateNotificationReadyForNextAttempt("录制已中断");
            }
        } catch (Exception error) {
            reportFailure("录制", error);
        } finally {
            getSharedPreferences("session", MODE_PRIVATE).edit()
                    .putBoolean("record_waiting_retry", false).apply();
        }
    }

    private void publishQueuedBranchAndContinue(
            android.content.SharedPreferences preferences,
            SessionOrchestrator.ReplayReceipt replay,
            boolean continuous) throws Exception {
        if (!replay.pausedAtTarget || !replay.continuationPrearmed)
            throw new IOException("resident prefix did not produce an armed breakpoint");
        String archivePath = preferences.getString("last_replay_archive", "");
        String archiveSha = preferences.getString("last_replay_archive_sha", "");
        String sourceSha = preferences.getString("last_replay_recording_sha", "");
        long target = preferences.getLong("last_replay_target_tick", -1L);
        A9TasLibrary.Entry base = A9TasLibrary.verified(
                this, archivePath, archiveSha);
        if (target < 0 || target >= base.summary.frameCount || sourceSha.isEmpty())
            throw new IOException("resident prefix receipt is incomplete");
        int pid = preferences.getInt("prepared_pid", 0);
        long startTicks = preferences.getLong("prepared_start_ticks", 0L);
        if (pid <= 0 || startTicks <= 0)
            throw new IOException("prepared process identity disappeared at branch point");
        boolean pendingSaved = preferences.edit()
                .putBoolean("branch_pending", true)
                .putString("branch_pending_archive", base.file.getAbsolutePath())
                .putString("branch_pending_archive_sha", base.archiveSha256)
                .putLong("branch_pending_target_tick", target)
                .putString("branch_pending_source_sha", sourceSha)
                .putInt("branch_pending_pid", pid)
                .putLong("branch_pending_start_ticks", startTicks)
                .putBoolean("branch_pending_continuous", continuous)
                .commit();
        if (!pendingSaved)
            throw new IOException("unable to persist resident branch point");

        setState("BRANCH_PAUSED", "Tick " + target +
                " reached through resident Retry replay · continuation armed");
        updateNotification("已到断点 T" + target + " · 续录已武装", true);
        // Leave the current operation before entering suffix recording.  This
        // keeps a long brush-lap session iterative instead of recursively
        // nesting one Java call frame per accepted branch.
        branchAfterCleanWaitingCancellation = true;
    }

    private A9TasLibrary.Entry archiveCheckpoint(
            SessionOrchestrator.RecordReceipt receipt) throws Exception {
        if (!receipt.checkpoint || receipt.file == null || receipt.ticks < 1)
            throw new IOException("checkpoint receipt is incomplete");
        A9TasLibrary.Entry archived = A9TasLibrary.promoteDraft(
                this, receipt.file, receipt.sha256, receipt.ticks,
                A9TasLibrary.metadataForLatestRaw(this));
        boolean published = getSharedPreferences("session", MODE_PRIVATE).edit()
                .remove("attempt_draft_path").remove("attempt_draft_sha")
                .remove("attempt_draft_ticks")
                .putString("latest_archive", archived.file.getAbsolutePath())
                .putString("latest_archive_sha", archived.archiveSha256)
                .putString("latest_archive_id",
                        archived.summary.manifest.getString("recording_id"))
                .putString("selected_archive", archived.file.getAbsolutePath())
                .putString("selected_archive_sha", archived.archiveSha256)
                .putString("selected_archive_title", archived.title())
                .putLong("replay_target_tick", receipt.ticks - 1L)
                .putString("replay_target_archive_sha", archived.archiveSha256)
                .commit();
        if (!published) throw new IOException("checkpoint archive state could not be persisted");
        return archived;
    }

    /** Selects the next iteration only after the current runtime has reached a
     * clean Retry boundary. UI changes made while paused therefore affect the
     * next lap without mutating the archive/target snapshot used by this lap. */
    private void scheduleConfiguredNextAttempt(
            android.content.SharedPreferences preferences, String detail) {
        boolean loadPrefix = preferences.getBoolean("continuous_load_selected", false);
        branchAfterCleanWaitingCancellation = loadPrefix;
        freshRecordAfterCleanOperation = !loadPrefix;
        preferences.edit().putBoolean("record_waiting_retry", false)
                .putString("operation_kind", "cycle_switching").apply();
        setState(loadPrefix ? "CONTINUOUS_PREFIX_LOADING" : "RETRY_REARMING", detail);
        updateNotification(loadPrefix ? "正在加载所选存档前缀" : "正在从 Tick 0 录制", true);
    }

    private void replaySelected(boolean autoInstall) {
        replaySelected(autoInstall, true, false);
    }

    private void replaySelected(boolean autoInstall, boolean allowCleanRepair) {
        replaySelected(autoInstall, allowCleanRepair, false);
    }

    private void replaySelected(boolean autoInstall, boolean allowCleanRepair,
                                boolean gameAlreadyForeground) {
        try {
                if (autoInstall) ensureReplaySessionInstalled();
                cancelBeforeArmIfRequested();
                setState("REPLAYING", "Replay armed · the APK will resume the paused race once");
                SessionOrchestrator.ReplayReceipt receipt =
            SessionOrchestrator.replaySelected(this, observer("replay"), false,
                    gameAlreadyForeground);
                setState("REPLAY_COMPLETE", receipt.ticks + " ticks · " +
                        receipt.intervalCount + " source intervals · " + receipt.nitroCalls +
                        " Nitro · " + receipt.barrelFrames + " Barrel frames · fixed-delta " +
                        String.format(java.util.Locale.ROOT, "%.3f ms", receipt.fixedDeltaUs / 1000.0) +
                        " · source clock verified");
                if (receipt.pausedAtTarget)
                    setState("REPLAY_PAUSED_AT_TARGET", receipt.ticks + " ticks · " +
                            "target reached · game paused · source fixed-delta " +
                            String.format(java.util.Locale.ROOT, "%.3f ms", receipt.fixedDeltaUs / 1000.0) +
                            " · session runtime retained");
                updateNotification(receipt.pausedAtTarget ?
                        "Replay paused at target · " + receipt.ticks + " ticks" :
                        "Replay complete · " + receipt.ticks + " ticks", false);
            } catch (SessionOrchestrator.OperationCancelledException cancelled) {
                setState("RETRY_READY",
                        "Replay cancelled · resident session retained · ready for another operation");
                updateNotification("Replay cancelled · session remains ready", false);
            } catch (Exception error) {
                DiagnosticBundle.recordFailure(this, "REPLAY", error);
                setState("RETRY_READY", error.getMessage() == null ?
                        error.getClass().getSimpleName() : error.getMessage());
                updateNotification("本次回放未执行 · 修正状态后可直接重试", false);
        }
    }

    private void branchRecord(boolean continuous) {
        branchRecord(continuous, true, false);
    }

    private void branchRecord(boolean continuous, boolean allowCleanRepair) {
        branchRecord(continuous, allowCleanRepair, false);
    }

    private void branchRecord(boolean continuous, boolean allowCleanRepair,
                              boolean gameAlreadyForeground) {
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        if (preferences.getBoolean("branch_pending", false)) {
            if (preferences.getBoolean("game_process_hard_paused", false)) {
                resumeHardPausedBranch();
                return;
            }
            boolean automatic = preferences.getBoolean("branch_handoff_mode_v2", false) &&
                    preferences.getBoolean("branch_auto_handoff", false);
            int delaySeconds = automatic ?
                    preferences.getInt("branch_resume_delay_seconds", 5) : 0;
            continuePendingBranch(preferences, delaySeconds, false, automatic, continuous);
            return;
        }
        boolean previousPause = preferences.getBoolean("replay_pause_at_target", false);
        A9TasLibrary.Entry base = null;
        try {
            base = A9TasLibrary.selected(this);
            long target = preferences.getLong("replay_target_tick", base.summary.targetTick);
            String binding = preferences.getString("replay_target_archive_sha", "");
            if (!base.archiveSha256.equals(binding) || target < 0 ||
                    target >= base.summary.frameCount)
                throw new IllegalArgumentException(
                        "Select a valid branch point in the base recording");
            preferences.edit().putBoolean("replay_pause_at_target", true).commit();

            ensureReplaySessionInstalled();
            cancelBeforeArmIfRequested();
            setState("BRANCH_REPLAYING", "Replaying immutable prefix to Tick " + target + "…");
            SessionOrchestrator.ReplayReceipt replay =
                    SessionOrchestrator.replaySelected(this, observer("branch"), true,
                            gameAlreadyForeground, true);
            if (!replay.pausedAtTarget)
                throw new IllegalStateException("branch prefix did not pause at its target");
            if (!replay.continuationPrearmed)
                throw new IllegalStateException(
                        "branch prefix completed without an armed continuation runtime");
            int pid = preferences.getInt("prepared_pid", 0);
            long startTicks = preferences.getLong("prepared_start_ticks", 0L);
            if (pid <= 0 || startTicks <= 0)
                throw new IOException("prepared process identity disappeared at branch point");
            boolean pendingSaved = preferences.edit()
                    .putBoolean("branch_pending", true)
                    .putString("branch_pending_archive", base.file.getAbsolutePath())
                    .putString("branch_pending_archive_sha", base.archiveSha256)
                    .putLong("branch_pending_target_tick", target)
                    .putString("branch_pending_source_sha", replay.recordingSha256)
                    .putInt("branch_pending_pid", pid)
                    .putLong("branch_pending_start_ticks", startTicks)
                    .putBoolean("branch_pending_continuous", continuous)
                    .commit();
            if (!pendingSaved)
                throw new IOException("unable to persist the paused branch point");

            if (replay.hardStoppedAtTarget) {
                boolean frozenPublished = preferences.edit()
                        .putBoolean("game_process_hard_paused", true)
                        .putInt("hard_paused_pid", pid)
                        .putLong("hard_paused_start_ticks", startTicks).commit();
                if (!frozenPublished)
                    throw new IOException("unable to persist the hard-paused process identity");
                setState("BRANCH_HARD_PAUSED", "Tick " + target +
                        " 已精确冻结 · 点击“恢复并准备续录”后仍由你手动继续比赛");
                updateNotification("断点已精确冻结 · 回到悬浮窗继续", false);
                return;
            }

            boolean automatic = preferences.getBoolean("branch_handoff_mode_v2", false) &&
                    preferences.getBoolean("branch_auto_handoff", false);
            int delaySeconds = preferences.getInt("branch_resume_delay_seconds", 5);
            if (automatic && (delaySeconds < 0 || delaySeconds > 600))
                throw new IOException("stored branch resume delay is invalid");
            setState("BRANCH_PAUSED", "Tick " + target +
                    " reached · game paused · arming continuation");
            updateNotification("Branch paused at T" + target +
                    " · arming continuation", true);
            continuePendingBranch(preferences, automatic ? delaySeconds : 0,
                    true, automatic, continuous);
        } catch (SessionOrchestrator.ReplayInterruptedException interrupted) {
            boolean acceptedAutomaticPause = interrupted.automaticPause;
            boolean acceptedManualPause = replayInterruptRequested.getAndSet(false);
            if ((!acceptedAutomaticPause && !acceptedManualPause) || base == null ||
                    interrupted.ticks <= 0 ||
                    interrupted.ticks > base.summary.frameCount) {
                boolean retained = reconcileBranchFailure(preferences);
                setState("RETRY_READY", retained ?
                        "回放中断回执无效 · 原运行时仍暂停保留" :
                        "回放中断回执无效 · 已清理过期断点");
                return;
            }
            long target = interrupted.ticks - 1L;
            int pid = preferences.getInt("prepared_pid", 0);
            long startTicks = preferences.getLong("prepared_start_ticks", 0L);
            boolean pendingSaved = preferences.edit()
                    .putBoolean("branch_pending", true)
                    .putString("branch_pending_archive", base.file.getAbsolutePath())
                    .putString("branch_pending_archive_sha", base.archiveSha256)
                    .putLong("branch_pending_target_tick", target)
                    .putString("branch_pending_source_sha",
                            interrupted.recordingSha256)
                    .putInt("branch_pending_pid", pid)
                    .putLong("branch_pending_start_ticks", startTicks)
                    .putBoolean("branch_pending_continuous", continuous)
                    .commit();
            if (!pendingSaved) {
                setState("RETRY_READY", "无法保存回放中断点；运行时仍暂停，可直接重试");
                return;
            }
            setState("BRANCH_PAUSED", "已在 Tick " + target +
                    (acceptedAutomaticPause ?
                            " 提前暂停并舍弃计划点之后的尾部 · 正在武装续录" :
                            " 停止加载 · 正在武装从下一 Tick 开始的续录"));
            updateNotification("前缀已截断到 T" + target + " · 正在武装续录", true);
            cancelRequested.set(false);
            preferences.edit().putBoolean("cancel_pending", false).apply();
            // The user already paused the race.  Preserve that pause and let
            // them resume manually after the suffix recorder is armed.
            continuePendingBranch(preferences, 0, true, false, continuous);
        } catch (SessionOrchestrator.OperationCancelledException cancelled) {
            boolean retained = reconcileBranchFailure(preferences);
            setState("RETRY_READY",
                    retained ?
                            "Branch cancelled · paused breakpoint and immutable base were preserved" :
                            "Branch cancelled · stale breakpoint cleared · immutable base preserved");
            updateNotification(retained ?
                    "Branch cancelled · breakpoint preserved" :
                    "Branch cancelled · base preserved", false);
        } catch (Exception error) {
            DiagnosticBundle.recordFailure(this, "BRANCH_RECORD", error);
            boolean retained = reconcileBranchFailure(preferences);
            setState("RETRY_READY", error.getMessage() == null ?
                    error.getClass().getSimpleName() : error.getMessage() +
                    (retained ? " · paused breakpoint preserved" :
                            " · stale breakpoint cleared; base preserved"));
            updateNotification(retained ?
                    "Branch failed · breakpoint and base preserved" :
                    "Branch failed · base recording preserved", false);
        } finally {
            preferences.edit().putBoolean("replay_pause_at_target", previousPause).apply();
        }
    }

    private void resumeHardPausedBranch() {
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        try {
            if (!preferences.getBoolean("branch_pending", false))
                throw new IOException("没有可恢复的断点续录任务");
            boolean continuous = preferences.getBoolean("branch_pending_continuous", false);
            setState("BRANCH_RELEASING_HARD_PAUSE",
                    "正在验证冻结进程并投递一次暂停键…");
            SessionOrchestrator.resumeHardStoppedAtPauseMenu(this);
            setState("BRANCH_ARMING", "游戏已停在暂停菜单 · 正在武装后段录制…");
            continuePendingBranch(preferences, 0, true, false, continuous);
        } catch (Exception error) {
            DiagnosticBundle.recordFailure(this, "BRANCH_HARD_RESUME", error);
            setState("RECOVERY_REQUIRED", error.getMessage() == null ?
                    error.getClass().getSimpleName() : error.getMessage());
            updateNotification("恢复冻结断点失败 · 请打开 A9 TAS 查看", false);
        }
    }

    private void continuePendingBranch(android.content.SharedPreferences preferences,
                                       int armedPauseSeconds,
                                       boolean gameAlreadyForeground,
                                       boolean automaticResume,
                                       boolean continuous) {
        try {
            A9TasLibrary.Entry base = pendingBranchBase(preferences);
            long target = preferences.getLong("branch_pending_target_tick", -1L);
            int expectedPid = preferences.getInt("branch_pending_pid", 0);
            long expectedStart = preferences.getLong("branch_pending_start_ticks", 0L);
            if (!preferences.getBoolean("prepared_ready", false) ||
                    preferences.getInt("prepared_pid", 0) != expectedPid ||
                    preferences.getLong("prepared_start_ticks", 0L) != expectedStart)
                throw new IOException("branch point belongs to a different or terminated game process");

            cancelBeforeArmIfRequested();
            if (!preferences.getBoolean("branch_replay_ready", false) ||
                    !preferences.getBoolean("session_hooks_installed", false))
                throw new IOException("breakpoint runtime is no longer retained; replay the prefix again");
            cancelBeforeArmIfRequested();
            setState("BRANCH_ARMING", "断点运行时仍在原进程 · 正在武装后段录制…");
            SessionOrchestrator.RecordReceipt suffix =
                    SessionOrchestrator.recordFromPausedReplay(this, observer("branch"),
                            armedPauseSeconds, gameAlreadyForeground, automaticResume,
                            continuous && preferences.getBoolean("brush_session_enabled", true));
            if (suffix.retryDiscarded) {
                clearPendingBranch(preferences);
                preferences.edit().putBoolean("branch_replay_ready", false).apply();
                if (continuous && preferences.getBoolean("auto_retry_record", true)) {
                    // Keep the proven hooks resident, but bind only after Retry
                    // has created the new race object.  The native waiter owns
                    // the timing-critical pause; Java then has unlimited time
                    // to load/rearm the selected prefix while countdown 3 is
                    // held.
                    setState("RETRY_INTERCEPTING",
                            "新增后段已丢弃 · 正在拦停 Retry 新局倒计时");
                    updateNotification("Retry 已识别 · 正在自动暂停新局", true);
                    SessionOrchestrator.waitForRetryCountdown(this, observer("branch"));
                    SessionOrchestrator.pauseRetryCountdown(this);
                    scheduleConfiguredNextAttempt(preferences,
                            "Retry 已识别 · 新增后段已丢弃 · 正在执行下一局设置");
                    return;
                }
                setState("ATTEMPT_DISCARDED",
                        "检测到直接 Retry · 新增后段已丢弃 · 原主分支未改变");
                updateNotification("后段已自动丢弃 · 原主分支保留", false);
                return;
            }
            A9TasLibrary.Entry branch = A9TasBranchEditor.adoptContinuous(
                    this, base, target, suffix.file, suffix.sha256);
            long prefixTicks = target + 1L;
            long newlyRecordedTicks = suffix.ticks - prefixTicks;
            preferences.edit()
                    .putString("latest_archive", branch.file.getAbsolutePath())
                    .putString("latest_archive_sha", branch.archiveSha256)
                    .putString("latest_archive_id",
                            branch.summary.manifest.getString("recording_id"))
                    // The accepted branch is the next iteration's main line.
                    // Keep the old archive immutable in the library, but make
                    // the new archive the active selection and reset its
                    // default replay/trim cursor to the new final tick.
                    .putString("selected_archive", branch.file.getAbsolutePath())
                    .putString("selected_archive_sha", branch.archiveSha256)
                    .putString("selected_archive_title", branch.title())
                    .putLong("replay_target_tick", branch.summary.targetTick)
                    .putString("replay_target_archive_sha", branch.archiveSha256)
                    .remove("latest_recording")
                    .remove("latest_recording_sha")
                    .remove("latest_recording_ticks")
                    // A paused suffix is first materialized as an attempt
                    // draft.  Once its verified bytes have been spliced, that
                    // draft no longer exists and must not remain visible in UI.
                    .remove("attempt_draft_path")
                    .remove("attempt_draft_sha")
                    .remove("attempt_draft_ticks")
                    .apply();
            if (suffix.archive != null && suffix.archive.isFile()) {
                try {
                    A9TasLibrary.Entry suffixEntry = new A9TasLibrary.Entry(suffix.archive,
                            A9TasArchive.inspect(suffix.archive), suffix.archiveSha256);
                    A9TasLibrary.delete(this, suffixEntry);
                } catch (Exception ignored) {}
            }
            if (suffix.file.isFile()) suffix.file.delete();
            clearPendingBranch(preferences);
            preferences.edit().putBoolean("branch_replay_ready", false)
                    .putBoolean("branch_runtime_prearmed", false).apply();
            setState(suffix.checkpoint ? "BRANCH_CHECKPOINT_SAVED" : "BRANCH_SAVED",
                     branch.summary.frameCount + " ticks · base through T" +
                    target + " + " + newlyRecordedTicks +
                    " newly recorded ticks · pause interval omitted · A9TAS1 verified" +
                    (suffix.checkpoint ? " · 可继续选择更早的保留 Tick" : ""));
            updateNotification("Branch saved · " + branch.summary.frameCount + " ticks", false);
            if (continuous && preferences.getBoolean("auto_retry_record", true)) {
                preferences.edit().putBoolean("record_waiting_retry", true)
                        .putString("operation_kind", "record_waiting").apply();
                SessionOrchestrator.OperationObserver nextObserver = observer("branch");
                if (preferences.getBoolean("continuous_load_selected", false)) {
                    setState("RETRY_QUEUED",
                            "新主分支前缀已常驻排队 · 回到游戏直接 Retry");
                    updateNotification("新主分支已排队 · 直接 Retry", true);
                    SessionOrchestrator.ReplayReceipt replay =
                            SessionOrchestrator.queueNextRacePrefix(
                                    this, nextObserver);
                    publishQueuedBranchAndContinue(preferences, replay, true);
                } else {
                    setState("RETRY_QUEUED",
                            "下一局 Tick 0 录制已常驻排队 · 回到游戏直接 Retry");
                    updateNotification("下一局已排队 · 直接 Retry", true);
                    SessionOrchestrator.queueNextRaceRecord(this, nextObserver);
                    freshRecordAfterCleanOperation = true;
                }
            }
        } catch (SessionOrchestrator.OperationCancelledException cancelled) {
            boolean retained = reconcileBranchFailure(preferences);
            setState("RETRY_READY",
                    retained ?
                            "Branch continuation cancelled · paused branch point and base were preserved" :
                            "Branch continuation cancelled · stale branch point cleared · base preserved");
            updateNotification(retained ?
                    "Branch continuation cancelled · breakpoint preserved" :
                    "Branch continuation cancelled · base preserved", false);
        } catch (Exception error) {
            DiagnosticBundle.recordFailure(this, "BRANCH_CONTINUATION", error);
            boolean retained = reconcileBranchFailure(preferences);
            setState("RETRY_READY", error.getMessage() == null ?
                    error.getClass().getSimpleName() : error.getMessage() +
                    (retained ? " · breakpoint preserved" :
                            " · stale breakpoint cleared; base preserved"));
            updateNotification(retained ?
                    "Branch continuation failed · breakpoint and base preserved" :
                    "Branch continuation failed · base preserved", false);
        }
    }

    /**
     * A branch point is useful only while the exact replay runtime remains in
     * the same process.  Never leave a UI-visible pending branch after restore,
     * process termination, or a completed suffix.  Conversely, do not tear
     * down a still-valid paused breakpoint merely because a host-side step can
     * be retried.
     */
    private boolean reconcileBranchFailure(
            android.content.SharedPreferences preferences) {
        boolean pending = preferences.getBoolean("branch_pending", false);
        boolean retained = preferences.getBoolean("branch_replay_ready", false) &&
                preferences.getBoolean("session_hooks_installed", false);
        if (pending && !retained) {
            clearPendingBranch(preferences);
            return false;
        }
        if (!pending && retained) {
            try {
                if (preferences.getBoolean("game_process_hard_paused", false))
                    SessionOrchestrator.resumeHardStoppedAtPauseMenu(this);
                SessionOrchestrator.restoreIfNeeded(this);
            } catch (Exception ignored) {
                // restoreIfNeeded already terminates and clears runtime state
                // when restoration cannot be proven.
            }
            preferences.edit().putBoolean("branch_replay_ready", false)
                    .putBoolean("branch_runtime_prearmed", false).apply();
            return false;
        }
        return pending && retained;
    }

    private A9TasLibrary.Entry pendingBranchBase(
            android.content.SharedPreferences preferences) throws Exception {
        String path = preferences.getString("branch_pending_archive", "");
        String expectedSha = preferences.getString("branch_pending_archive_sha", "");
        File library = new File(getFilesDir(), "library").getCanonicalFile();
        File archive = new File(path).getCanonicalFile();
        if (!library.equals(archive.getParentFile()) || !archive.isFile() ||
                !archive.getName().matches("[0-9a-f-]{36}[.]a9tas") ||
                !expectedSha.matches("[0-9a-f]{64}") ||
                !expectedSha.equals(A9TasLibrary.sha256(archive)))
            throw new IOException("paused branch base recording changed");
        A9TasArchive.Summary summary = A9TasArchive.inspect(archive);
        long target = preferences.getLong("branch_pending_target_tick", -1L);
        if (target < 0 || target >= summary.frameCount)
            throw new IOException("paused branch target is invalid");
        return new A9TasLibrary.Entry(archive, summary, expectedSha);
    }

    private static void clearPendingBranch(
            android.content.SharedPreferences preferences) {
        preferences.edit().putBoolean("branch_pending", false)
                .putBoolean("branch_replay_ready", false)
                .putBoolean("branch_runtime_prearmed", false)
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

    private void setState(String state, String detail) {
        getSharedPreferences("session", MODE_PRIVATE).edit()
                .putString("state", state).putString("detail", detail)
                .putLong("updated", System.currentTimeMillis()).apply();
        DiagnosticBundle.recordState(this, state, detail);
    }

    private void reportFailure(String operation, Throwable error) {
        String detail = error.getMessage();
        if (detail == null || detail.trim().isEmpty()) detail = error.getClass().getSimpleName();
        String errorId = DiagnosticBundle.recordFailure(this, operation, error);
        String shortId = errorId.length() < 8 ? errorId : errorId.substring(0, 8);
        detail = detail + " · 错误ID " + shortId;
        setState(requiresExplicitRecovery(error) ? "RECOVERY_REQUIRED" :
                "RETRY_READY", detail);
        String singleLine = detail.replace('\n', ' ').replace('\r', ' ').trim();
        if (singleLine.length() > 96) singleLine = singleLine.substring(0, 93) + "…";
        updateNotification(operation + "失败 · " + singleLine, false);
    }

    private SharedPreferences preferences() {
        return getSharedPreferences("session", MODE_PRIVATE);
    }

    private static boolean requiresExplicitRecovery(Throwable error) {
        String message = error == null || error.getMessage() == null ? "" :
                error.getMessage().toLowerCase(java.util.Locale.ROOT);
        return message.contains("uncertain=1") ||
                message.contains("restore uncertain") ||
                message.contains("恢复不确定");
    }

    private Notification notification(Notification.Builder builder, String detail, boolean cancellable) {
        builder.setSmallIcon(android.R.drawable.ic_media_play)
                .setContentTitle("A9 TAS session").setContentText(detail)
                .setOngoing(true);
        if (cancellable) {
            Intent cancel = new Intent(this, TasForegroundService.class).setAction(ACTION_CANCEL);
            PendingIntent pending = PendingIntent.getService(this, 9002, cancel,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            builder.addAction(new Notification.Action.Builder(
                    android.R.drawable.ic_menu_close_clear_cancel, "中断并恢复", pending).build());
        }
        return builder.build();
    }

    private void updateNotificationReadyForNextAttempt(String detail) {
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL_ID) : new Notification.Builder(this);
        notification(builder, detail, false);
        Intent next = new Intent(this, TasForegroundService.class)
                .setAction(ACTION_QUICK_RECORD);
        PendingIntent pending = PendingIntent.getService(this, 9003, next,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        builder.addAction(new Notification.Action.Builder(
                android.R.drawable.ic_media_play, "录制下一局", pending).build());
        getSystemService(NotificationManager.class).notify(NOTIFICATION_ID, builder.build());
    }

    private void updateNotification(String detail) { updateNotification(detail, false); }

    private void updateNotification(String detail, boolean cancellable) {
        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL_ID) : new Notification.Builder(this);
        getSystemService(NotificationManager.class).notify(NOTIFICATION_ID,
                notification(builder, detail, cancellable));
    }

    @Override public IBinder onBind(Intent intent) { return null; }

    @Override public void onDestroy() {
        if (overlayController != null) overlayController.hide();
        liveInstance = null;
        RootShell.closePersistent();
        super.onDestroy();
    }
}
