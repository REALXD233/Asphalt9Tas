package dev.a9tas.android;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.net.Uri;
import android.provider.Settings;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.Spinner;
import android.widget.TextView;

import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public final class MainActivity extends Activity {
    private static final int REQUEST_EXPORT_RECORDING = 41;
    private static final int REQUEST_IMPORT_RECORDING = 42;
    private static final int REQUEST_OVERLAY_PERMISSION = 43;
    private static final int REQUEST_EXPORT_DIAGNOSTICS = 44;
    private static final int REQUEST_IMPORT_PROFILE = 45;
    private static final int REQUEST_EXPORT_PROFILE = 46;
    private TextView statusText;
    private TextView profileText;
    private TextView licenseStatusText;
    private TextView deviceCodeText;
    private Spinner processSpinner;
    private Spinner recordingSpinner;
    private Spinner recordingSortSpinner;
    private Spinner controlModeSpinner;
    private Spinner replaySpeedSpinner;
    private Spinner recordSlowmoSpinner;
    private Spinner recordTickRateSpinner;
    private EditText titleInput;
    private EditText mapInput;
    private EditText carInput;
    private EditText targetTickInput;
    private EditText recordingFilterInput;
    private EditText branchDelayInput;
    private EditText licenseInput;
    private CheckBox pauseAtTargetCheck;
    private CheckBox brushSessionCheck;
    private CheckBox recordPauseInterruptCheck;
    private CheckBox autoRetryRecordCheck;
    private CheckBox continuousLoadSelectedCheck;
    private CheckBox branchAutoHandoffCheck;
    private CheckBox experimentalBypassCheck;
    private Spinner experimentalProfileSpinner;
    private Button startServiceButton;
    private Button scanButton;
    private Button importProfileButton;
    private Button exportProfileButton;
    private Button generateProfileButton;
    private Button activateLicenseButton;
    private Button clearLicenseButton;
    private Button installSessionButton;
    private Button recordButton;
    private Button replayButton;
    private Button branchRecordButton;
    private Button trimButton;
    private Button packageButton;
    private Button importButton;
    private Button exportButton;
    private Button renameButton;
    private Button deleteButton;
    private Button bulkDeleteButton;
    private Button undoDeleteButton;
    private Button cancelButton;
    private Button restoreButton;
    private Button setupToggleButton;
    private Button quickOptionsToggleButton;
    private Button profileOptionsToggleButton;
    private Button recordingInfoToggleButton;
    private Button advancedToggleButton;
    private Button overlayButton;
    private Button diagnosticButton;
    private Button diagnosticSaveButton;
    private Button compatibilityButton;
    private LinearLayout licenseCard;
    private LinearLayout prepareCard;
    private LinearLayout progressContainer;
    private LinearLayout quickOptionsContainer;
    private LinearLayout profileOptionsContainer;
    private LinearLayout recordingInfoCard;
    private LinearLayout advancedCard;
    private ProgressBar operationProgress;
    private TextView operationProgressText;
    private TextView recordingDetailText;
    private TextView libraryCountText;
    private BuildProfileRegistry registry;
    private ArtifactRegistry artifactRegistry;
    private boolean startupReady;
    private boolean setupExpanded;
    private boolean quickOptionsExpanded;
    private boolean profileOptionsExpanded;
    private boolean recordingInfoExpanded;
    private boolean advancedExpanded;
    private LicenseManager.Status cachedUiLicenseStatus;
    private long cachedUiLicenseUntilElapsed;
    private final List<GameProcessScanner.Candidate> candidates = new ArrayList<>();
    private final List<A9TasLibrary.Entry> recordings = new ArrayList<>();
    private final List<A9TasLibrary.Entry> allRecordings = new ArrayList<>();
    private boolean libraryRefreshRunning;
    private String observedLatestArchive = "";
    private String pendingExportPath = "";
    private String pendingExportSha = "";
    private String pendingProfileExportId = "";
    private DiagnosticBundle.Created pendingDiagnostic;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable statePoll = new Runnable() {
        @Override public void run() {
            android.content.SharedPreferences preferences =
                    getSharedPreferences("session", MODE_PRIVATE);
            String state = preferences.getString("state", "");
            String detail = preferences.getString("detail", "");
            if (startupReady && !state.isEmpty()) {
                boolean active = preferences.getBoolean("operation_active", false) ||
                        preferences.getBoolean("orphan_promotion_active", false);
                boolean licensed = uiLicenseStatus().valid;
                boolean cancelPending = preferences.getBoolean("cancel_pending", false);
                boolean prepared = preferences.getBoolean("prepared_ready", false);
                boolean hooks = preferences.getBoolean("session_hooks_installed", false);
                boolean branchPending = preferences.getBoolean("branch_pending", false);
                boolean recoveryRequired = "RECOVERY_REQUIRED".equals(state);
                String operationKind = preferences.getString("operation_kind", "");
                boolean nextAttemptConfigurable = !active ||
                        "record_waiting".equals(operationKind) ||
                        "cycle_switching".equals(operationKind) ||
                        "WAITING_RETRY".equals(state) ||
                        "BRANCH_ARMED_PAUSED".equals(state) ||
                        "BRANCH_PAUSED".equals(state);
                boolean archivedRecordReady = preferences.getBoolean(
                        "brush_archived_record_ready", false);
                boolean continuousLoad = preferences.getBoolean(
                        "continuous_load_selected", false);
                String selectedArchive = preferences.getString("selected_archive", "");
                boolean continuousSourceReady = !continuousLoad ||
                        !selectedArchive.isEmpty() && new File(selectedArchive).isFile();
                statusText.setText(humanState(state) + "\n" + detail);
                updateSetupVisibility(licensed, prepared, active);
                scanButton.setEnabled(licensed && !active && !branchPending &&
                        !recoveryRequired);
                installSessionButton.setEnabled(licensed && prepared && !hooks && !active &&
                        !recoveryRequired);
                recordButton.setEnabled(licensed && prepared && !active && !branchPending &&
                        !recoveryRequired && continuousSourceReady);
                recordButton.setText(recoveryRequired ? "请先恢复旧会话" :
                        active && "record_waiting".equals(
                        preferences.getString("operation_kind", "")) ?
                        "连续模式正在等待 Retry" :
                        continuousLoad && !continuousSourceReady ? "请先在录像库选择前缀" :
                        continuousLoad ? "加载所选前缀并连续续录" : archivedRecordReady ?
                        "继续连续刷圈" : hooks ? "开始连续刷圈" : "一键开始连续刷圈");
                brushSessionCheck.setEnabled(!active);
                recordPauseInterruptCheck.setEnabled(!active);
                autoRetryRecordCheck.setEnabled(nextAttemptConfigurable);
                continuousLoadSelectedCheck.setEnabled(nextAttemptConfigurable);
                String latest = preferences.getString("latest_recording", "");
                String latestArchive = preferences.getString("latest_archive", "");
                packageButton.setEnabled(!latest.isEmpty() && new File(latest).isFile());
                exportButton.setEnabled(!recordings.isEmpty() ||
                        !latest.isEmpty() && new File(latest).isFile());
                replayButton.setEnabled(licensed && prepared && !active &&
                        !branchPending && !recoveryRequired && !recordings.isEmpty());
                replaySpeedSpinner.setEnabled(!active && !branchPending);
                recordSlowmoSpinner.setEnabled(!active && !branchPending);
                int currentSlowmo = preferences.getInt("record_slowmo_divisor", 1);
                int currentSlowmoIndex = currentSlowmo == 90 ? 1 : currentSlowmo == 75 ? 2 : currentSlowmo == 2 ? 3 : currentSlowmo == 4 ? 4 : currentSlowmo == 8 ? 5 : 0;
                if (recordSlowmoSpinner.getSelectedItemPosition() != currentSlowmoIndex)
                    recordSlowmoSpinner.setSelection(currentSlowmoIndex);
                recordTickRateSpinner.setEnabled(!active && !branchPending);
                branchRecordButton.setEnabled(licensed && prepared && !active &&
                        !recoveryRequired && !recordings.isEmpty());
                branchRecordButton.setText(branchPending ?
                        preferences.getBoolean("game_process_hard_paused", false) ?
                                "恢复并准备续录" : "从当前断点开始续录" :
                        "从所选长度续录新分支");
                recordingSpinner.setEnabled(nextAttemptConfigurable);
                targetTickInput.setEnabled(nextAttemptConfigurable);
                branchAutoHandoffCheck.setEnabled(nextAttemptConfigurable);
                branchDelayInput.setEnabled(nextAttemptConfigurable &&
                        branchAutoHandoffCheck.isChecked());
                trimButton.setEnabled(!active && !recordings.isEmpty());
                renameButton.setEnabled(!active && !recordings.isEmpty());
                deleteButton.setEnabled(!active && !recordings.isEmpty());
                bulkDeleteButton.setEnabled(!active && !recordings.isEmpty());
                undoDeleteButton.setEnabled(!active &&
                        A9TasLibrary.lastTrashCount(MainActivity.this) > 0);
                restoreButton.setEnabled(!active && (prepared || hooks || "FAILED".equals(state) ||
                        "RECOVERY_REQUIRED".equals(state)));
                boolean overlayVisible = TasForegroundService.isOverlayVisible();
                overlayButton.setText(overlayVisible ? "隐藏游戏悬浮控制" : "开启游戏悬浮控制");
                overlayButton.setEnabled(licensed);
                progressContainer.setVisibility(active ? View.VISIBLE : View.GONE);
                if (active) {
                    int ticks = preferences.getInt("operation_ticks", 0);
                    int limit = preferences.getInt("operation_limit", 0);
                    String kind = preferences.getString("operation_kind", "operation");
                    boolean waitingRetry = "record_waiting".equals(kind);
                    boolean cancellable = "record".equals(kind) || waitingRetry ||
                            "replay".equals(kind) || "branch".equals(kind);
                    operationProgress.setIndeterminate(limit <= 0);
                    if (limit > 0) operationProgress.setProgress(
                            (int) Math.min(1000L, ticks * 1000L / Math.max(1, limit)), true);
                    operationProgressText.setText(waitingRetry ?
                            "连续刷圈待命 · 回到游戏直接 Retry" : limit > 0 ?
                            operationLabel(kind) + " · " + ticks + " / " + limit + " Tick" :
                            operationLabel(kind) + " · 已处理 " + ticks + " Tick · 等待自然完赛");
                    cancelButton.setEnabled(!cancelPending);
                    cancelButton.setVisibility(cancellable ? View.VISIBLE : View.GONE);
                    cancelButton.setText(cancelPending ? "正在结束…" : waitingRetry ?
                            "结束连续刷圈" : "取消并恢复游戏");
                }
                if (!latestArchive.equals(observedLatestArchive)) refreshLibrary();
            }
            handler.postDelayed(this, 750L);
        }
    };

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        DiagnosticBundle.installCrashHandler(this);
        DiagnosticBundle.recordState(this, "UI_OPEN", "A9 TAS main activity created");
        setContentView(R.layout.activity_main);
        statusText = findViewById(R.id.statusText);
        profileText = findViewById(R.id.profileText);
        licenseStatusText = findViewById(R.id.licenseStatusText);
        deviceCodeText = findViewById(R.id.deviceCodeText);
        profileText.setTextIsSelectable(true);
        processSpinner = findViewById(R.id.processSpinner);
        recordingSpinner = findViewById(R.id.recordingSpinner);
        recordingSortSpinner = findViewById(R.id.recordingSortSpinner);
        controlModeSpinner = findViewById(R.id.controlModeSpinner);
        replaySpeedSpinner = findViewById(R.id.replaySpeedSpinner);
        recordSlowmoSpinner = findViewById(R.id.recordSlowmoSpinner);
        recordTickRateSpinner = findViewById(R.id.recordTickRateSpinner);
        titleInput = findViewById(R.id.titleInput);
        mapInput = findViewById(R.id.mapInput);
        carInput = findViewById(R.id.carInput);
        targetTickInput = findViewById(R.id.targetTickInput);
        recordingFilterInput = findViewById(R.id.recordingFilterInput);
        branchDelayInput = findViewById(R.id.branchDelayInput);
        licenseInput = findViewById(R.id.licenseInput);
        pauseAtTargetCheck = findViewById(R.id.pauseAtTargetCheck);
        brushSessionCheck = findViewById(R.id.brushSessionCheck);
        recordPauseInterruptCheck = findViewById(R.id.recordPauseInterruptCheck);
        autoRetryRecordCheck = findViewById(R.id.autoRetryRecordCheck);
        continuousLoadSelectedCheck = findViewById(R.id.continuousLoadSelectedCheck);
        branchAutoHandoffCheck = findViewById(R.id.branchAutoHandoffCheck);
        experimentalBypassCheck = findViewById(R.id.experimentalBypassCheck);
        experimentalProfileSpinner = findViewById(R.id.experimentalProfileSpinner);
        startServiceButton = findViewById(R.id.startServiceButton);
        scanButton = findViewById(R.id.scanButton);
        importProfileButton = findViewById(R.id.importProfileButton);
        exportProfileButton = findViewById(R.id.exportProfileButton);
        generateProfileButton = findViewById(R.id.generateProfileButton);
        activateLicenseButton = findViewById(R.id.activateLicenseButton);
        clearLicenseButton = findViewById(R.id.clearLicenseButton);
        installSessionButton = findViewById(R.id.installSessionButton);
        recordButton = findViewById(R.id.recordButton);
        replayButton = findViewById(R.id.replayButton);
        branchRecordButton = findViewById(R.id.branchRecordButton);
        trimButton = findViewById(R.id.trimButton);
        packageButton = findViewById(R.id.packageButton);
        importButton = findViewById(R.id.importButton);
        exportButton = findViewById(R.id.exportButton);
        renameButton = findViewById(R.id.renameButton);
        deleteButton = findViewById(R.id.deleteButton);
        bulkDeleteButton = findViewById(R.id.bulkDeleteButton);
        undoDeleteButton = findViewById(R.id.undoDeleteButton);
        cancelButton = findViewById(R.id.cancelButton);
        restoreButton = findViewById(R.id.restoreButton);
        setupToggleButton = findViewById(R.id.setupToggleButton);
        quickOptionsToggleButton = findViewById(R.id.quickOptionsToggleButton);
        profileOptionsToggleButton = findViewById(R.id.profileOptionsToggleButton);
        recordingInfoToggleButton = findViewById(R.id.recordingInfoToggleButton);
        advancedToggleButton = findViewById(R.id.advancedToggleButton);
        overlayButton = findViewById(R.id.overlayButton);
        diagnosticButton = findViewById(R.id.diagnosticButton);
        diagnosticSaveButton = findViewById(R.id.diagnosticSaveButton);
        compatibilityButton = findViewById(R.id.compatibilityButton);
        licenseCard = findViewById(R.id.licenseCard);
        prepareCard = findViewById(R.id.prepareCard);
        progressContainer = findViewById(R.id.progressContainer);
        quickOptionsContainer = findViewById(R.id.quickOptionsContainer);
        profileOptionsContainer = findViewById(R.id.profileOptionsContainer);
        recordingInfoCard = findViewById(R.id.recordingInfoCard);
        advancedCard = findViewById(R.id.advancedCard);
        operationProgress = findViewById(R.id.operationProgress);
        operationProgressText = findViewById(R.id.operationProgressText);
        recordingDetailText = findViewById(R.id.recordingDetailText);
        libraryCountText = findViewById(R.id.libraryCountText);
        // Page containers preserve all existing controls, listeners and edit values.
        setupExpanded = true;
        new TasPageNavigation(this, uiLicenseStatus().valid &&
                getSharedPreferences("session", MODE_PRIVATE).getBoolean("prepared_ready", false));
        setupToggleButton.setOnClickListener(view -> {
            setupExpanded = !setupExpanded;
            view.performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP);
            android.content.SharedPreferences preferences =
                    getSharedPreferences("session", MODE_PRIVATE);
            updateSetupVisibility(uiLicenseStatus().valid,
                    preferences.getBoolean("prepared_ready", false),
                    preferences.getBoolean("operation_active", false));
        });
        quickOptionsToggleButton.setOnClickListener(view -> {
            quickOptionsExpanded = !quickOptionsExpanded;
            quickOptionsContainer.setVisibility(quickOptionsExpanded ? View.VISIBLE : View.GONE);
            quickOptionsToggleButton.setText(quickOptionsExpanded ? "收起刷圈与回放设置" :
                    "刷圈与回放设置");
            view.performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP);
        });
        profileOptionsToggleButton.setOnClickListener(view -> {
            profileOptionsExpanded = !profileOptionsExpanded;
            profileOptionsContainer.setVisibility(profileOptionsExpanded ? View.VISIBLE : View.GONE);
            profileOptionsToggleButton.setText(profileOptionsExpanded ?
                    "收起兼容性与 Profile 设置" : "兼容性与 Profile 高级设置");
            view.performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP);
        });
        recordingInfoToggleButton.setOnClickListener(view -> {
            recordingInfoExpanded = !recordingInfoExpanded;
            recordingInfoCard.setVisibility(recordingInfoExpanded ? View.VISIBLE : View.GONE);
            recordingInfoToggleButton.setText(recordingInfoExpanded ? "收起下一次录制信息" :
                    "下一次录制的信息（可选）");
            view.performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP);
        });
        advancedToggleButton.setOnClickListener(view -> {
            advancedExpanded = !advancedExpanded;
            advancedCard.setVisibility(advancedExpanded ? View.VISIBLE : View.GONE);
            advancedToggleButton.setText(advancedExpanded ? "收起恢复与高级操作" :
                    "恢复、诊断与高级操作");
            view.performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP);
        });
        ArrayAdapter<String> controlAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item,
                new String[]{"unknown", "manual", "touchdrive"});
        controlModeSpinner.setAdapter(controlAdapter);
        replaySpeedSpinner.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item,
                new String[]{"1× 标准", "2×", "4×", "8×"}));
        final int[] replaySpeedValues = {1, 2, 4, 8};
        final int[] slowmoDivisors = {1, 90, 75, 2, 4, 8};
        recordSlowmoSpinner.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item,
                new String[]{"1× 正常录制", "0.9× 慢速（实验）", "0.75× 慢速（实验）", "0.5× 慢速（实验）", "0.25× 慢速（实验）", "0.125× 慢速（实验）"}));
        int savedSlowmo = getSharedPreferences("session", MODE_PRIVATE)
                .getInt("record_slowmo_divisor", 1);
        recordSlowmoSpinner.setSelection(savedSlowmo == 90 ? 1 : savedSlowmo == 75 ? 2 : savedSlowmo == 2 ? 3 : savedSlowmo == 4 ? 4 : savedSlowmo == 8 ? 5 : 0);
        recordSlowmoSpinner.setOnItemSelectedListener(
                new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent,
                                                  View view, int position, long id) {
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putInt("record_slowmo_divisor", slowmoDivisors[
                                Math.max(0, Math.min(slowmoDivisors.length - 1, position))]).apply();
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        recordTickRateSpinner.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item,
                new String[]{"60 Tick/s · 16.667 ms（默认）",
                        "120 Tick/s · 8.333 ms（实验）", "144 Tick/s · 6.944 ms（实验）"}));
        final int[] recordTickRates = {60, 120, 144};
        int savedRate = getSharedPreferences("session", MODE_PRIVATE)
                .getInt("record_tick_hz", 60);
        SessionOrchestrator.stableRecordDeltaUs(getSharedPreferences("session", MODE_PRIVATE));
        recordTickRateSpinner.setSelection(savedRate == 120 ? 1 : savedRate == 144 ? 2 : 0);
        recordTickRateSpinner.setOnItemSelectedListener(
                new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent,
                                                  View view, int position, long id) {
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putInt("record_tick_hz", recordTickRates[
                                Math.max(0, Math.min(recordTickRates.length - 1, position))]).apply();
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        int savedReplaySpeed = getSharedPreferences("session", MODE_PRIVATE)
                .getInt("replay_speed_factor", 1);
        int savedReplaySpeedIndex = 0;
        for (int index = 0; index < replaySpeedValues.length; ++index)
            if (replaySpeedValues[index] == savedReplaySpeed)
                savedReplaySpeedIndex = index;
        replaySpeedSpinner.setSelection(savedReplaySpeedIndex);
        replaySpeedSpinner.setOnItemSelectedListener(
                new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view,
                                                  int position, long id) {
                int bounded = Math.max(0, Math.min(replaySpeedValues.length - 1, position));
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putInt("replay_speed_factor", replaySpeedValues[bounded]).apply();
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        recordingSortSpinner.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item,
                new String[]{"最新优先", "最早优先", "名称 A–Z", "名称 Z–A",
                        "最长优先", "最短优先"}));
        int savedSort = Math.max(0, Math.min(5,
                getSharedPreferences("session", MODE_PRIVATE)
                        .getInt("library_sort_index", 0)));
        recordingSortSpinner.setSelection(savedSort);
        recordingSortSpinner.setOnItemSelectedListener(
                new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view,
                                                  int position, long id) {
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putInt("library_sort_index", position).apply();
                applyLibraryView();
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        recordingFilterInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence value, int start,
                                                    int count, int after) {}
            @Override public void onTextChanged(CharSequence value, int start,
                                                int before, int count) { applyLibraryView(); }
            @Override public void afterTextChanged(Editable value) {}
        });
        restoreMetadataInputs();
        // These fields describe the recording currently being produced, not
        // the already-selected archive.  Persist every edit immediately so a
        // user can name a running attempt, return to the paused game and seal
        // it without having to press another app-side button first.
        TextWatcher liveMetadataWriter = new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence value, int start,
                                                    int count, int after) {}
            @Override public void onTextChanged(CharSequence value, int start,
                                                int before, int count) {}
            @Override public void afterTextChanged(Editable value) {
                persistMetadataInputs();
            }
        };
        titleInput.addTextChangedListener(liveMetadataWriter);
        mapInput.addTextChangedListener(liveMetadataWriter);
        carInput.addTextChangedListener(liveMetadataWriter);
        controlModeSpinner.setOnItemSelectedListener(
                new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view,
                                                  int position, long id) {
                persistMetadataInputs();
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        pauseAtTargetCheck.setChecked(getSharedPreferences("session", MODE_PRIVATE)
                .getBoolean("replay_pause_at_target", false));
        pauseAtTargetCheck.setOnCheckedChangeListener((button, checked) ->
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putBoolean("replay_pause_at_target", checked).apply());
        android.content.SharedPreferences session =
                getSharedPreferences("session", MODE_PRIVATE);
        brushSessionCheck.setChecked(
                session.getBoolean("brush_session_enabled", true));
        brushSessionCheck.setOnCheckedChangeListener((button, checked) ->
                session.edit().putBoolean("brush_session_enabled", checked).apply());
        recordPauseInterruptCheck.setChecked(
                session.getBoolean("record_pause_interrupt", true));
        recordPauseInterruptCheck.setOnCheckedChangeListener((button, checked) ->
                session.edit().putBoolean("record_pause_interrupt", checked).apply());
        autoRetryRecordCheck.setChecked(
                session.getBoolean("auto_retry_record", true));
        autoRetryRecordCheck.setOnCheckedChangeListener((button, checked) ->
                session.edit().putBoolean("auto_retry_record", checked).apply());
        continuousLoadSelectedCheck.setChecked(
                session.getBoolean("continuous_load_selected", false));
        continuousLoadSelectedCheck.setOnCheckedChangeListener((button, checked) -> {
            session.edit().putBoolean("continuous_load_selected", checked).apply();
            recordButton.setText(checked ? "加载所选前缀并连续续录" :
                    "一键开始连续刷圈");
            if (checked && session.getBoolean("record_waiting_retry", false)) {
                Intent switchIntent = new Intent(this, TasForegroundService.class)
                        .setAction(TasForegroundService.ACTION_CHECKPOINT_BRANCH);
                if (Build.VERSION.SDK_INT >= 26)
                    startForegroundService(switchIntent);
                else
                    startService(switchIntent);
            }
        });
        // v2 manual handoff means "arm now, then let the user resume in-game".
        // Do not inherit the old checkbox's different "wait before arming"
        // semantics across an APK update.
        boolean automaticBranchHandoff = session.getBoolean(
                "branch_handoff_mode_v2", false) &&
                session.getBoolean("branch_auto_handoff", false);
        if (!session.getBoolean("branch_handoff_mode_v2", false))
            session.edit().putBoolean("branch_handoff_mode_v2", true)
                    .putBoolean("branch_auto_handoff", false)
                    .putBoolean("branch_manual_resume", true).apply();
        branchAutoHandoffCheck.setChecked(automaticBranchHandoff);
        branchDelayInput.setText(Integer.toString(
                session.getInt("branch_resume_delay_seconds", 5)));
        branchDelayInput.setEnabled(automaticBranchHandoff);
        branchAutoHandoffCheck.setOnCheckedChangeListener((button, checked) -> {
            branchDelayInput.setEnabled(checked &&
                    !session.getBoolean("branch_pending", false));
            session.edit().putBoolean("branch_auto_handoff", checked)
                    .putBoolean("branch_manual_resume", !checked)
                    .putBoolean("branch_handoff_mode_v2", true).apply();
        });
        branchDelayInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence value, int start,
                                                    int count, int after) {}
            @Override public void onTextChanged(CharSequence value, int start,
                                                int before, int count) {}
            @Override public void afterTextChanged(Editable value) {
                try {
                    String text = value == null ? "" : value.toString().trim();
                    int seconds = Integer.parseInt(text.isEmpty() ? "5" : text);
                    if (seconds >= 0 && seconds <= 600)
                        session.edit().putInt("branch_resume_delay_seconds", seconds).apply();
                } catch (NumberFormatException ignored) {}
            }
        });
        reconcileInstalledVersion(session);
        reconcileColdRuntime(session);
        recoverLatestOrphanAttemptDraft(session);
        promoteRecoveredAttemptDraftAsync(session);
        initializeColdProcessUi(session);
        try {
            UtcTimestamp.selfTest();
            reloadProfileRegistry();
            artifactRegistry = ArtifactRegistry.loadAndVerify(this);
            verifyTickLengthSelection();
            verifyArchiveDecoder();
            PhysicsIntervalReceipt.selfTest();
            RecoveryPolicy.selfTest();
            startupReady = true;
            getSharedPreferences("session", MODE_PRIVATE).edit()
                    .putString("startup_selftest",
                            "API24_TIME+A9TAS1_TARGET+A9PIO2+TICK_LENGTH+COLD_RECOVERY_PASS")
                    .apply();
            statusText.setText("就绪 · " + registry.size() + " 个精确构建配置 · " +
                    artifactRegistry.backends.size() + " 个运行后端 · " +
                    artifactRegistry.artifactCount() +
                    " 个制品校验通过 · 启动自检通过");
        } catch (Exception error) {
            DiagnosticBundle.recordFailure(this, "STARTUP_SELFTEST", error);
            startupReady = false;
            getSharedPreferences("session", MODE_PRIVATE).edit()
                    .putString("startup_selftest", "FAILED:" + error.getMessage()).apply();
            registry = null;
            artifactRegistry = null;
            statusText.setText("Embedded identity check failed: " + error.getMessage());
        }
        scanButton.setOnClickListener(view -> scan());
        activateLicenseButton.setOnClickListener(view -> activateLicense());
        clearLicenseButton.setOnClickListener(view -> {
            LicenseManager.clear(this);
            invalidateUiLicenseStatus();
            licenseInput.setText("");
            refreshLicenseState();
            statusText.setText("研究许可已移除");
        });
        experimentalBypassCheck.setChecked(getSharedPreferences("session", MODE_PRIVATE)
                .getBoolean("experimental_identity_bypass", false));
        experimentalProfileSpinner.setEnabled(experimentalBypassCheck.isChecked());
        experimentalBypassCheck.setOnCheckedChangeListener((button, checked) -> {
            getSharedPreferences("session", MODE_PRIVATE).edit()
                    .putBoolean("experimental_identity_bypass", checked).apply();
            experimentalProfileSpinner.setEnabled(checked);
            updateSelection(processSpinner.getSelectedItemPosition());
        });
        experimentalProfileSpinner.setOnItemSelectedListener(
                new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view,
                                                  int position, long id) {
                BuildProfileRegistry.Profile profile = selectedExperimentalProfile();
                if (profile != null) getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putString("experimental_profile_id", profile.id).apply();
                if (experimentalBypassCheck.isChecked())
                    updateSelection(processSpinner.getSelectedItemPosition());
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        startServiceButton.setOnClickListener(view -> startSelected());
        importProfileButton.setOnClickListener(view -> chooseProfileImportSource());
        exportProfileButton.setOnClickListener(view -> chooseProfileExportDestination());
        generateProfileButton.setOnClickListener(view -> generateSelectedProfile());
        installSessionButton.setOnClickListener(view -> {
            Intent intent = new Intent(this, TasForegroundService.class)
                    .setAction(TasForegroundService.ACTION_INSTALL_SESSION);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(intent); else startService(intent);
            installSessionButton.setEnabled(false);
            statusText.setText("INSTALLING_SESSION · keep the race paused");
        });
        recordButton.setOnClickListener(view -> {
            persistMetadataInputs();
            Intent intent = new Intent(this, TasForegroundService.class)
                    .setAction(TasForegroundService.ACTION_QUICK_RECORD);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(intent); else startService(intent);
            recordButton.setEnabled(false);
            statusText.setText("正在开始快捷录制\n将自动安装会话、返回游戏并恢复比赛");
        });
        overlayButton.setOnClickListener(view -> toggleOverlay());
        packageButton.setOnClickListener(view -> packageLatestRecording());
        importButton.setOnClickListener(view -> chooseImportSource());
        replayButton.setOnClickListener(view -> {
            int selected = recordingSpinner.getSelectedItemPosition();
            if (selected < 0 || selected >= recordings.size()) {
                statusText.setText("Replay failed · select a verified A9TAS1 recording");
                return;
            }
            A9TasLibrary.Entry entry = recordings.get(selected);
            long targetTick;
            try {
                targetTick = selectedTargetTick(entry);
            } catch (Exception error) {
                reportUiFailure("REPLAY_TARGET_SELECTION", error);
                statusText.setText("回放失败 · " + error.getMessage());
                return;
            }
            if (!getSharedPreferences("session", MODE_PRIVATE).edit()
                    .putLong("replay_target_tick", targetTick)
                    .putString("replay_target_archive_sha", entry.archiveSha256).commit()) {
                statusText.setText("Replay failed · unable to save target tick");
                return;
            }
            Intent intent = new Intent(this, TasForegroundService.class)
                    .setAction(TasForegroundService.ACTION_QUICK_REPLAY);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(intent); else startService(intent);
            replayButton.setEnabled(false);
            statusText.setText("正在回放前 " + (targetTick + 1) +
                    " Tick\n完整录像保持不变，可修改长度后再次回放");
        });
        branchRecordButton.setOnClickListener(view -> startBranchRecording());
        trimButton.setOnClickListener(view -> trimSelectedRecording());
        exportButton.setOnClickListener(view -> chooseExportDestination());
        renameButton.setOnClickListener(view -> showRenameDialog());
        deleteButton.setOnClickListener(view -> showDeleteDialog());
        bulkDeleteButton.setOnClickListener(view -> showBulkDeleteDialog());
        undoDeleteButton.setOnClickListener(view -> undoLastBulkDelete());
        cancelButton.setOnClickListener(view -> {
            startService(new Intent(this, TasForegroundService.class)
                    .setAction(TasForegroundService.ACTION_CANCEL));
            cancelButton.setEnabled(false);
            cancelButton.setText("正在取消并恢复…");
        });
        restoreButton.setOnClickListener(view -> {
            startService(new Intent(this, TasForegroundService.class)
                    .setAction(TasForegroundService.ACTION_RESTORE));
            restoreButton.setEnabled(false);
            statusText.setText("正在恢复原始游戏状态…");
        });
        diagnosticButton.setOnClickListener(view -> shareDiagnostics());
        diagnosticSaveButton.setOnClickListener(view -> chooseDiagnosticDestination());
        compatibilityButton.setOnClickListener(view -> runCompatibilityPreflight());
        findViewById(R.id.stopServiceButton).setOnClickListener(view -> {
            Intent intent = new Intent(this, TasForegroundService.class)
                    .setAction(TasForegroundService.ACTION_STOP);
            startService(intent);
            statusText.setText("Session service stopped");
        });
        processSpinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view,
                                                  int position, long id) { updateSelection(position); }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {
                updateSelection(-1);
            }
        });
        recordingSpinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view,
                                                  int position, long id) {
                if (position >= 0 && position < recordings.size()) {
                    A9TasLibrary.Entry selected = recordings.get(position);
                    android.content.SharedPreferences preferences =
                            getSharedPreferences("session", MODE_PRIVATE);
                    String targetBinding = preferences.getString(
                            "replay_target_archive_sha", "");
                    long selectedTarget = selected.archiveSha256.equals(targetBinding) ?
                            preferences.getLong("replay_target_tick",
                                    selected.summary.targetTick) :
                            selected.summary.targetTick;
                    if (selectedTarget < 0 || selectedTarget >= selected.summary.frameCount)
                        selectedTarget = selected.summary.targetTick;
                    preferences.edit()
                            .putString("selected_archive", selected.file.getAbsolutePath())
                            .putString("selected_archive_sha", selected.archiveSha256)
                            .putString("selected_archive_title", selected.title())
                            .putLong("replay_target_tick", selectedTarget)
                            .putString("replay_target_archive_sha", selected.archiveSha256).apply();
                    targetTickInput.setText(Long.toString(selectedTarget + 1));
                    targetTickInput.setHint("回放长度 · 1…" +
                            selected.summary.frameCount + " Tick");
                    updateRecordingDetails(selected);
                    exportButton.setEnabled(true);
                    renameButton.setEnabled(true);
                    deleteButton.setEnabled(true);
                }
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        targetTickInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence value, int start,
                                                    int count, int after) {}
            @Override public void onTextChanged(CharSequence value, int start,
                                                int before, int count) {}
            @Override public void afterTextChanged(Editable value) {
                int position = recordingSpinner.getSelectedItemPosition();
                if (position < 0 || position >= recordings.size()) return;
                A9TasLibrary.Entry selected = recordings.get(position);
                try {
                    long target = targetTickFromLength(
                            value == null ? "" : value.toString(),
                            selected.summary.frameCount);
                    getSharedPreferences("session", MODE_PRIVATE).edit()
                            .putString("selected_archive", selected.file.getAbsolutePath())
                            .putString("selected_archive_sha", selected.archiveSha256)
                            .putString("selected_archive_title", selected.title())
                            .putLong("replay_target_tick", target)
                            .putString("replay_target_archive_sha", selected.archiveSha256)
                            .apply();
                } catch (IllegalArgumentException ignored) {
                    // Keep the last valid cursor while the user is editing a
                    // partially-entered number such as an empty field.
                }
            }
        });
        refreshLibrary();
        refreshLicenseState();
        if (Build.VERSION.SDK_INT >= 33 &&
                checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, 10);
    }

    private void initializeColdProcessUi(android.content.SharedPreferences preferences) {
        boolean active = preferences.getBoolean("operation_active", false);
        boolean prepared = preferences.getBoolean("prepared_ready", false);
        boolean hooks = preferences.getBoolean("session_hooks_installed", false);
        List<String> labels = new ArrayList<>();
        labels.add(active || prepared || hooks ?
                "当前进程会话已保留" : "尚未扫描 · 请先启动游戏");
        processSpinner.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item, labels));
        profileText.setText(active || prepared || hooks ?
                "当前进程会话已保留；如游戏已退出，请重新扫描" :
                "BuildProfile：等待扫描");
        startServiceButton.setEnabled(false);

        String oldState = preferences.getString("state", "");
        if (!active && !prepared && !hooks &&
                ("SCANNING".equals(oldState) || "SCAN_COMPLETE".equals(oldState))) {
            preferences.edit()
                    .putString("state", "READY")
                    .putString("detail", "启动游戏后点击“授权 Root 并扫描游戏”")
                    .apply();
        }
    }

    private void reconcileColdRuntime(android.content.SharedPreferences preferences) {
        boolean active = preferences.getBoolean("operation_active", false);
        boolean hooks = preferences.getBoolean("session_hooks_installed", false);
        boolean serviceLive = TasForegroundService.isLive();
        String priorState = preferences.getString("state", "");
        if (!serviceLive && !active && !hooks &&
                ("RETRY_READY".equals(priorState) || "FAILED".equals(priorState))) {
            boolean prepared = preferences.getBoolean("prepared_ready", false);
            preferences.edit()
                    .putString("state", prepared ? "PREPARED" : "READY")
                    .putString("detail", prepared ?
                            "上次失败未留下活动 Hook · 可直接重新操作" :
                            "上次失败已清理 · 请重新扫描准备游戏")
                    .apply();
            return;
        }
        if (!serviceLive && active && !hooks) {
            boolean prepared = preferences.getBoolean("prepared_ready", false);
            preferences.edit().putBoolean("operation_active", false)
                    .putBoolean("cancel_pending", false)
                    .putString("state", prepared ? "PREPARED" : "READY")
                    .putString("detail", prepared ?
                            "上次界面操作已自动清理 · 当前进程仍可直接继续" :
                            "上次界面操作已自动清理 · 可重新扫描或重试")
                    .apply();
            return;
        }
        if (!RecoveryPolicy.requiresRecovery(active, hooks,
                serviceLive)) return;
        String previousOperation = preferences.getString("operation_kind", "operation");
        boolean published = preferences.edit()
                .putBoolean("operation_active", false)
                .putBoolean("cancel_pending", false)
                .putString("state", "AUTO_RECOVERING")
                .putString("detail", "检测到上次 " + previousOperation +
                        " 服务已结束 · 正在自动恢复旧会话")
                .commit();
        if (!published) {
            DiagnosticBundle.recordFailure(this, "COLD_RUNTIME_RECOVERY",
                    new IllegalStateException("cold recovery state could not be persisted"));
            return;
        }
        DiagnosticBundle.recordState(this, "AUTO_RECOVERING",
                "Cold process lost its service; restoring the hook-owning session automatically");
        try {
            Intent restore = new Intent(this, TasForegroundService.class)
                    .setAction(TasForegroundService.ACTION_RESTORE);
            if (Build.VERSION.SDK_INT >= 26) startForegroundService(restore);
            else startService(restore);
        } catch (Exception error) {
            preferences.edit()
                    .putString("state", "RECOVERY_REQUIRED")
                    .putString("detail", "自动恢复未能启动 · 请点击恢复游戏状态")
                    .apply();
            DiagnosticBundle.recordFailure(this, "AUTO_COLD_RESTORE", error);
        }
    }

    /**
     * An APK update kills this app process but does not necessarily stop the
     * game process that owns a prepared payload or installed hooks. Never
     * present a stale pre-update session as reusable. A hook-owning session is
     * restored automatically by reconcileColdRuntime; a passive prepared
     * process is reset to the normal scan/prepare path.
     */
    private void recoverLatestOrphanAttemptDraft(
            android.content.SharedPreferences preferences) {
        String published = preferences.getString("attempt_draft_path", "");
        if (!published.isEmpty() && new File(published).isFile() &&
                preferences.getInt("attempt_draft_ticks", 0) > 0)
            return;
        try {
            File root = new File(getFilesDir(), "drafts").getCanonicalFile();
            File[] files = root.listFiles();
            if (files == null) return;
            File newest = null;
            A9TasArchive.SourceSummary newestSummary = null;
            for (File candidate : files) {
                if (!candidate.isFile() ||
                        !candidate.getName().matches("attempt-[0-9]+-[0-9]+\\.a9g4r2"))
                    continue;
                File canonical = candidate.getCanonicalFile();
                if (!root.equals(canonical.getParentFile())) continue;
                try {
                    A9TasArchive.SourceSummary summary =
                            A9TasArchive.inspectSource(canonical);
                    if (summary.frameCount < 1 ||
                            summary.frameCount > Integer.MAX_VALUE)
                        continue;
                    if (newest == null || canonical.lastModified() > newest.lastModified()) {
                        newest = canonical;
                        newestSummary = summary;
                    }
                } catch (Exception ignored) {}
            }
            if (newest == null || newestSummary == null) return;
            String hash = A9TasLibrary.sha256(newest);
            android.content.SharedPreferences.Editor editor = preferences.edit()
                    .putString("attempt_draft_path", newest.getAbsolutePath())
                    .putString("attempt_draft_sha", hash)
                    .putInt("attempt_draft_ticks", (int) newestSummary.frameCount);
            // A stale hook-owning session is more urgent than archiving its
            // already-safe draft. Preserve RECOVERY_REQUIRED until the user
            // explicitly restores the old game process; the draft metadata
            // remains available and will be promoted on the next clean open.
            if (!"RECOVERY_REQUIRED".equals(preferences.getString("state", ""))) {
                editor.putString("state", "ATTEMPT_CHECKPOINT_RECOVERED")
                        .putString("detail", "已找回上次异常前封存的 " +
                                newestSummary.frameCount + " Tick 暂停封存片段");
            }
            editor.commit();
        } catch (Exception ignored) {
            // A corrupt or foreign orphan is never published into the UI.
        }
    }

    private void promoteRecoveredAttemptDraftAsync(
            android.content.SharedPreferences preferences) {
        if ("RECOVERY_REQUIRED".equals(preferences.getString("state", "")) ||
                preferences.getBoolean("session_hooks_installed", false))
            return;
        String path = preferences.getString("attempt_draft_path", "");
        String hash = preferences.getString("attempt_draft_sha", "");
        int ticks = preferences.getInt("attempt_draft_ticks", 0);
        if (path.isEmpty() || ticks < 1 || !hash.matches("[0-9a-f]{64}")) return;
        preferences.edit().putBoolean("orphan_promotion_active", true)
                .putString("state", "CHECKPOINT_ARCHIVING")
                .putString("detail", "正在把上次封存片段直接加入录像库").apply();
        new Thread(() -> {
            try {
                A9TasLibrary.Entry archived = A9TasLibrary.promoteDraft(
                        this, new File(path), hash, ticks,
                        A9TasLibrary.metadataForLatestRaw(this));
                boolean published = preferences.edit()
                        .remove("attempt_draft_path").remove("attempt_draft_sha")
                        .remove("attempt_draft_ticks")
                        .putBoolean("orphan_promotion_active", false)
                        .putString("latest_archive", archived.file.getAbsolutePath())
                        .putString("latest_archive_sha", archived.archiveSha256)
                        .putString("latest_archive_id",
                                archived.summary.manifest.getString("recording_id"))
                        .putString("selected_archive", archived.file.getAbsolutePath())
                        .putString("selected_archive_sha", archived.archiveSha256)
                        .putString("selected_archive_title", archived.title())
                        .putLong("replay_target_tick", ticks - 1L)
                        .putString("replay_target_archive_sha", archived.archiveSha256)
                        .putString("state", "CHECKPOINT_SAVED")
                        .putString("detail", "已自动找回并保存 " + ticks + " Tick 片段")
                        .commit();
                if (!published) throw new java.io.IOException(
                        "recovered checkpoint state could not be persisted");
                runOnUiThread(this::refreshLibrary);
            } catch (Exception error) {
                reportUiFailure("ORPHAN_CHECKPOINT_PROMOTION", error);
                preferences.edit().putBoolean("orphan_promotion_active", false)
                        .putString("state", "CHECKPOINT_ARCHIVE_FAILED")
                        .putString("detail", "封存片段仍安全保留，但自动归档失败：" +
                                (error.getMessage() == null ?
                                        error.getClass().getSimpleName() : error.getMessage()))
                        .apply();
            }
        }, "a9tas-promote-recovered-checkpoint").start();
    }

    private void reconcileInstalledVersion(
            android.content.SharedPreferences preferences) {
        final int currentVersion;
        try {
            currentVersion = getPackageManager().getPackageInfo(
                    getPackageName(), 0).versionCode;
        } catch (Exception ignored) {
            return;
        }
        int seenVersion = preferences.getInt("apk_version_code_seen", 0);
        if (seenVersion == currentVersion) return;
        boolean active = preferences.getBoolean("operation_active", false);
        boolean hooks = preferences.getBoolean("session_hooks_installed", false);
        boolean prepared = preferences.getBoolean("prepared_ready", false);
        android.content.SharedPreferences.Editor editor = preferences.edit()
                .putInt("apk_version_code_seen", currentVersion)
                .putBoolean("overlay_visible", false);
        if (active || hooks) {
            editor.putBoolean("operation_active", false)
                    .putBoolean("cancel_pending", false)
                    .putString("state", "RECOVERY_REQUIRED")
                    .putString("detail", "APK 已更新；请先恢复旧会话，再重新扫描准备");
        } else {
            editor.putBoolean("branch_pending", false)
                    .putBoolean("branch_replay_ready", false)
                    .putBoolean("branch_runtime_prearmed", false)
                    .putBoolean("brush_archived_record_ready", false)
                    .putBoolean("resident_retry_record_armed", false)
                    .putBoolean("resident_retry_auto_loop", false)
                    .putBoolean("resident_pending_queued", false)
                    .remove("resident_pending_frame_limit")
                    .putBoolean("record_waiting_retry", false)
                    .remove("branch_pending_archive")
                    .remove("branch_pending_archive_sha")
                    .remove("branch_pending_target_tick")
                    .remove("branch_pending_source_sha")
                    .remove("branch_pending_pid")
                    .remove("branch_pending_start_ticks")
                    .remove("session_owner")
                    .remove("session_base");
            if (prepared) {
                editor.putBoolean("prepared_ready", false)
                        .remove("prepared_pid")
                        .remove("prepared_start_ticks")
                        .remove("prepared_profile_id")
                        .remove("prepared_process")
                        .remove("prepared_package")
                        .remove("prepared_native_sha")
                        .remove("prepared_host_machine")
                        .remove("prepared_native_bridge")
                        .remove("prepared_bridge_set")
                        .remove("prepared_libc_sha")
                        .remove("prepared_runtime_backend")
                        .remove("prepared_experimental_bypass");
            }
            editor.putString("state", "READY")
                    .putString("detail", "新版已安装；启动游戏后重新扫描并准备");
        }
        editor.commit();
    }

    private void verifyArchiveDecoder() throws Exception {
        File sample = new File(getCacheDir(), "a9tas1-decoder-selftest.a9tas");
        File librarySample = new File(new File(getFilesDir(), "library"),
                UUID.randomUUID().toString() + ".a9tas");
        File fullStaging = null;
        File targetStaging = null;
        try (InputStream input = getAssets().open("selftest/a9tas1-synthetic.a9tas");
             FileOutputStream output = new FileOutputStream(sample, false)) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            output.getFD().sync();
        }
        try {
            A9TasArchive.Summary result = A9TasArchive.inspect(sample);
            if (result.frameCount != 3 || result.intervalCount != 3 ||
                    result.fixedDeltaUs != 16667 || result.targetTick != 2)
                throw new IllegalStateException("A9TAS1 self-test identity mismatch");
            File library = new File(getFilesDir(), "library");
            if (!library.isDirectory() && !library.mkdirs())
                throw new IllegalStateException("target-tick self-test library unavailable");
            try (FileInputStream input = new FileInputStream(sample);
                 FileOutputStream output = new FileOutputStream(librarySample, false)) {
                byte[] buffer = new byte[4096];
                int count;
                while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
                output.getFD().sync();
            }
            A9TasLibrary.Entry entry = new A9TasLibrary.Entry(
                    librarySample, result, A9TasLibrary.sha256(librarySample));
            A9TasLibrary.Entry renamed = A9TasLibrary.rename(this, entry,
                    "A9TAS library self-test");
            if (!"A9TAS library self-test".equals(
                    renamed.summary.manifest.getString("title")) ||
                    !result.recordingSha256.equals(renamed.summary.recordingSha256))
                throw new IllegalStateException("rename self-test identity mismatch");
            List<A9TasLibrary.Entry> libraryViewSource = new ArrayList<>();
            libraryViewSource.add(renamed);
            if (A9TasLibrary.view(libraryViewSource, "library self-test",
                    "name_asc").size() != 1 ||
                    !A9TasLibrary.view(libraryViewSource, "definitely-no-match",
                            "newest").isEmpty() ||
                    !"原始录像".equals(renamed.lineageLabel()))
                throw new IllegalStateException("library view self-test mismatch");
            File prefix = A9TasLibrary.materializeReplaySource(this, renamed, 1);
            targetStaging = prefix;
            fullStaging = new File(new File(getFilesDir(), "replay-staging"),
                    result.recordingSha256 + ".a9g4r2");
            A9TasArchive.SourceSummary prefixSummary = A9TasArchive.inspectSource(prefix);
            if (prefixSummary.frameCount != 2 || prefixSummary.intervalCount != 2)
                throw new IllegalStateException("target-tick prefix self-test mismatch");
            A9TasLibrary.delete(this, renamed);
        } finally {
            if (sample.exists()) sample.delete();
            if (librarySample.exists() && !librarySample.delete())
                throw new IllegalStateException("target-tick self-test cleanup failed");
            if (targetStaging != null && targetStaging.exists() && !targetStaging.delete())
                throw new IllegalStateException("target-tick prefix cleanup failed");
            if (fullStaging != null && fullStaging.exists() && !fullStaging.delete())
                throw new IllegalStateException("target-tick source cleanup failed");
        }
    }

    private void activateLicense() {
        DiagnosticBundle.beginOperation(this, "LICENSE_ACTIVATION");
        try {
            LicenseManager.Status status = LicenseManager.activate(
                    this, licenseInput.getText().toString());
            invalidateUiLicenseStatus();
            licenseInput.setText("");
            refreshLicenseState();
            statusText.setText("研究许可已激活\n有效期至 " + formatEpoch(status.expires));
            updateSelection(processSpinner.getSelectedItemPosition());
        } catch (Exception error) {
            reportUiFailure("LICENSE_ACTIVATION", error);
            licenseStatusText.setText("激活失败 · " + (error.getMessage() == null ?
                    error.getClass().getSimpleName() : error.getMessage()));
        }
    }

    private void refreshLicenseState() {
        try {
            invalidateUiLicenseStatus();
            LicenseManager.Status status = LicenseManager.current(this);
            cachedUiLicenseStatus = status;
            cachedUiLicenseUntilElapsed =
                    android.os.SystemClock.elapsedRealtime() + 15_000L;
            String device = LicenseManager.deviceCode(this);
            deviceCodeText.setText("设备码 · " + device);
            licenseStatusText.setText(status.valid ?
                    "已激活 · 到期 " + formatEpoch(status.expires) :
                    "未激活 · " + status.message);
            scanButton.setEnabled(status.valid);
            if (!status.valid) startServiceButton.setEnabled(false);
        } catch (Exception error) {
            reportUiFailure("LICENSE_STATUS", error);
            deviceCodeText.setText("设备码不可用");
            licenseStatusText.setText("许可检查失败 · " + error.getMessage());
            scanButton.setEnabled(false);
            startServiceButton.setEnabled(false);
        }
    }

    private static String formatEpoch(long seconds) {
        java.text.SimpleDateFormat format = new java.text.SimpleDateFormat(
                "yyyy-MM-dd HH:mm 'UTC'", java.util.Locale.ROOT);
        format.setTimeZone(java.util.TimeZone.getTimeZone("UTC"));
        return format.format(new java.util.Date(seconds * 1000L));
    }

    @Override protected void onResume() {
        super.onResume();
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        if (preferences.getBoolean("overlay_permission_requested", false) &&
                Build.VERSION.SDK_INT >= 23 && Settings.canDrawOverlays(this)) {
            preferences.edit().putBoolean("overlay_permission_requested", false).apply();
            dispatchOverlay(TasForegroundService.ACTION_SHOW_OVERLAY);
        }
        handler.post(statePoll);
    }

    @Override protected void onPause() {
        handler.removeCallbacks(statePoll);
        super.onPause();
    }

    private void toggleOverlay() {
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        if (TasForegroundService.isOverlayVisible()) {
            dispatchOverlay(TasForegroundService.ACTION_HIDE_OVERLAY);
            return;
        }
        if (Build.VERSION.SDK_INT >= 23 && !Settings.canDrawOverlays(this)) {
            preferences.edit().putBoolean("overlay_permission_requested", true).apply();
            Intent permission = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivityForResult(permission, REQUEST_OVERLAY_PERMISSION);
            statusText.setText("请允许 A9 TAS 显示在其他应用上层\n授权后会自动开启悬浮控制");
            return;
        }
        dispatchOverlay(TasForegroundService.ACTION_SHOW_OVERLAY);
    }

    private void dispatchOverlay(String action) {
        Intent intent = new Intent(this, TasForegroundService.class).setAction(action);
        if (TasForegroundService.ACTION_HIDE_OVERLAY.equals(action)) startService(intent);
        else if (Build.VERSION.SDK_INT >= 26) startForegroundService(intent);
        else startService(intent);
    }

    private void scan() {
        if (!LicenseManager.current(this).valid) {
            statusText.setText("请先激活有效的研究许可");
            refreshLicenseState();
            return;
        }
        if (registry == null || artifactRegistry == null) return;
        DiagnosticBundle.beginOperation(this, "GAME_SCAN");
        statusText.setText("正在申请 Root 并扫描已映射的游戏进程…");
        startServiceButton.setEnabled(false);
        new Thread(() -> {
            try {
                boolean recovered = SessionOrchestrator.reconcileTerminatedRuntimeState(this);
                if (getSharedPreferences("session", MODE_PRIVATE)
                        .getBoolean("session_hooks_installed", false))
                    throw new IllegalStateException(
                            "a live session still owns hooks; stop and restore it before scanning");
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .remove("prepared_ready").remove("prepared_pid")
                         .remove("prepared_start_ticks").remove("prepared_profile_id")
                         .remove("prepared_process").remove("prepared_package")
                         .remove("prepared_native_sha").remove("session_owner")
                         .remove("prepared_host_machine").remove("prepared_native_bridge")
                         .remove("prepared_bridge_set").remove("prepared_libc_sha")
                         .remove("prepared_runtime_backend")
                         .remove("prepared_experimental_bypass")
                         .remove("session_base")
                        .putString("state", "SCANNING")
                        .putString("detail", recovered
                                ? "Recovered a terminated stale session; scanning games…"
                                : "Scanning mapped game libraries…").apply();
                List<GameProcessScanner.Candidate> found = GameProcessScanner.scan(
                        this, registry, artifactRegistry);
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putString("state", "SCAN_COMPLETE")
                        .putString("detail", "Found " + found.size() + " game process(es)").apply();
                runOnUiThread(() -> showCandidates(found));
            } catch (Exception error) {
                reportUiFailure("GAME_SCAN", error);
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putString("state", "FAILED")
                        .putString("detail", "Scan failed: " + error.getMessage()).apply();
                runOnUiThread(() -> statusText.setText("Scan failed: " + error.getMessage()));
            }
        }, "a9tas-scan").start();
    }

    private void showCandidates(List<GameProcessScanner.Candidate> found) {
        candidates.clear();
        candidates.addAll(found);
        List<String> labels = new ArrayList<>();
        for (GameProcessScanner.Candidate candidate : candidates) labels.add(candidate.toString());
        if (labels.isEmpty()) labels.add("未发现正在运行的 A9 进程");
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item, labels);
        processSpinner.setAdapter(adapter);
        statusText.setText(candidates.isEmpty() ? "未发现已映射 libAsphalt9.so 的进程"
                : "已发现 " + candidates.size() + " 个游戏进程");
        updateSelection(candidates.isEmpty() ? -1 : 0);
    }

    private void updateSelection(int position) {
        generateProfileButton.setEnabled(false);
        exportProfileButton.setEnabled(false);
        if (!LicenseManager.current(this).valid) {
            profileText.setText("研究许可未激活；扫描与 TAS 操作已锁定");
            startServiceButton.setEnabled(false);
            return;
        }
        if (position < 0 || position >= candidates.size()) {
            profileText.setText("BuildProfile：尚未扫描到游戏进程");
            startServiceButton.setEnabled(false);
            return;
        }
        GameProcessScanner.Candidate candidate = candidates.get(position);
        exportProfileButton.setEnabled(candidate.profile != null);
        ArtifactRegistry.Backend autogenBackend = candidate.backend != null ? candidate.backend :
                artifactRegistry == null ? null :
                        artifactRegistry.findExperimental(candidate.hostMachine, candidate.bridgeSet);
        boolean canAutogenerate = candidate.startTicks > 0 && candidate.profile == null &&
                Arm64ProfileAutoGenerator.supports(candidate, autogenBackend);
        generateProfileButton.setEnabled(canAutogenerate);
        // Experimental selection is a fallback for an unknown build/runtime.
        // Never let a stale spinner choice override a build that the current
        // scan has already matched to an exact Profile and backend.
        if (experimentalBypassCheck.isChecked() && !candidate.supported()) {
            BuildProfileRegistry.Profile profile = selectedExperimentalProfile();
            ArtifactRegistry.Backend backend = artifactRegistry == null ? null :
                    artifactRegistry.findExperimental(candidate.hostMachine, candidate.bridgeSet);
            if (candidate.startTicks > 0 && profile != null && backend != null) {
                profileText.setText("实验模式 · 未验证兼容\n将强制使用 " + profile.label +
                        "\n" + candidate.hostMachine + " / " + candidate.bridgeSet +
                        " · " + backend.id + "\n" + candidate.compatibilityReceipt());
                startServiceButton.setEnabled(true);
            } else {
                profileText.setText("实验模式仍无法建立执行链：缺少生命周期、Profile 或架构载荷\n" +
                        candidate.compatibilityReceipt());
                startServiceButton.setEnabled(false);
            }
            return;
        }
        if (candidate.startTicks <= 0) {
            profileText.setText("已发现游戏进程，但无法读取其生命周期身份\n" +
                    candidate.compatibilityReceipt());
            startServiceButton.setEnabled(false);
        } else if (!candidate.nativeIdentityAvailable()) {
            profileText.setText("已发现游戏进程，但无法读取 libAsphalt9.so 计算构建身份\n" +
                    candidate.compatibilityReceipt());
            startServiceButton.setEnabled(false);
        } else if (candidate.profile == null) {
            profileText.setText("当前游戏构建尚未适配 · Native SHA " +
                    candidate.nativeSha256.substring(0, 16) + "…\n" +
                    candidate.compatibilityReceipt());
            startServiceButton.setEnabled(false);
        } else if (!candidate.runtimeSupported()) {
            profileText.setText("游戏构建可识别，但运行环境尚未适配\n" +
                    candidate.runtimeLabel() + "\n" + candidate.compatibilityReceipt());
            startServiceButton.setEnabled(false);
        } else {
            profileText.setText((experimentalBypassCheck.isChecked() ?
                    "已识别验证构建 · 已忽略实验 Profile\n" : "") +
                    "兼容 · " + candidate.profile.label + "\n" +
                    candidate.runtimeLabel() + " · Build ID " + candidate.profile.buildId +
                    "\n" + candidate.compatibilityReceipt());
            startServiceButton.setEnabled(true);
        }
    }

    private void startSelected() {
        if (!LicenseManager.current(this).valid) {
            statusText.setText("准备失败 · 研究许可不可用");
            refreshLicenseState();
            return;
        }
        int position = processSpinner.getSelectedItemPosition();
        if (position < 0 || position >= candidates.size()) return;
        GameProcessScanner.Candidate candidate = candidates.get(position);
        boolean experimental = experimentalBypassCheck.isChecked() &&
                !candidate.supported();
        BuildProfileRegistry.Profile profile = experimental ? selectedExperimentalProfile() :
                candidate.profile;
        ArtifactRegistry.Backend backend = experimental ?
                artifactRegistry.findExperimental(candidate.hostMachine, candidate.bridgeSet) :
                candidate.backend;
        if (experimental) {
            if (candidate.startTicks <= 0 || profile == null || backend == null) return;
        } else if (!candidate.supported()) return;
        Intent intent = new Intent(this, TasForegroundService.class)
                .setAction(TasForegroundService.ACTION_START)
                .putExtra(TasForegroundService.EXTRA_PID, candidate.pid)
                .putExtra(TasForegroundService.EXTRA_START_TICKS, candidate.startTicks)
                .putExtra(TasForegroundService.EXTRA_PROFILE_ID, profile.id)
                .putExtra(TasForegroundService.EXTRA_PROCESS, candidate.processName)
                .putExtra(TasForegroundService.EXTRA_PACKAGE, candidate.packageName)
                // Preserve what the device actually observed.  In experimental mode the
                // selected Profile is an explicit compatibility assumption, not a forged
                // measurement of the mapped game library.
                .putExtra(TasForegroundService.EXTRA_NATIVE_SHA, candidate.nativeSha256)
                .putExtra(TasForegroundService.EXTRA_HOST_MACHINE, candidate.hostMachine)
                .putExtra(TasForegroundService.EXTRA_NATIVE_BRIDGE, candidate.nativeBridge)
                .putExtra(TasForegroundService.EXTRA_BRIDGE_SET, candidate.bridgeSet)
                .putExtra(TasForegroundService.EXTRA_RUNTIME_BACKEND, backend.id)
                .putExtra(TasForegroundService.EXTRA_LIBC_SHA, candidate.libcSha256)
                .putExtra(TasForegroundService.EXTRA_EXPERIMENTAL_BYPASS, experimental);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(intent); else startService(intent);
        statusText.setText(experimental ?
                "EXPERIMENTAL PREPARING · identity checks bypassed; the game may restart" :
                "PREPARING · fixed artifacts will be verified before the game restarts");
    }

    private BuildProfileRegistry.Profile selectedExperimentalProfile() {
        if (registry == null) return null;
        int position = experimentalProfileSpinner.getSelectedItemPosition();
        return position >= 0 && position < registry.all().size() ? registry.all().get(position) : null;
    }

    private void generateSelectedProfile() {
        int position = processSpinner.getSelectedItemPosition();
        if (position < 0 || position >= candidates.size() || artifactRegistry == null) return;
        GameProcessScanner.Candidate candidate = candidates.get(position);
        generateProfileButton.setEnabled(false);
        scanButton.setEnabled(false);
        DiagnosticBundle.beginOperation(this, "PROFILE_AUTOGEN");
        statusText.setText("正在只读分析未知 ARM64 游戏核心…\n可能需要几十秒，请保持游戏进程运行");
        new Thread(() -> {
            try {
                BuildProfileRegistry.Profile profile = Arm64ProfileAutoGenerator.generate(
                        this, candidate, artifactRegistry);
                if (!getSharedPreferences("profile_autogen_binding", MODE_PRIVATE).edit()
                        .clear()
                        .putInt("pid", candidate.pid)
                        .putLong("start_ticks", candidate.startTicks)
                        .putString("library_path", candidate.libraryPath)
                        .putString("host_machine", candidate.hostMachine)
                        .putString("bridge_set", candidate.bridgeSet)
                        .putString("native_sha256", profile.nativeSha256)
                        .putString("profile_id", profile.id)
                        .commit())
                    throw new java.io.IOException("无法发布本次进程的 Profile 身份绑定");
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putString("experimental_profile_id", profile.id).apply();
                runOnUiThread(() -> {
                    try {
                        reloadProfileRegistry();
                        statusText.setText("Profile 自动生成成功 · " + profile.label +
                                "\n正在重新扫描并按核心身份自动匹配…");
                        scanButton.setEnabled(true);
                        scan();
                    } catch (Exception error) {
                        reportUiFailure("PROFILE_AUTOGEN_RELOAD", error);
                        statusText.setText("Profile 已生成，但重新加载失败 · " + message(error));
                        scanButton.setEnabled(true);
                    }
                });
            } catch (Exception error) {
                reportUiFailure("PROFILE_AUTOGEN", error);
                runOnUiThread(() -> {
                    statusText.setText("Profile 自动定位失败 · " + message(error));
                    scanButton.setEnabled(true);
                    updateSelection(processSpinner.getSelectedItemPosition());
                });
            }
        }, "a9tas-profile-autogen").start();
    }

    private void chooseExportDestination() {
        String path;
        String sha;
        int selected = recordingSpinner.getSelectedItemPosition();
        if (selected >= 0 && selected < recordings.size()) {
            A9TasLibrary.Entry entry = recordings.get(selected);
            path = entry.file.getAbsolutePath();
            sha = entry.archiveSha256;
        } else {
            path = getSharedPreferences("session", MODE_PRIVATE)
                    .getString("latest_recording", "");
            sha = getSharedPreferences("session", MODE_PRIVATE)
                    .getString("latest_recording_sha", "");
        }
        File source = new File(path);
        if (!source.isFile()) {
            statusText.setText("Export failed · latest recording is unavailable");
            return;
        }
        DiagnosticBundle.beginOperation(this, "RECORDING_EXPORT");
        pendingExportPath = source.getAbsolutePath();
        pendingExportSha = sha;
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("application/octet-stream")
                .putExtra(Intent.EXTRA_TITLE, source.getName())
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_EXPORT_RECORDING);
    }

    @Override protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_EXPORT_PROFILE &&
                (resultCode != RESULT_OK || data == null || data.getData() == null)) {
            exportProfileButton.setEnabled(true);
            pendingProfileExportId = "";
            return;
        }
        if (requestCode == REQUEST_EXPORT_DIAGNOSTICS &&
                (resultCode != RESULT_OK || data == null || data.getData() == null)) {
            diagnosticSaveButton.setEnabled(true);
            pendingDiagnostic = null;
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        if (requestCode == REQUEST_EXPORT_DIAGNOSTICS) {
            exportDiagnostics(data.getData());
            return;
        }
        if (requestCode == REQUEST_IMPORT_RECORDING) {
            importSelectedDocument(data.getData());
            return;
        }
        if (requestCode == REQUEST_IMPORT_PROFILE) {
            importSelectedProfile(data.getData());
            return;
        }
        if (requestCode == REQUEST_EXPORT_PROFILE) {
            exportSelectedProfile(data.getData());
            return;
        }
        if (requestCode != REQUEST_EXPORT_RECORDING) return;
        Uri destination = data.getData();
        String source = pendingExportPath;
        String expectedSha = pendingExportSha;
        exportButton.setEnabled(false);
        statusText.setText("EXPORTING · writing and verifying the selected document…");
        new Thread(() -> {
            try {
                RecordingExporter.Receipt receipt = RecordingExporter.export(
                        this, source, expectedSha, destination);
                String detail = "Exported " + receipt.bytes + " bytes · SHA-256 " +
                        receipt.sha256.substring(0, 16) + "… verified";
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putString("state", "RECORDING_SAVED")
                        .putString("detail", detail)
                        .putString("latest_export_uri", destination.toString()).apply();
                runOnUiThread(() -> statusText.setText("RECORDING_SAVED · " + detail));
            } catch (Exception error) {
                reportUiFailure("RECORDING_EXPORT", error);
                String detail = error.getMessage() == null ?
                        error.getClass().getSimpleName() : error.getMessage();
                runOnUiThread(() -> {
                    statusText.setText("Export failed · " + detail);
                    exportButton.setEnabled(true);
                });
            }
        }, "a9tas-recording-export").start();
    }

    private void chooseDiagnosticDestination() {
        diagnosticSaveButton.setEnabled(false);
        DiagnosticBundle.beginOperation(this, "DIAGNOSTIC_SAVE");
        statusText.setText("正在生成脱敏诊断包…");
        new Thread(() -> {
            try {
                DiagnosticBundle.Created created = DiagnosticBundle.create(this);
                pendingDiagnostic = created;
                runOnUiThread(() -> {
                    Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                            .addCategory(Intent.CATEGORY_OPENABLE)
                            .setType("application/zip")
                            .putExtra(Intent.EXTRA_TITLE, created.file.getName())
                            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                    startActivityForResult(intent, REQUEST_EXPORT_DIAGNOSTICS);
                });
            } catch (Exception error) {
                runOnUiThread(() -> {
                    diagnosticSaveButton.setEnabled(true);
                    reportUiFailure("DIAGNOSTIC_CREATE_FOR_SAVE", error);
                    statusText.setText("诊断包生成失败\n" +
                            (error.getMessage() == null ?
                                    error.getClass().getSimpleName() : error.getMessage()));
                });
            }
        }, "a9tas-diagnostic-create").start();
    }

    private void exportDiagnostics(Uri destination) {
        final DiagnosticBundle.Created source = pendingDiagnostic;
        pendingDiagnostic = null;
        if (source == null) {
            diagnosticButton.setEnabled(true);
            diagnosticSaveButton.setEnabled(true);
            statusText.setText("诊断包导出失败\n临时诊断包已失效，请重新生成");
            return;
        }
        statusText.setText("正在导出并校验诊断包…");
        new Thread(() -> {
            try {
                DiagnosticBundle.Exported exported =
                        DiagnosticBundle.export(this, source, destination);
                if (source.file.isFile()) source.file.delete();
                runOnUiThread(() -> {
                    diagnosticButton.setEnabled(true);
                    diagnosticSaveButton.setEnabled(true);
                    statusText.setText("诊断包已导出\n" + exported.bytes +
                            " 字节 · SHA-256 " +
                            exported.sha256.substring(0, 16) + "…");
                });
            } catch (Exception error) {
                runOnUiThread(() -> {
                    diagnosticButton.setEnabled(true);
                    diagnosticSaveButton.setEnabled(true);
                    reportUiFailure("DIAGNOSTIC_EXPORT", error);
                    statusText.setText("诊断包导出失败\n" +
                            (error.getMessage() == null ?
                                    error.getClass().getSimpleName() : error.getMessage()));
                });
            }
        }, "a9tas-diagnostic-export").start();
    }

    private void shareDiagnostics() {
        diagnosticButton.setEnabled(false);
        DiagnosticBundle.beginOperation(this, "DIAGNOSTIC_SHARE");
        statusText.setText("正在生成并校验脱敏诊断包…");
        new Thread(() -> {
            try {
                DiagnosticBundle.Created created = DiagnosticBundle.create(this);
                Uri uri = DiagnosticShareProvider.uriFor(this, created.file);
                runOnUiThread(() -> {
                    Intent share = new Intent(Intent.ACTION_SEND)
                            .setType("application/zip")
                            .putExtra(Intent.EXTRA_STREAM, uri)
                            .putExtra(Intent.EXTRA_SUBJECT, "A9 TAS 脱敏诊断包");
                    share.setClipData(ClipData.newRawUri("A9 TAS diagnostics", uri));
                    share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    diagnosticButton.setEnabled(true);
                    statusText.setText("诊断包已生成 · 请选择发送方式\nSHA-256 " +
                            created.sha256.substring(0, 16) + "…");
                    startActivity(Intent.createChooser(share, "发送 A9 TAS 诊断包"));
                });
            } catch (Exception error) {
                reportUiFailure("DIAGNOSTIC_SHARE", error);
                runOnUiThread(() -> {
                    diagnosticButton.setEnabled(true);
                    statusText.setText("诊断包分享失败\n" + message(error));
                });
            }
        }, "a9tas-diagnostic-share").start();
    }

    private void runCompatibilityPreflight() {
        if (registry == null || artifactRegistry == null) {
            statusText.setText("兼容性预检不可用 · APK 启动自检尚未通过");
            return;
        }
        compatibilityButton.setEnabled(false);
        DiagnosticBundle.beginOperation(this, "COMPATIBILITY_PREFLIGHT");
        statusText.setText("正在执行只读兼容性预检…\n不会安装 Hook 或写入游戏状态");
        new Thread(() -> {
            try {
                CompatibilityReport.Result result = CompatibilityReport.run(
                        this, registry, artifactRegistry);
                runOnUiThread(() -> {
                    compatibilityButton.setEnabled(true);
                    statusText.setText("兼容性预检完成 · " + result.summary +
                            "\n可点击“一键生成并分享诊断包”发送完整报告");
                });
            } catch (Exception error) {
                reportUiFailure("COMPATIBILITY_PREFLIGHT", error);
                runOnUiThread(() -> {
                    compatibilityButton.setEnabled(true);
                    statusText.setText("兼容性预检失败\n" + message(error));
                });
            }
        }, "a9tas-compatibility-preflight").start();
    }

    private void reportUiFailure(String operation, Throwable error) {
        DiagnosticBundle.recordFailure(this, operation, error);
    }

    private static String message(Throwable error) {
        if (error == null) return "未知错误";
        String value = error.getMessage();
        return value == null || value.trim().isEmpty() ?
                error.getClass().getSimpleName() : value;
    }

    private void chooseImportSource() {
        new AlertDialog.Builder(this).setTitle("导入录像")
                .setItems(new String[]{"浏览 Documents／Download（Root）", "系统文件选择器"},
                        (dialog, which) -> {
                            if (which == 0) chooseSharedRecording();
                            else chooseSystemImportSource();
                        }).show();
    }

    private void chooseSharedRecording() {
        importButton.setEnabled(false);
        statusText.setText("正在读取 Documents／Download…");
        new Thread(() -> {
            try {
                List<String> files = SharedRecordingBrowser.list();
                runOnUiThread(() -> {
                    importButton.setEnabled(true);
                    if (isFinishing() || isDestroyed()) return;
                    if (files.isEmpty()) {
                        statusText.setText("未找到录像：请把 .a9tas 文件放在 Documents 或 Download 目录内（非子文件夹）");
                        return;
                    }
                    String[] names = new String[files.size()];
                    for (int i = 0; i < files.size(); i++) names[i] = files.get(i).substring(8);
                    statusText.setText("请选择要导入的录像");
                    new AlertDialog.Builder(this).setTitle("共享目录录像（最多200项）")
                            .setItems(names, (dialog, which) -> importRecording(null, files.get(which)))
                            .setNegativeButton("取消", null).show();
                });
            } catch (Exception error) {
                reportUiFailure("RECORDING_BROWSE", error);
                runOnUiThread(() -> {
                    importButton.setEnabled(true);
                    statusText.setText("读取目录失败 · " + message(error));
                });
            }
        }, "a9tas-recording-browse").start();
    }

    private void chooseSystemImportSource() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_IMPORT_RECORDING);
    }

    private void chooseProfileImportSource() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_IMPORT_PROFILE);
    }

    private void chooseProfileExportDestination() {
        int position = processSpinner.getSelectedItemPosition();
        BuildProfileRegistry.Profile profile = position >= 0 && position < candidates.size() ?
                candidates.get(position).profile : null;
        if (profile == null) {
            statusText.setText("Profile 导出失败 · 当前进程没有精确匹配的 BuildProfile");
            return;
        }
        pendingProfileExportId = profile.id;
        exportProfileButton.setEnabled(false);
        Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("application/json")
                .putExtra(Intent.EXTRA_TITLE,
                        "a9tas-profile-" + profile.nativeSha256.substring(0, 16) + ".json")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_EXPORT_PROFILE);
    }

    private void exportSelectedProfile(Uri destination) {
        String profileId = pendingProfileExportId;
        pendingProfileExportId = "";
        statusText.setText("正在导出并验证 ARM64 BuildProfile…");
        new Thread(() -> {
            try {
                BuildProfileRegistry.Profile profile = registry == null ? null :
                        registry.findById(profileId);
                byte[] bundle = BuildProfileRegistry.exportBundle(this, profile);
                try (OutputStream output = getContentResolver().openOutputStream(
                        destination, "w")) {
                    if (output == null) throw new java.io.IOException("无法打开导出目标");
                    output.write(bundle);
                    output.flush();
                }
                try (InputStream input = getContentResolver().openInputStream(destination)) {
                    if (input == null || !java.util.Arrays.equals(
                            bundle, BuildProfileRegistry.readAll(input)))
                        throw new java.io.IOException("导出的 Profile 回读不一致");
                }
                runOnUiThread(() -> {
                    statusText.setText("Profile 导出成功 · " + profile.label +
                            "\n可在其他同核心设备直接导入，无需更新 APK");
                    updateSelection(processSpinner.getSelectedItemPosition());
                });
            } catch (Exception error) {
                reportUiFailure("PROFILE_EXPORT", error);
                runOnUiThread(() -> {
                    statusText.setText("Profile 导出失败 · " + message(error));
                    updateSelection(processSpinner.getSelectedItemPosition());
                });
            }
        }, "a9tas-profile-export").start();
    }

    private void importSelectedProfile(Uri source) {
        DiagnosticBundle.beginOperation(this, "PROFILE_IMPORT");
        importProfileButton.setEnabled(false);
        statusText.setText("正在验证并导入 ARM64 BuildProfile…");
        new Thread(() -> {
            try (InputStream input = getContentResolver().openInputStream(source)) {
                if (input == null) throw new java.io.IOException("无法打开 Profile 文档");
                BuildProfileRegistry.Profile imported =
                        BuildProfileRegistry.importBundle(this, input);
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putString("experimental_profile_id", imported.id).apply();
                runOnUiThread(() -> {
                    try {
                        reloadProfileRegistry();
                        statusText.setText("Profile 已导入 · " + imported.label +
                                "\n重新扫描游戏后将按核心身份自动选择");
                    } catch (Exception error) {
                        reportUiFailure("PROFILE_RELOAD", error);
                        statusText.setText("Profile 已保存，但重新加载失败 · " + message(error));
                    } finally {
                        importProfileButton.setEnabled(true);
                    }
                });
            } catch (Exception error) {
                reportUiFailure("PROFILE_IMPORT", error);
                runOnUiThread(() -> {
                    statusText.setText("Profile 导入失败 · " + message(error));
                    importProfileButton.setEnabled(true);
                });
            }
        }, "a9tas-profile-import").start();
    }

    private void reloadProfileRegistry() throws Exception {
        registry = BuildProfileRegistry.load(this);
        List<String> labels = new ArrayList<>();
        for (BuildProfileRegistry.Profile profile : registry.all())
            labels.add(profile.label + (profile.imported() ? " · 已导入" : ""));
        experimentalProfileSpinner.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item, labels));
        String selected = getSharedPreferences("session", MODE_PRIVATE)
                .getString("experimental_profile_id", "");
        for (int index = 0; index < registry.all().size(); ++index)
            if (registry.all().get(index).id.equals(selected)) {
                experimentalProfileSpinner.setSelection(index);
                break;
            }
    }

    private void importSelectedDocument(Uri source) {
        importRecording(source, null);
    }

    private void importRecording(Uri source, String sharedPath) {
        DiagnosticBundle.beginOperation(this, "RECORDING_IMPORT");
        importButton.setEnabled(false);
        statusText.setText("IMPORTING · copying and strictly validating A9TAS1…");
        new Thread(() -> {
            try {
                A9TasLibrary.Entry imported = sharedPath == null
                        ? RecordingImporter.importArchive(this, source)
                        : SharedRecordingBrowser.importFile(this, sharedPath);
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putString("latest_archive", imported.file.getAbsolutePath())
                        .putString("latest_archive_sha", imported.archiveSha256)
                        .putString("latest_archive_id",
                                imported.summary.manifest.getString("recording_id"))
                        .putString("selected_archive", imported.file.getAbsolutePath())
                        .putString("selected_archive_sha", imported.archiveSha256)
                        .putString("selected_archive_title", imported.title())
                        .putString("state", "RECORDING_SAVED")
                        .putString("detail", "Imported A9TAS1 · " +
                                imported.summary.frameCount + " ticks · verified").apply();
                runOnUiThread(() -> {
                    importButton.setEnabled(true);
                    refreshLibrary();
                });
            } catch (Exception error) {
                reportUiFailure("RECORDING_IMPORT", error);
                String detail = error.getMessage() == null ?
                        error.getClass().getSimpleName() : error.getMessage();
                runOnUiThread(() -> {
                    statusText.setText("Import failed · " + detail);
                    importButton.setEnabled(true);
                });
            }
        }, "a9tas-recording-import").start();
    }

    private void persistMetadataInputs() {
        getSharedPreferences("session", MODE_PRIVATE).edit()
                .putString("recording_title", titleInput.getText().toString())
                .putString("recording_map", mapInput.getText().toString())
                .putString("recording_car", carInput.getText().toString())
                .putString("recording_control_mode",
                        String.valueOf(controlModeSpinner.getSelectedItem())).apply();
    }

    private void restoreMetadataInputs() {
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        titleInput.setText(preferences.getString("recording_title", ""));
        mapInput.setText(preferences.getString("recording_map", ""));
        carInput.setText(preferences.getString("recording_car", ""));
        String mode = preferences.getString("recording_control_mode", "unknown");
        controlModeSpinner.setSelection("manual".equals(mode) ? 1 :
                "touchdrive".equals(mode) ? 2 : 0);
    }

    private void packageLatestRecording() {
        DiagnosticBundle.beginOperation(this, "RECORDING_PACKAGE");
        persistMetadataInputs();
        packageButton.setEnabled(false);
        statusText.setText("PACKAGING · validating raw recording and writing canonical A9TAS1…");
        new Thread(() -> {
            try {
                File raw = A9TasLibrary.latestRaw(this);
                if (raw == null) throw new java.io.IOException("no private raw recording exists");
                String rawSha = A9TasLibrary.sha256(raw);
                A9TasLibrary.Listing existing = A9TasLibrary.list(this);
                A9TasLibrary.Entry packaged = null;
                for (A9TasLibrary.Entry entry : existing.valid) {
                    if (rawSha.equals(entry.summary.recordingSha256)) {
                        packaged = entry;
                        break;
                    }
                }
                if (packaged == null) packaged = A9TasLibrary.pack(this, raw, rawSha,
                        A9TasLibrary.metadataForLatestRaw(this));
                A9TasLibrary.Entry result = packaged;
                getSharedPreferences("session", MODE_PRIVATE).edit()
                        .putString("latest_archive", result.file.getAbsolutePath())
                        .putString("latest_archive_sha", result.archiveSha256)
                        .putString("latest_archive_id",
                                result.summary.manifest.getString("recording_id"))
                        .putString("state", "RECORDING_SAVED")
                        .putString("detail", "A9TAS1 verified · " +
                                result.summary.frameCount + " ticks · " + result.file.getName()).apply();
                runOnUiThread(this::refreshLibrary);
            } catch (Exception error) {
                reportUiFailure("RECORDING_PACKAGE", error);
                String detail = error.getMessage() == null ?
                        error.getClass().getSimpleName() : error.getMessage();
                runOnUiThread(() -> {
                    statusText.setText("Packaging failed · " + detail);
                    packageButton.setEnabled(true);
                });
            }
        }, "a9tas-pack-latest").start();
    }

    private A9TasLibrary.Entry selectedRecording() {
        int index = recordingSpinner.getSelectedItemPosition();
        return index >= 0 && index < recordings.size() ? recordings.get(index) : null;
    }

    private void showRenameDialog() {
        A9TasLibrary.Entry selected = selectedRecording();
        if (selected == null) return;
        EditText input = new EditText(this);
        input.setSingleLine(true);
        input.setMaxLines(1);
        input.setText(selected.summary.manifest.optString("title", ""));
        input.setSelectAllOnFocus(true);
        int padding = Math.round(20 * getResources().getDisplayMetrics().density);
        LinearLayout container = new LinearLayout(this);
        container.setPadding(padding, 0, padding, 0);
        container.addView(input, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        new AlertDialog.Builder(this)
                .setTitle("重命名录像")
                .setMessage("只修改录像标题；帧数据、哈希绑定和录像 ID 保持不变。")
                .setView(container)
                .setNegativeButton("取消", null)
                .setPositiveButton("保存", (dialog, which) ->
                        renameSelected(selected, input.getText().toString()))
                .show();
    }

    private void renameSelected(A9TasLibrary.Entry selected, String title) {
        String cleanTitle = title == null ? "" : title.trim();
        if (cleanTitle.isEmpty() || cleanTitle.length() > 120) {
            statusText.setText("重命名失败\n标题必须为 1–120 个字符");
            return;
        }
        renameButton.setEnabled(false);
        deleteButton.setEnabled(false);
        statusText.setText("正在重命名并重新验证录像…");
        new Thread(() -> {
            try {
                A9TasLibrary.Entry renamed = A9TasLibrary.rename(this, selected, cleanTitle);
                android.content.SharedPreferences preferences =
                        getSharedPreferences("session", MODE_PRIVATE);
                android.content.SharedPreferences.Editor editor = preferences.edit()
                        .putString("selected_archive", renamed.file.getAbsolutePath())
                        .putString("selected_archive_sha", renamed.archiveSha256)
                        .putString("selected_archive_title", renamed.title())
                        .putString("replay_target_archive_sha", renamed.archiveSha256);
                if (renamed.file.getAbsolutePath().equals(
                        preferences.getString("latest_archive", "")))
                    editor.putString("latest_archive_sha", renamed.archiveSha256);
                if (!editor.commit()) throw new java.io.IOException("unable to publish renamed selection");
                runOnUiThread(() -> {
                    statusText.setText("录像已重命名\n帧数据与 recording SHA-256 验证通过");
                    refreshLibrary();
                });
            } catch (Exception error) {
                reportUiFailure("RECORDING_RENAME", error);
                runOnUiThread(() -> {
                    statusText.setText("重命名失败\n" + error.getMessage());
                    renameButton.setEnabled(true);
                    deleteButton.setEnabled(true);
                });
            }
        }, "a9tas-library-rename").start();
    }

    private void showDeleteDialog() {
        A9TasLibrary.Entry selected = selectedRecording();
        if (selected == null) return;
        String title = selected.summary.manifest.optString("title", selected.file.getName());
        new AlertDialog.Builder(this)
                .setTitle("删除录像？")
                .setMessage("“" + title + "”将从 APK 私有录像库中删除。已导出的副本不会受影响。")
                .setNegativeButton("保留", null)
                .setPositiveButton("删除", (dialog, which) -> deleteSelected(selected))
                .show();
    }

    private void deleteSelected(A9TasLibrary.Entry selected) {
        DiagnosticBundle.beginOperation(this, "RECORDING_DELETE");
        renameButton.setEnabled(false);
        deleteButton.setEnabled(false);
        statusText.setText("正在删除并核对录像身份…");
        new Thread(() -> {
            try {
                A9TasLibrary.delete(this, selected);
                runOnUiThread(() -> {
                    statusText.setText("录像已删除\n游戏会话和其他录像未受影响");
                    refreshLibrary();
                });
            } catch (Exception error) {
                reportUiFailure("RECORDING_DELETE", error);
                runOnUiThread(() -> {
                    statusText.setText("删除失败\n" + error.getMessage());
                    renameButton.setEnabled(true);
                    deleteButton.setEnabled(true);
                });
            }
        }, "a9tas-library-delete").start();
    }

    private void updateRecordingDetails(A9TasLibrary.Entry entry) {
        JSONObject race = entry.summary.manifest.optJSONObject("race");
        String map = race == null ? "未知地图" : race.optString("map", "未知地图");
        String car = race == null ? "未知车辆" : race.optString("car", "未知车辆");
        recordingDetailText.setText("完整长度 " + entry.summary.frameCount + " Tick · " + map + " / " + car +
                "\n" + entry.lineageLabel() + " · " + entry.createdUtc() +
                "\n默认试播长度 " + (entry.summary.targetTick + 1) + " Tick · 固定步长 " +
                String.format(Locale.ROOT, "%.3f ms", entry.summary.fixedDeltaUs / 1000.0) +
                " · Nitro " + entry.summary.nitroCalls + " · Barrel " +
                entry.summary.barrelFrames);
    }

    private String selectedLibraryOrder() {
        switch (recordingSortSpinner == null ? 0 :
                recordingSortSpinner.getSelectedItemPosition()) {
            case 1: return "oldest";
            case 2: return "name_asc";
            case 3: return "name_desc";
            case 4: return "longest";
            case 5: return "shortest";
            default: return "newest";
        }
    }

    private void applyLibraryView() {
        if (recordingSpinner == null || recordingFilterInput == null) return;
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        String selectedPath = preferences.getString("selected_archive",
                preferences.getString("latest_archive", ""));
        recordings.clear();
        recordings.addAll(A9TasLibrary.view(allRecordings,
                recordingFilterInput.getText().toString(), selectedLibraryOrder()));
        List<String> labels = new ArrayList<>();
        for (A9TasLibrary.Entry entry : recordings) labels.add(entry.label());
        if (labels.isEmpty()) labels.add(allRecordings.isEmpty() ?
                "尚无录像" : "没有符合筛选条件的录像");
        recordingSpinner.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item, labels));
        int selection = 0;
        for (int index = 0; index < recordings.size(); ++index)
            if (recordings.get(index).file.getAbsolutePath().equals(selectedPath))
                selection = index;
        if (!recordings.isEmpty()) {
            recordingSpinner.setSelection(selection);
            updateRecordingDetails(recordings.get(selection));
        } else {
            recordingDetailText.setText(allRecordings.isEmpty() ?
                    "尚无录像" : "调整筛选条件以显示录像");
            targetTickInput.setText("");
        }
        libraryCountText.setText(recordings.size() + " 个筛选结果 · 共 " +
                allRecordings.size() + " 个录像");
        boolean active = preferences.getBoolean("operation_active", false);
        renameButton.setEnabled(!active && !recordings.isEmpty());
        deleteButton.setEnabled(!active && !recordings.isEmpty());
        bulkDeleteButton.setEnabled(!active && !recordings.isEmpty());
        undoDeleteButton.setEnabled(!active && A9TasLibrary.lastTrashCount(this) > 0);
        exportButton.setEnabled(!recordings.isEmpty() ||
                new File(preferences.getString("latest_recording", "")).isFile());
    }

    private void showBulkDeleteDialog() {
        if (recordings.isEmpty()) return;
        int count = recordings.size();
        String query = recordingFilterInput.getText().toString().trim();
        String scope = query.isEmpty() ? "当前录像库中的全部 " + count + " 个录像" :
                "筛选“" + query + "”得到的 " + count + " 个录像";
        new AlertDialog.Builder(this)
                .setTitle("批量移入回收站？")
                .setMessage(scope + "将从主录像库移走。可以使用“撤销批量删除”恢复最近一次操作；游戏进程不会受到影响。")
                .setNegativeButton("取消", null)
                .setPositiveButton("移入回收站", (dialog, which) -> trashVisibleRecordings())
                .show();
    }

    private void trashVisibleRecordings() {
        DiagnosticBundle.beginOperation(this, "RECORDING_TRASH_BATCH");
        List<A9TasLibrary.Entry> selected = new ArrayList<>(recordings);
        bulkDeleteButton.setEnabled(false);
        statusText.setText("正在校验并移动 " + selected.size() + " 个录像…");
        new Thread(() -> {
            try {
                int count = A9TasLibrary.trashBatch(this, selected);
                runOnUiThread(() -> {
                    statusText.setText("已将 " + count + " 个录像移入回收站\n可撤销最近一次批量操作");
                    refreshLibrary();
                });
            } catch (Exception error) {
                reportUiFailure("RECORDING_TRASH_BATCH", error);
                runOnUiThread(() -> {
                    statusText.setText("批量删除失败\n" + error.getMessage());
                    bulkDeleteButton.setEnabled(!recordings.isEmpty());
                });
            }
        }, "a9tas-library-trash").start();
    }

    private void undoLastBulkDelete() {
        DiagnosticBundle.beginOperation(this, "RECORDING_TRASH_UNDO");
        undoDeleteButton.setEnabled(false);
        statusText.setText("正在撤销最近一次批量删除…");
        new Thread(() -> {
            try {
                int count = A9TasLibrary.restoreLastTrashBatch(this);
                runOnUiThread(() -> {
                    statusText.setText("已恢复 " + count + " 个录像");
                    refreshLibrary();
                });
            } catch (Exception error) {
                reportUiFailure("RECORDING_TRASH_UNDO", error);
                runOnUiThread(() -> {
                    statusText.setText("撤销批量删除失败\n" + error.getMessage());
                    undoDeleteButton.setEnabled(A9TasLibrary.lastTrashCount(this) > 0);
                });
            }
        }, "a9tas-library-trash-undo").start();
    }

    private static String operationLabel(String kind) {
        if ("record".equals(kind)) return "正在录制";
        if ("record_waiting".equals(kind)) return "等待 Retry";
        if ("replay".equals(kind)) return "正在回放";
        if ("branch".equals(kind)) return "正在续录分支";
        if ("cycle_switching".equals(kind)) return "正在切换下一局";
        if ("prepare".equals(kind)) return "正在准备";
        if ("install".equals(kind)) return "正在安装会话";
        return "正在处理";
    }

    private long selectedTargetTick(A9TasLibrary.Entry entry) throws Exception {
        return targetTickFromLength(targetTickInput.getText().toString(),
                entry.summary.frameCount);
    }

    private static long targetTickFromLength(String text, long frameCount) {
        String value = text == null ? "" : text.trim();
        long length;
        try {
            length = value.isEmpty() ? frameCount : Long.parseLong(value);
        } catch (NumberFormatException error) {
            throw new IllegalArgumentException("Tick 数必须是整数");
        }
        if (length < 1 || length > frameCount)
            throw new IllegalArgumentException("Tick 数必须在 1…" +
                    frameCount + " 之间");
        return length - 1L;
    }

    private static void verifyTickLengthSelection() {
        if (targetTickFromLength("1", 800) != 0 ||
                targetTickFromLength("500", 800) != 499 ||
                targetTickFromLength("600", 800) != 599 ||
                targetTickFromLength("", 800) != 799)
            throw new IllegalStateException("Tick length conversion self-test failed");
        try {
            targetTickFromLength("0", 800);
            throw new IllegalStateException("zero Tick length was accepted");
        } catch (IllegalArgumentException expected) {}
        try {
            targetTickFromLength("801", 800);
            throw new IllegalStateException("oversized Tick length was accepted");
        } catch (IllegalArgumentException expected) {}
    }

    private void startBranchRecording() {
        DiagnosticBundle.beginOperation(this, "BRANCH_REQUEST");
        android.content.SharedPreferences preferences =
                getSharedPreferences("session", MODE_PRIVATE);
        if (preferences.getBoolean("branch_pending", false)) {
            long pendingTick = preferences.getLong("branch_pending_target_tick", -1L);
            new AlertDialog.Builder(this)
                    .setTitle("从前 " + (pendingTick + 1) + " Tick 的断点开始录制？")
                    .setMessage("工具会先武装后段录制并返回当前暂停画面，不会发送 ESC。请在游戏中手动继续；第一个真实 Tick 才会写入后段，暂停等待时间不会进入最终分支录像。")
                    .setNegativeButton("暂不续录", null)
                    .setNeutralButton("放弃此断点", (dialog, which) -> {
                        clearPendingBranch(preferences);
                        Intent restore = new Intent(this, TasForegroundService.class)
                                .setAction(TasForegroundService.ACTION_RESTORE);
                        if (Build.VERSION.SDK_INT >= 26) startForegroundService(restore);
                        else startService(restore);
                        statusText.setText("已放弃续录断点\n基础录像和当前游戏画面均未修改");
                    })
                    .setPositiveButton("武装录制后段", (dialog, which) ->
                            launchBranchService("正在从断点武装录制；完成后请手动继续游戏"))
                    .show();
            return;
        }
        int position = recordingSpinner.getSelectedItemPosition();
        if (position < 0 || position >= recordings.size()) {
            statusText.setText("续录失败 · 请先选择基础录像");
            return;
        }
        A9TasLibrary.Entry entry = recordings.get(position);
        final long tick;
        try {
            tick = selectedTargetTick(entry);
            if (tick >= entry.summary.frameCount)
                throw new IllegalArgumentException("续录点超出录像范围");
        } catch (Exception error) {
            reportUiFailure("BRANCH_TARGET_SELECTION", error);
            statusText.setText("续录失败 · " + error.getMessage());
            return;
        }
        final boolean automaticHandoff = branchAutoHandoffCheck.isChecked();
        final int delaySeconds;
        try {
            String delay = branchDelayInput.getText().toString().trim();
            delaySeconds = Integer.parseInt(delay.isEmpty() ? "5" : delay);
            if (delaySeconds < 0 || delaySeconds > 600)
                throw new IllegalArgumentException("武装后缓冲时间必须是 0…600 秒");
        } catch (NumberFormatException error) {
            statusText.setText("续录失败 · 武装后缓冲时间必须是整数秒");
            return;
        } catch (IllegalArgumentException error) {
            statusText.setText("续录失败 · " + error.getMessage());
            return;
        }
        String resumeDescription = !automaticHandoff ?
                "断点后立即武装录制并保持暂停；你在游戏中手动继续后，第一个真实 Tick 开始录制。" :
                "断点后立即武装录制，确认武装成功后保持暂停 " + delaySeconds +
                        " 秒，再自动恢复比赛。";
        new AlertDialog.Builder(this)
                .setTitle("回放前 " + (tick + 1) + " Tick 后续录？")
                .setMessage("工具会回放所选录像的前 " + (tick + 1) +
                        " Tick，然后发送第一次 ESC。" +
                        resumeDescription + "\n\n最终存档只拼接连续 Tick，不保存暂停间隔。原录像不会改变。请让新比赛停在倒计时 3。")
                .setNegativeButton("取消", null)
                .setPositiveButton("开始续录", (dialog, which) -> {
                    persistMetadataInputs();
                    boolean saved = preferences.edit()
                            .putString("selected_archive", entry.file.getAbsolutePath())
                            .putString("selected_archive_sha", entry.archiveSha256)
                            .putString("selected_archive_title", entry.title())
                            .putLong("replay_target_tick", tick)
                            .putString("replay_target_archive_sha", entry.archiveSha256)
                            .putBoolean("branch_auto_handoff", automaticHandoff)
                            .putBoolean("branch_manual_resume", !automaticHandoff)
                            .putBoolean("branch_handoff_mode_v2", true)
                            .putInt("branch_resume_delay_seconds", delaySeconds)
                            .commit();
                    if (!saved) {
                        statusText.setText("续录失败 · 无法保存分支点");
                        return;
                    }
                    launchBranchService("正在回放前 " + (tick + 1) +
                            " Tick 到续录断点");
                }).show();
    }

    private void launchBranchService(String detail) {
        Intent intent = new Intent(this, TasForegroundService.class)
                .setAction(TasForegroundService.ACTION_BRANCH_RECORD);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(intent);
        else startService(intent);
        branchRecordButton.setEnabled(false);
        statusText.setText("正在创建分支\n" + detail);
    }

    private static void clearPendingBranch(android.content.SharedPreferences preferences) {
        preferences.edit().putBoolean("branch_pending", false)
                .putBoolean("branch_replay_ready", false)
                .putBoolean("branch_runtime_prearmed", false)
                .remove("branch_pending_archive")
                .remove("branch_pending_archive_sha")
                .remove("branch_pending_target_tick")
                .remove("branch_pending_source_sha")
                .remove("branch_pending_pid")
                .remove("branch_pending_start_ticks")
                .putString("state", "BRANCH_DISCARDED")
                .putString("detail", "Paused branch point discarded; base recording preserved")
                .putLong("updated", System.currentTimeMillis()).apply();
    }

    private void trimSelectedRecording() {
        DiagnosticBundle.beginOperation(this, "RECORDING_TRIM");
        int position = recordingSpinner.getSelectedItemPosition();
        if (position < 0 || position >= recordings.size()) {
            statusText.setText("裁剪失败 · 请先选择录像");
            return;
        }
        A9TasLibrary.Entry entry = recordings.get(position);
        final long tick;
        try {
            tick = selectedTargetTick(entry);
            if (tick >= entry.summary.frameCount - 1)
                throw new IllegalArgumentException("所选 Tick 已是录像末尾，无需裁剪");
        } catch (Exception error) {
            reportUiFailure("TRIM_TARGET_SELECTION", error);
            statusText.setText("裁剪失败 · " + error.getMessage());
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("创建前 " + (tick + 1) + " Tick 的副本？")
                .setMessage("将创建一个只包含前 " + (tick + 1) +
                        " Tick 的新录像。原录像保持不变，可在确认新副本后单独删除。")
                .setNegativeButton("取消", null)
                .setPositiveButton("创建裁剪副本", (dialog, which) -> {
                    trimButton.setEnabled(false);
                    statusText.setText("正在校验并创建裁剪副本…");
                    new Thread(() -> {
                        try {
                            A9TasLibrary.Entry trimmed =
                                    A9TasBranchEditor.trimCopy(this, entry, tick);
                            getSharedPreferences("session", MODE_PRIVATE).edit()
                                    .putString("latest_archive", trimmed.file.getAbsolutePath())
                                    .putString("latest_archive_sha", trimmed.archiveSha256)
                                    .putString("latest_archive_id",
                                            trimmed.summary.manifest.getString("recording_id"))
                                    .putString("selected_archive", trimmed.file.getAbsolutePath())
                                    .putString("selected_archive_sha", trimmed.archiveSha256)
                                    .putString("selected_archive_title", trimmed.title())
                                    .putLong("replay_target_tick", trimmed.summary.targetTick)
                                    .putString("replay_target_archive_sha",
                                            trimmed.archiveSha256)
                                    .apply();
                            runOnUiThread(() -> {
                                statusText.setText("裁剪副本已保存\n" +
                                        trimmed.summary.frameCount + " Tick · 原录像未改变");
                                refreshLibrary();
                            });
                        } catch (Exception error) {
                            reportUiFailure("RECORDING_TRIM", error);
                            runOnUiThread(() -> {
                                statusText.setText("裁剪失败 · " + (error.getMessage() == null ?
                                        error.getClass().getSimpleName() : error.getMessage()));
                                trimButton.setEnabled(true);
                            });
                        }
                    }, "a9tas-trim").start();
                }).show();
    }

    /** Keep the brush-lap controls above the fold once setup has succeeded. */
    private void updateSetupVisibility(boolean licensed, boolean prepared,
                                       boolean operationActive) {
        boolean setupComplete = licensed && prepared;
        boolean showLicense = setupExpanded || !licensed;
        boolean showPrepare = setupExpanded || !prepared;
        licenseCard.setVisibility(showLicense ? View.VISIBLE : View.GONE);
        prepareCard.setVisibility(showPrepare ? View.VISIBLE : View.GONE);
        setupToggleButton.setVisibility(setupComplete ? View.VISIBLE : View.GONE);
        setupToggleButton.setEnabled(!operationActive);
        setupToggleButton.setText(setupExpanded ? "收起游戏与许可设置" :
                "更换游戏或管理许可");
        // Settings now has a permanent navigation tab, including during operations.
        setupToggleButton.setVisibility(View.GONE);
    }

    /**
     * State polling runs every 750 ms for tick progress. Re-verifying the same
     * ECDSA license on every pass adds no useful feedback, so cache only this
     * display decision briefly. Every privileged action still calls
     * LicenseManager.current() directly before it starts.
     */
    private LicenseManager.Status uiLicenseStatus() {
        long now = android.os.SystemClock.elapsedRealtime();
        if (cachedUiLicenseStatus == null || now >= cachedUiLicenseUntilElapsed) {
            cachedUiLicenseStatus = LicenseManager.current(this);
            cachedUiLicenseUntilElapsed = now + 15_000L;
        }
        return cachedUiLicenseStatus;
    }

    private void invalidateUiLicenseStatus() {
        cachedUiLicenseStatus = null;
        cachedUiLicenseUntilElapsed = 0L;
    }

    private static String humanState(String state) {
        switch (state) {
            case "PREPARING": return "正在准备游戏";
            case "PREPARED": return "游戏已准备";
            case "INSTALLING_SESSION": return "正在安装比赛会话";
            case "SESSION_INSTALLED": return "比赛会话已就绪";
            case "RECORDING": return "正在录制";
            case "RECORDING_SAVED": return "录像已保存";
            case "ATTEMPT_CHECKPOINT": return "暂停片段待归档";
            case "ATTEMPT_CHECKPOINT_RECOVERED": return "已找回暂停封存片段";
            case "ATTEMPT_SAVED_FULL": return "完整尝试已保存";
            case "ATTEMPT_DISCARDED": return "本次尝试已丢弃";
            case "CHECKPOINT_ARCHIVING": return "正在保存暂停片段";
            case "CHECKPOINT_SAVED": return "暂停片段已保存";
            case "CHECKPOINT_ARCHIVE_FAILED": return "暂停片段归档失败";
            case "WAITING_RETRY": return "连续刷圈等待 Retry";
            case "RETRY_QUEUED": return "下一局已常驻排队";
            case "RETRY_REARMING": return "正在自动武装下一局";
            case "CONTINUOUS_RECORD_STOPPED": return "连续刷圈已结束";
            case "BRUSH_BASE_SAVED": return "主分支已更新";
            case "REPLAYING": return "正在回放";
            case "REPLAY_COMPLETE": return "回放完成";
            case "REPLAY_PAUSED_AT_TARGET": return "已在目标 Tick 暂停";
            case "BRANCH_REPLAYING": return "正在回放分支前段";
            case "BRANCH_PAUSED": return "已停在续录断点";
            case "BRANCH_HARD_PAUSED": return "断点已精确冻结";
            case "BRANCH_RELEASING_HARD_PAUSE": return "正在恢复冻结断点";
            case "BRANCH_ARMING": return "正在武装续录";
            case "BRANCH_ARMED_PAUSED": return "续录已武装，保持暂停";
            case "BRANCH_RECORDING": return "正在录制新后段";
            case "BRANCH_SAVED": return "分支录像已保存";
            case "BRANCH_CHECKPOINT_SAVED": return "分支断点已保存";
            case "BRANCH_DISCARDED": return "已放弃续录断点";
            case "CANCEL_REQUESTED": return "正在取消";
            case "CANCELLED_RESTORED": return "已取消并恢复";
            case "RESTORING": return "正在恢复游戏";
            case "AUTO_RECOVERING": return "正在自动恢复";
            case "RESTORED": return "游戏已恢复";
            case "RECOVERY_REQUIRED": return "需要恢复";
            case "RETRY_READY": return "可直接重试";
            case "FAILED": return "操作失败";
            case "STOPPED": return "服务已停止";
            case "READY": return "等待扫描";
            case "SCAN_COMPLETE": return "扫描完成";
            case "SCANNING": return "正在扫描";
            case "BUSY": return "已有操作进行中";
            case "LICENSE_REQUIRED": return "需要研究许可";
            default: return state;
        }
    }

    private void refreshLibrary() {
        if (libraryRefreshRunning) return;
        libraryRefreshRunning = true;
        new Thread(() -> {
            try {
                A9TasLibrary.Listing listing = A9TasLibrary.list(this);
                runOnUiThread(() -> {
                    allRecordings.clear();
                    allRecordings.addAll(listing.valid);
                    observedLatestArchive = getSharedPreferences("session", MODE_PRIVATE)
                            .getString("latest_archive", "");
                    applyLibraryView();
                    android.content.SharedPreferences session =
                            getSharedPreferences("session", MODE_PRIVATE);
                    boolean active = session.getBoolean("operation_active", false) ||
                            session.getBoolean("orphan_promotion_active", false);
                    boolean prepared = session.getBoolean("prepared_ready", false);
                    boolean branchPending = session.getBoolean("branch_pending", false);
                    boolean recoveryRequired = "RECOVERY_REQUIRED".equals(
                            session.getString("state", ""));
                    replayButton.setEnabled(uiLicenseStatus().valid && prepared && !active &&
                            !branchPending && !recoveryRequired && !recordings.isEmpty());
                    if (!listing.invalid.isEmpty())
                        statusText.setText("录像库中有 " + listing.invalid.size() +
                                " 个无效文件；其余有效录像仍可使用");
                    libraryRefreshRunning = false;
                });
            } catch (Exception error) {
                reportUiFailure("RECORDING_LIBRARY_SCAN", error);
                runOnUiThread(() -> {
                    libraryRefreshRunning = false;
                    statusText.setText("录像库扫描失败 · " + error.getMessage());
                });
            }
        }, "a9tas-library-list").start();
    }
}
