package dev.a9tas.android;

import android.animation.ValueAnimator;
import android.app.Service;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.view.animation.DecelerateInterpolator;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * A deliberately thin control surface over the existing foreground service.
 * It never reads game memory, owns no TAS state and performs no root polling;
 * closing it therefore cannot alter an active record/replay transaction.
 */
final class TasOverlayController {
    private static final long SMOOTH_REFRESH_MILLIS = 1000L;
    private final Service service;
    private final SharedPreferences preferences;
    private final WindowManager windowManager;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private FrameLayout root;
    private WindowManager.LayoutParams params;
    private TextView bubble;
    private TextView statusText;
    private TextView tickText;
    private TextView detailText;
    private Button startButton;
    private Button responseModeButton;
    private Button checkpointButton;
    private Button stopButton;
    private Button replayButton;
    private Button branchButton;
    private Button restoreButton;
    private Button speedButton;
    private Button hardResumeButton;
    private Button metadataToggleButton;
    private LinearLayout metadataContainer;
    private TextView selectedPrefixText;
    private TextView selectedRecordingText;
    private EditText overlayTargetLengthInput;
    private Button inspectPrefixButton;
    private Spinner overlayRecordingSpinner;
    private ArrayAdapter<String> overlayRecordingAdapter;
    private EditText overlayTitleInput;
    private EditText overlayMapInput;
    private EditText overlayCarInput;
    private EditText overlayNotesInput;
    private Spinner overlayControlModeSpinner;
    private Button overlayMetadataButton;
    private final List<A9TasLibrary.Entry> overlayRecordings = new ArrayList<>();
    private boolean expanded;
    private boolean preferenceListenerRegistered;
    private boolean checkpointUiPending;
    private boolean overlaySpinnerBinding;
    private boolean overlayLibraryReloading;
    private boolean overlayMetadataSaving;
    private boolean overlayTargetTextBinding;
    private String overlayTargetDraft;
    private boolean metadataExpanded;
    private String overlayLibraryRevision = "";
    private long lastPresentedFailureUpdate = Long.MIN_VALUE;

    private final Runnable preferenceRefresh = this::refreshState;
    private final SharedPreferences.OnSharedPreferenceChangeListener preferenceListener =
            (sharedPreferences, key) -> {
                // SharedPreferences notifies once per changed key. Coalesce a
                // transaction into one render on the next display frame.
                handler.removeCallbacks(preferenceRefresh);
                handler.postDelayed(preferenceRefresh, 16L);
            };


    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            if (root == null) return;
            refreshState();
            handler.postDelayed(this, overlayRefreshMillis());
        }
    };

    TasOverlayController(Service service) {
        this.service = service;
        preferences = service.getSharedPreferences("session", Service.MODE_PRIVATE);
        windowManager = (WindowManager) service.getSystemService(Service.WINDOW_SERVICE);
    }

    boolean show() {
        if (Build.VERSION.SDK_INT >= 23 && !Settings.canDrawOverlays(service)) return false;
        if (!preferenceListenerRegistered) {
            preferences.registerOnSharedPreferenceChangeListener(preferenceListener);
            preferenceListenerRegistered = true;
        }
        if (root != null) {
            refreshState();
            return true;
        }
        root = new FrameLayout(service);
        params = new WindowManager.LayoutParams(
                dp(58), dp(58),
                Build.VERSION.SDK_INT >= 26 ?
                        WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY :
                        WindowManager.LayoutParams.TYPE_PHONE,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                        WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        params.gravity = Gravity.TOP | Gravity.START;
        params.x = Math.max(0, preferences.getInt("overlay_x", dp(12)));
        params.y = Math.max(dp(32), preferences.getInt("overlay_y", dp(180)));
        showBubble();
        windowManager.addView(root, params);
        preferences.edit().putBoolean("overlay_visible", true).apply();
        handler.removeCallbacks(refresh);
        handler.post(refresh);
        return true;
    }

    boolean isShowing() { return root != null; }

    void hide() {
        handler.removeCallbacks(refresh);
        handler.removeCallbacks(preferenceRefresh);
        if (preferenceListenerRegistered) {
            preferences.unregisterOnSharedPreferenceChangeListener(preferenceListener);
            preferenceListenerRegistered = false;
        }
        if (root != null) {
            try { windowManager.removeViewImmediate(root); }
            catch (IllegalArgumentException ignored) {}
        }
        root = null;
        params = null;
        bubble = null;
        statusText = null;
        tickText = null;
        detailText = null;
        startButton = null;
        responseModeButton = null;
        checkpointButton = null;
        stopButton = null;
        replayButton = null;
        branchButton = null;
        restoreButton = null;
        speedButton = null;
        hardResumeButton = null;
        metadataToggleButton = null;
        metadataContainer = null;
        selectedPrefixText = null;
        selectedRecordingText = null;
        overlayTargetLengthInput = null;
        inspectPrefixButton = null;
        overlayRecordingSpinner = null;
        overlayRecordingAdapter = null;
        overlayTitleInput = null;
        overlayMapInput = null;
        overlayCarInput = null;
        overlayNotesInput = null;
        overlayControlModeSpinner = null;
        overlayMetadataButton = null;
        overlayRecordings.clear();
        overlaySpinnerBinding = false;
        overlayLibraryReloading = false;
        overlayMetadataSaving = false;
        overlayTargetTextBinding = false;
        overlayTargetDraft = null;
        metadataExpanded = false;
        overlayLibraryRevision = "";
        expanded = false;
        preferences.edit().putBoolean("overlay_visible", false).apply();
    }

    private void showBubble() {
        if (root == null || params == null) return;
        disableOverlayTextInput();
        expanded = false;
        params.flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS;
        root.removeAllViews();
        params.width = dp(58);
        params.height = dp(58);
        bubble = new TextView(service);
        bubble.setText("TAS");
        bubble.setTextColor(Color.WHITE);
        bubble.setTextSize(13f);
        bubble.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        bubble.setGravity(Gravity.CENTER);
        bubble.setContentDescription("展开 A9 TAS 悬浮控制");
        bubble.setBackground(roundRect(0xE60A84FF, 29));
        bubble.setElevation(dp(10));
        root.addView(bubble, new FrameLayout.LayoutParams(dp(58), dp(58)));
        bubble.setOnTouchListener(new DragTouchListener(true));
        if (root.isAttachedToWindow()) windowManager.updateViewLayout(root, params);
        refreshState();
    }

    private void showPanel() {
        if (root == null || params == null) return;
        expanded = true;
        // Keep normal expanded controls non-focusable so opening the panel
        // during a run does not steal steering keys.  A deliberate tap on an
        // enabled metadata field temporarily opts into keyboard focus.
        params.flags = WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE |
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS;
        params.softInputMode = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE;
        root.removeAllViews();
        params.width = dp(292);
        int screenWidth = service.getResources().getDisplayMetrics().widthPixels;
        int screenHeight = service.getResources().getDisplayMetrics().heightPixels;
        params.height = Math.min(dp(620), Math.max(dp(360), screenHeight - dp(32)));
        params.x = Math.min(params.x, Math.max(dp(8), screenWidth - dp(300)));
        params.y = Math.min(params.y, Math.max(dp(16), screenHeight - params.height - dp(8)));

        LinearLayout panel = new LinearLayout(service);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(16), dp(14), dp(16), dp(14));
        panel.setBackground(roundRect(0xF2181C25, 20));
        panel.setElevation(dp(14));

        LinearLayout header = new LinearLayout(service);
        header.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = text("A9 TAS", 17, Color.WHITE, true);
        header.addView(title, new LinearLayout.LayoutParams(0, dp(44), 1f));
        Button collapse = button("收起", 0x00212121);
        collapse.setContentDescription("收起 A9 TAS 悬浮控制");
        collapse.setOnClickListener(view -> showBubble());
        header.addView(collapse, new LinearLayout.LayoutParams(dp(72), dp(44)));
        header.setOnTouchListener(new DragTouchListener(false));
        panel.addView(header, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(44)));

        statusText = text("就绪", 15, Color.WHITE, true);
        LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        statusParams.topMargin = dp(8);
        panel.addView(statusText, statusParams);
        tickText = text("0 Tick", 12, 0xFFB4B8C2, false);
        panel.addView(tickText);
        detailText = text("", 11, 0xFF8D929E, false);
        detailText.setMaxLines(3);
        LinearLayout.LayoutParams detailParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        detailParams.topMargin = dp(3);
        panel.addView(detailText, detailParams);

        ScrollView scroll = new ScrollView(service);
        scroll.setFillViewport(false);
        scroll.setVerticalFadingEdgeEnabled(true);
        scroll.setFadingEdgeLength(dp(18));
        LinearLayout content = new LinearLayout(service);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(0, 0, 0, dp(12));

        addSectionTitle(content, "连续刷圈 · 高频操作");

        responseModeButton = button("", 0xFF2B303B);
        responseModeButton.setOnClickListener(view -> cycleControlResponseMode());
        addButton(content, responseModeButton, 8);

        startButton = button("开始连续刷圈", 0xFF0A84FF);
        startButton.setOnClickListener(view -> dispatch(TasForegroundService.ACTION_QUICK_RECORD));
        addButton(content, startButton, 10);

        hardResumeButton = button("恢复并准备续录", 0xFFFF9F0A);
        hardResumeButton.setOnClickListener(view ->
                dispatch(TasForegroundService.ACTION_RESUME_HARD_PAUSE));
        hardResumeButton.setVisibility(View.GONE);
        addButton(content, hardResumeButton, 8);

        selectedPrefixText = text("当前模式：从 Tick 0 直接录制",
                12, 0xFFB4B8C2, false);
        LinearLayout.LayoutParams prefixParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        prefixParams.topMargin = dp(8);
        content.addView(selectedPrefixText, prefixParams);

        CheckBox loadPrefix = checkBox("加载当前所选存档前缀",
                "continuous_load_selected", false);
        loadPrefix.setOnCheckedChangeListener((button, checked) -> {
            preferences.edit().putBoolean(
                    "continuous_load_selected", checked).apply();
            // A checkpoint may already be waiting on a resident blank-record
            // queue. Enabling prefix load must replace that queued choice now,
            // before the user presses Retry; merely changing a preference is
            // too late once the lifecycle callback starts the next race.
            if (checked && preferences.getBoolean(
                    "record_waiting_retry", false))
                dispatch(TasForegroundService.ACTION_CHECKPOINT_BRANCH);
        });
        content.addView(loadPrefix);

        checkpointButton = button("保存至当前段（暂停时）", 0xFF2B303B);
        checkpointButton.setOnClickListener(view -> {
            // This request is intentionally asynchronous: the recorder first
            // seals the last complete Tick and only then publishes the file.
            // A tap therefore needs immediate local acknowledgement instead
            // of waiting for the service receipt to arrive on the next poll.
            checkpointUiPending = true;
            checkpointButton.setEnabled(false);
            checkpointButton.setText("已接收 · 正在保存…");
            checkpointButton.setAlpha(1f);
            checkpointButton.setBackground(roundRect(0xFF0A84FF, 13));
            if (statusText != null) {
                statusText.setText("保存请求已接收");
                statusText.setTextColor(0xFFA9A7FF);
            }
            if (detailText != null)
                detailText.setText("保存已完成的 Tick · 不会取消暂停");
            dispatch(TasForegroundService.ACTION_OVERLAY_CHECKPOINT);
        });
        addButton(content, checkpointButton, 8);

        // A destructive action still needs to look actionable.  The old muted
        // maroon was visually indistinguishable from our disabled treatment,
        // even while the Button accepted taps.
        stopButton = button("结束连续刷圈", 0xFF2B303B);
        stopButton.setOnClickListener(view -> dispatch(TasForegroundService.ACTION_CANCEL));
        addButton(content, stopButton, 8);

        TextView hint = text("跑砸后直接 Retry；工具会丢弃未保留片段，并按当前设置从 Tick 0 录制或加载所选前缀。",
                11, 0xFF8D929E, false);
        LinearLayout.LayoutParams hintParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        hintParams.topMargin = dp(9);
        content.addView(hint, hintParams);

        addSectionTitle(content, "回放与分段续录");
        overlayRecordingSpinner = new Spinner(service);
        overlayRecordingAdapter = darkSpinnerAdapter(new ArrayList<>());
        overlayRecordingSpinner.setAdapter(overlayRecordingAdapter);
        overlayRecordingSpinner.setPopupBackgroundDrawable(roundRect(0xFF20242D, 10));
        LinearLayout.LayoutParams recordingSpinnerParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(48));
        recordingSpinnerParams.topMargin = dp(8);
        content.addView(overlayRecordingSpinner, recordingSpinnerParams);
        overlayRecordingSpinner.setOnItemSelectedListener(
                new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view,
                                                  int position, long id) {
                if (!overlaySpinnerBinding) selectOverlayRecording(position);
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });

        selectedRecordingText = text("尚未选择录像", 12, 0xFFB4B8C2, false);
        content.addView(selectedRecordingText);

        TextView targetLabel = text("检查位置 · 保留长度（Tick）",
                12, 0xFFB4B8C2, true);
        LinearLayout.LayoutParams targetLabelParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        targetLabelParams.topMargin = dp(10);
        content.addView(targetLabel, targetLabelParams);

        LinearLayout targetRow = new LinearLayout(service);
        targetRow.setGravity(Gravity.CENTER_VERTICAL);
        Button minusTarget = button("−10", 0xFF2B303B);
        minusTarget.setOnClickListener(view -> adjustOverlayTargetLength(-10));
        targetRow.addView(minusTarget, new LinearLayout.LayoutParams(dp(64), dp(48)));
        overlayTargetLengthInput = editField("Tick 数", true, 7);
        overlayTargetLengthInput.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
        overlayTargetLengthInput.setGravity(Gravity.CENTER);
        overlayTargetLengthInput.setSelectAllOnFocus(true);
        overlayTargetLengthInput.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(android.text.Editable editable) {
                if (!overlayTargetTextBinding) {
                    overlayTargetDraft = editable.toString();
                    publishOverlayTargetDraft();
                }
            }
        });
        LinearLayout.LayoutParams targetInputParams = new LinearLayout.LayoutParams(
                0, dp(48), 1f);
        targetInputParams.leftMargin = dp(7);
        targetInputParams.rightMargin = dp(7);
        targetRow.addView(overlayTargetLengthInput, targetInputParams);
        Button plusTarget = button("+10", 0xFF2B303B);
        plusTarget.setOnClickListener(view -> adjustOverlayTargetLength(10));
        targetRow.addView(plusTarget, new LinearLayout.LayoutParams(dp(64), dp(48)));
        LinearLayout.LayoutParams targetRowParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(48));
        targetRowParams.topMargin = dp(5);
        content.addView(targetRow, targetRowParams);

        inspectPrefixButton = button("加载到此 Tick 并暂停检查", 0xFF0A84FF);
        inspectPrefixButton.setOnClickListener(view -> inspectSelectedPrefix());
        addButton(content, inspectPrefixButton, 7);

        replayButton = button("回放所选录像", 0xFF2B303B);
        replayButton.setOnClickListener(view ->
                dispatch(TasForegroundService.ACTION_QUICK_REPLAY));
        addButton(content, replayButton, 8);

        branchButton = button("从所选 Tick 续录", 0xFF2B303B);
        branchButton.setOnClickListener(view -> {
            String state = preferences.getString("state", "");
            dispatch("BRANCH_REPLAYING".equals(state) ||
                    "BRANCH_INTERRUPTING".equals(state) ?
                    TasForegroundService.ACTION_INTERRUPT_REPLAY :
                    preferences.getBoolean("record_waiting_retry", false) ?
                            TasForegroundService.ACTION_CHECKPOINT_BRANCH :
                            TasForegroundService.ACTION_BRANCH_RECORD);
        });
        addButton(content, branchButton, 8);

        metadataToggleButton = button("编辑当前录像信息", 0xFF2B303B);
        metadataToggleButton.setOnClickListener(view -> {
            metadataExpanded = !metadataExpanded;
            metadataContainer.setVisibility(metadataExpanded ? View.VISIBLE : View.GONE);
            metadataToggleButton.setText(metadataExpanded ? "收起录像信息编辑" :
                    "编辑当前录像信息");
            if (!metadataExpanded) disableOverlayTextInput();
            view.performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP);
        });
        addButton(content, metadataToggleButton, 8);

        metadataContainer = new LinearLayout(service);
        metadataContainer.setOrientation(LinearLayout.VERTICAL);
        metadataContainer.setVisibility(metadataExpanded ? View.VISIBLE : View.GONE);
        addSectionTitle(metadataContainer, "名称与详细信息");
        TextView editHint = text("仅修改已保存录像的信息；不会改变帧数据。下一次录制名称请在主界面“录制信息”中设置。",
                11, 0xFF8D929E, false);
        metadataContainer.addView(editHint);
        overlayTitleInput = editField("录像名称", true, 120);
        metadataContainer.addView(overlayTitleInput);
        overlayMapInput = editField("地图", true, 120);
        metadataContainer.addView(overlayMapInput);
        overlayCarInput = editField("车辆", true, 120);
        metadataContainer.addView(overlayCarInput);
        overlayControlModeSpinner = new Spinner(service);
        java.util.ArrayList<String> controlModes = new java.util.ArrayList<>();
        controlModes.add("unknown");
        controlModes.add("manual");
        controlModes.add("touchdrive");
        overlayControlModeSpinner.setAdapter(darkSpinnerAdapter(controlModes));
        metadataContainer.addView(overlayControlModeSpinner, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(46)));
        overlayNotesInput = editField("备注", true, 2_000);
        metadataContainer.addView(overlayNotesInput);
        overlayMetadataButton = button("保存名称与详细信息", 0xFF2B303B);
        overlayMetadataButton.setOnClickListener(view -> saveOverlayMetadata());
        addButton(metadataContainer, overlayMetadataButton, 8);
        content.addView(metadataContainer);

        Button library = button("录像库 · Tick · 重命名 · 导入导出", 0xFF2B303B);
        library.setOnClickListener(view -> openMainActivity());
        addButton(content, library, 8);

        addSectionTitle(content, "运行选项");
        CheckBox retainSession = checkBox("完赛后保留刷圈会话",
                "brush_session_enabled", true);
        content.addView(retainSession);
        CheckBox retry = checkBox("重开后自动准备下一局", "auto_retry_record", true);
        content.addView(retry);
        CheckBox pauseSave = checkBox("比赛暂停后保存当前片段",
                "record_pause_interrupt", true);
        content.addView(pauseSave);
        CheckBox pauseTarget = checkBox("目标 Tick 后自动暂停",
                "replay_pause_at_target", false);
        content.addView(pauseTarget);
        CheckBox autoBranch = checkBox("武装完成后自动继续游戏",
                "branch_auto_handoff", false);
        autoBranch.setOnCheckedChangeListener((button, checked) -> preferences.edit()
                .putBoolean("branch_auto_handoff", checked)
                .putBoolean("branch_manual_resume", !checked)
                .putBoolean("branch_handoff_mode_v2", true).apply());
        content.addView(autoBranch);

        speedButton = button("回放速度", 0xFF2B303B);
        speedButton.setOnClickListener(view -> cycleReplaySpeed());
        addButton(content, speedButton, 8);

        addSectionTitle(content, "环境、恢复与高级操作");
        restoreButton = button("恢复原始游戏状态", 0xFF493036);
        restoreButton.setOnClickListener(view ->
                dispatch(TasForegroundService.ACTION_RESTORE));
        addButton(content, restoreButton, 8);

        Button open = button("扫描、准备、许可与完整设置", 0xFF2B303B);
        open.setOnClickListener(view -> openMainActivity());
        addButton(content, open, 8);

        Button hide = button("关闭悬浮窗", 0xFF2B303B);
        hide.setOnClickListener(view -> TasOverlayController.this.hide());
        addButton(content, hide, 8);

        scroll.addView(content, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT,
                ScrollView.LayoutParams.WRAP_CONTENT));
        LinearLayout.LayoutParams scrollParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f);
        scrollParams.topMargin = dp(8);
        panel.addView(scroll, scrollParams);

        root.addView(panel, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));
        if (root.isAttachedToWindow()) windowManager.updateViewLayout(root, params);
        root.setAlpha(0.65f);
        root.setScaleX(0.96f);
        root.setScaleY(0.96f);
        root.animate().alpha(1f).scaleX(1f).scaleY(1f).setDuration(160L).start();
        refreshState();
        reloadOverlayLibraryAsync();
    }

    private void refreshState() {
        if (root == null) return;
        boolean active = preferences.getBoolean("operation_active", false);
        String kind = preferences.getString("operation_kind", "");
        String state = preferences.getString("state", "READY");
        int ticks = preferences.getInt("operation_ticks", 0);
        boolean recording = active && "record".equals(kind) && "RECORDING".equals(state);
        boolean branchRecording = active && "branch".equals(kind) &&
                "BRANCH_RECORDING".equals(state);
        boolean branchArmed = active && "branch".equals(kind) &&
                "BRANCH_ARMED_PAUSED".equals(state);
        boolean replaying = active && "replay".equals(kind) &&
                "REPLAYING".equals(state);
        boolean waiting = active && "record_waiting".equals(kind);
        boolean preparing = active && !recording && !branchRecording && !branchArmed &&
                !replaying && !waiting;
        boolean prepared = preferences.getBoolean("prepared_ready", false);
        boolean recovery = "RECOVERY_REQUIRED".equals(state);
        boolean retryReady = "RETRY_READY".equals(state);
        boolean hardPaused = preferences.getBoolean("game_process_hard_paused", false);
        boolean failure = isFailureState(state);
        String overlayFeedback = preferences.getString("overlay_feedback", "");
        if (checkpointUiPending && (!active || failure ||
                !(recording || branchRecording) ||
                "当前没有可封存的录制".equals(overlayFeedback)))
            checkpointUiPending = false;
        long updated = preferences.getLong("updated", 0L);
        String selectedPath = preferences.getString("selected_archive", "");
        boolean selected = !selectedPath.isEmpty() && new File(selectedPath).isFile();
        boolean loadPrefix = preferences.getBoolean("continuous_load_selected", false);
        String selectedTitle = selectedArchiveTitle(selectedPath);
        String libraryRevision = preferences.getString("latest_archive_sha", "") + ":" +
                preferences.getString("selected_archive_sha", "");
        if (overlayRecordingSpinner != null &&
                !libraryRevision.equals(overlayLibraryRevision) &&
                !overlayLibraryReloading) {
            overlayLibraryRevision = libraryRevision;
            reloadOverlayLibraryAsync();
        }

        if (failure && !expanded && updated != lastPresentedFailureUpdate) {
            lastPresentedFailureUpdate = updated;
            showPanel();
            return;
        }

        if (bubble != null) {
            setTextIfChanged(bubble, failure ? "ERR" : recording || branchRecording ? "REC" :
                    branchArmed ? "ARM" : replaying ? "PLAY" :
                    waiting ? "WAIT" : preparing ? "PREP" : "TAS");
            bubble.setTextSize(failure || recording || branchRecording || branchArmed || replaying ||
                    waiting || preparing ?
                    9f : 13f);
            bubble.setBackground(roundRect(failure ? 0xE6D92D20 : recording || branchRecording ? 0xE6FF453A :
                    branchArmed ? 0xE6FF9F0A : replaying ? 0xE634C759 : waiting ? 0xE6FF9F0A :
                            preparing ? 0xE65E5CE6 : 0xE60A84FF, 29));
        }
        if (!expanded || statusText == null) return;
        setTextIfChanged(statusText, failure ? "操作失败" : retryReady ? "可直接重试" :
                hardPaused ? "断点已精确冻结" :
                checkpointUiPending ? "保存请求已接收" :
                recovery ? "需要先恢复旧会话" :
                recording || branchRecording ? "正在录制" :
                        branchArmed ? "续录已武装 · 请手动继续" : replaying ? "正在回放" :
                        waiting ? "已保存 · 等待 Retry" : preparing ? "准备中" :
                        active ? compactState(state) : prepared ? "已准备" : "等待准备游戏");
        statusText.setTextColor(failure ? 0xFFFF6961 : retryReady ? 0xFFFFC14D :
                checkpointUiPending ? 0xFFA9A7FF :
                recovery ? 0xFFFF9F0A :
                recording || branchRecording ? 0xFFFF6B63 : branchArmed ? 0xFFFFC14D :
                        replaying ? 0xFF55D87A :
                        waiting ? 0xFFFFC14D : preparing ? 0xFFA9A7FF : Color.WHITE);
        setTextIfChanged(tickText, failure ? "请查看下方原因；修正后可直接重试" : hardPaused ?
                "游戏进程已停止 · 当前 Tick 不会继续推进" :
                checkpointUiPending ? "正在保存已完成的 Tick" :
                branchArmed ? "等待第一个真实 Tick · 暂停时间不会写入录像" :
                        active ? ticks + " Tick" : "不访问游戏内存 · 低占用待命");
        setTextIfChanged(detailText, checkpointUiPending ?
                "保存最后一个完整 Tick · 不会取消暂停" :
                preferences.getString("detail", ""));
        startButton.setEnabled(!active && prepared && !recovery &&
                (!loadPrefix || selected));
        if (responseModeButton != null) {
            boolean lowLatency = preferences.getBoolean("control_low_latency", true);
            setTextIfChanged(responseModeButton, lowLatency ?
                    "响应模式 · 极速响应" : "响应模式 · 游戏流畅");
            responseModeButton.setEnabled(!active);
            responseModeButton.setAlpha(active ? 0.58f : 1f);
        }
        if (hardResumeButton != null) {
            hardResumeButton.setVisibility(hardPaused ? View.VISIBLE : View.GONE);
            hardResumeButton.setEnabled(hardPaused && !active);
            setTextIfChanged(hardResumeButton, active && hardPaused ? "正在恢复并武装…" :
                    "恢复并准备续录");
            hardResumeButton.setAlpha(hardResumeButton.isEnabled() ? 1f : 0.58f);
        }
        setTextIfChanged(startButton, loadPrefix ? selected ?
                "加载当前前缀并连续续录" : "请先选择前缀录像" : "开始连续刷圈");
        setTextIfChanged(selectedPrefixText, loadPrefix ? selected ?
                "当前加载：" + selectedTitle + " · 前 " +
                        (preferences.getLong("replay_target_tick", -1L) + 1L) + " Tick" :
                "当前加载：尚未选择存档" :
                "当前模式：不加载存档 · 从 Tick 0 直接录制");
        checkpointButton.setEnabled((recording || branchRecording) && !checkpointUiPending);
        setTextIfChanged(checkpointButton, checkpointUiPending ?
                "已接收 · 正在保存…" : "保存至当前段（暂停时）");
        stopButton.setEnabled(active);
        long target = preferences.getLong("replay_target_tick", -1L);
        setTextIfChanged(selectedRecordingText, selected ? selectedTitle +
                (target >= 0 ? " · 前 " + (target + 1L) + " Tick" : "") : "尚未选择录像");
        if (overlayTargetLengthInput != null && overlayTargetDraft == null) {
            String wanted = selected && target >= 0 ? Long.toString(target + 1L) : "";
            if (!wanted.equals(overlayTargetLengthInput.getText().toString())) {
                overlayTargetTextBinding = true;
                setTextIfChanged(overlayTargetLengthInput, wanted);
                overlayTargetTextBinding = false;
            }
        }
        boolean libraryEditable = (!active || waiting) && !overlayMetadataSaving &&
                overlayRecordingSpinner != null && !overlayRecordings.isEmpty();
        if (overlayRecordingSpinner != null)
            overlayRecordingSpinner.setEnabled((!active || waiting) && !overlayRecordings.isEmpty());
        setOverlayEditorEnabled(libraryEditable);
        if (metadataToggleButton != null) {
            metadataToggleButton.setEnabled(libraryEditable);
            setTextIfChanged(metadataToggleButton, !libraryEditable && active ?
                    "任务结束后可编辑录像信息" : metadataExpanded ?
                    "收起录像信息编辑" : "编辑当前录像信息");
            metadataToggleButton.setAlpha(libraryEditable ? 1f : 0.58f);
        }
        replayButton.setEnabled(!active && prepared && selected && !recovery);
        if (overlayTargetLengthInput != null) {
            // The value configures a future prefix; editing it cannot mutate the
            // currently recording ticks.  Keep it available while recording or
            // waiting for Retry, but lock it once a replay has started so the UI
            // never claims that an in-flight materialized source can be changed.
            boolean targetEditable = !active || waiting || recording || branchRecording;
            overlayTargetLengthInput.setEnabled(targetEditable && selected && !recovery);
        }
        if (inspectPrefixButton != null) {
            inspectPrefixButton.setEnabled(!active && prepared && selected && !recovery);
            setTextIfChanged(inspectPrefixButton, active && "replay".equals(kind) ?
                    "正在加载检查位置…" : "加载到此 Tick 并暂停检查");
            inspectPrefixButton.setAlpha(inspectPrefixButton.isEnabled() ? 1f : 0.42f);
        }
        boolean replayCanBeInterrupted = "BRANCH_REPLAYING".equals(state) ||
                "BRANCH_INTERRUPTING".equals(state);
        branchButton.setEnabled(replayCanBeInterrupted ||
                !active && prepared && selected && !recovery);
        if (waiting && selected) branchButton.setEnabled(true);
        setTextIfChanged(branchButton, replayCanBeInterrupted ?
                "停止加载并从当前点续录" : waiting ?
                "回放已保存片段并续录" : "从所选 Tick 续录");
        restoreButton.setEnabled(!active && (prepared || recovery ||
                preferences.getBoolean("session_hooks_installed", false)));
        int speed = preferences.getInt("replay_speed_factor", 1);
        setTextIfChanged(speedButton, "回放速度 · " + speed + "×");
        startButton.setAlpha(startButton.isEnabled() ? 1f : 0.42f);
        checkpointButton.setBackground(roundRect(
                checkpointUiPending ? 0xFF0A84FF : 0xFF2B303B, 13));
        checkpointButton.setAlpha(checkpointUiPending || checkpointButton.isEnabled() ?
                1f : 0.42f);
        stopButton.setBackground(roundRect(active ? 0xFFFF453A : 0xFF2B303B, 13));
        stopButton.setTextColor(active ? Color.WHITE : 0xFF8D929E);
        stopButton.setAlpha(active ? 1f : 0.42f);
        replayButton.setAlpha(replayButton.isEnabled() ? 1f : 0.42f);
        branchButton.setAlpha(branchButton.isEnabled() ? 1f : 0.42f);
        restoreButton.setAlpha(restoreButton.isEnabled() ? 1f : 0.42f);
    }

    private static void setTextIfChanged(TextView view, CharSequence value) {
        if (!android.text.TextUtils.equals(view.getText(), value)) view.setText(value);
    }

    private static String compactState(String state) {
        if (state == null || state.isEmpty()) return "处理中";
        if (state.startsWith("REPLAY")) return "正在回放";
        if (state.startsWith("BRANCH")) return "正在处理续录";
        if (state.startsWith("CHECKPOINT")) return "正在保存片段";
        return "正在处理";
    }

    private static boolean isFailureState(String state) {
        if (state == null) return false;
        return "FAILED".equals(state) || "BUSY".equals(state) ||
                "LICENSE_REQUIRED".equals(state) || state.endsWith("_FAILED");
    }

    private String selectedArchiveTitle(String path) {
        String title = preferences.getString("selected_archive_title", "").trim();
        if (title.isEmpty() && path != null && !path.isEmpty())
            title = new File(path).getName();
        title = title.replace('\n', ' ').replace('\r', ' ').trim();
        return title.length() > 42 ? title.substring(0, 39) + "…" : title;
    }

    private EditText editField(String hint, boolean singleLine, int maximum) {
        EditText input = new EditText(service);
        input.setHint(hint);
        input.setHintTextColor(0xFF777C87);
        input.setTextColor(Color.WHITE);
        input.setTextSize(12f);
        input.setSingleLine(singleLine);
        input.setFocusableInTouchMode(true);
        input.setPadding(dp(10), 0, dp(10), 0);
        input.setFilters(new android.text.InputFilter[] {
                new android.text.InputFilter.LengthFilter(maximum)
        });
        input.setBackground(roundRect(0xFF20242D, 10));
        // A non-focusable overlay can consume the first tap before OnClick is
        // delivered.  Opt into IME focus on ACTION_DOWN so one deliberate tap
        // always makes the field editable.
        input.setOnTouchListener((view, event) -> {
            if (event.getActionMasked() == MotionEvent.ACTION_DOWN)
                enableOverlayTextInput(input);
            return false;
        });
        input.setOnClickListener(view -> enableOverlayTextInput(input));
        LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(44));
        layout.topMargin = dp(6);
        input.setLayoutParams(layout);
        return input;
    }

    private void enableOverlayTextInput(EditText input) {
        if (params == null || root == null || input == null || !input.isEnabled()) return;
        params.flags &= ~WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE;
        params.flags &= ~WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM;
        params.softInputMode = WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE |
                WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE;
        windowManager.updateViewLayout(root, params);
        input.requestFocus();
        input.setSelection(input.length());
        handler.post(() -> {
            input.requestFocus();
            android.view.inputmethod.InputMethodManager keyboard =
                    (android.view.inputmethod.InputMethodManager) service.getSystemService(
                            Service.INPUT_METHOD_SERVICE);
            if (keyboard != null) {
                keyboard.showSoftInput(input,
                        android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT);
                handler.postDelayed(() -> keyboard.showSoftInput(input,
                        android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT), 120L);
            }
        });
    }

    private void disableOverlayTextInput() {
        if (params == null || root == null) return;
        View focused = root.findFocus();
        if (focused != null) focused.clearFocus();
        android.view.inputmethod.InputMethodManager keyboard =
                (android.view.inputmethod.InputMethodManager) service.getSystemService(
                        Service.INPUT_METHOD_SERVICE);
        if (keyboard != null)
            keyboard.hideSoftInputFromWindow(root.getWindowToken(), 0);
        params.flags |= WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE;
        if (root.isAttachedToWindow()) windowManager.updateViewLayout(root, params);
    }

    private void setOverlayEditorEnabled(boolean enabled) {
        if (overlayTitleInput != null) overlayTitleInput.setEnabled(enabled);
        if (overlayMapInput != null) overlayMapInput.setEnabled(enabled);
        if (overlayCarInput != null) overlayCarInput.setEnabled(enabled);
        if (overlayNotesInput != null) overlayNotesInput.setEnabled(enabled);
        if (overlayControlModeSpinner != null) overlayControlModeSpinner.setEnabled(enabled);
        if (overlayMetadataButton != null) {
            overlayMetadataButton.setEnabled(enabled);
            overlayMetadataButton.setText(overlayMetadataSaving ?
                    "正在验证并保存…" : "保存名称与详细信息");
            overlayMetadataButton.setAlpha(enabled || overlayMetadataSaving ? 1f : 0.42f);
            overlayMetadataButton.setBackground(roundRect(
                    overlayMetadataSaving ? 0xFF0A84FF : 0xFF2B303B, 13));
        }
    }

    private void selectOverlayRecording(int position) {
        if (position < 0 || position >= overlayRecordings.size()) return;
        A9TasLibrary.Entry entry = overlayRecordings.get(position);
        String priorSha = preferences.getString("selected_archive_sha", "");
        long priorTarget = preferences.getLong("replay_target_tick", -1L);
        long target = priorSha.equals(entry.archiveSha256) ?
                Math.max(0L, Math.min(entry.summary.frameCount - 1L, priorTarget)) :
                entry.summary.frameCount - 1L;
        preferences.edit()
                .putString("selected_archive", entry.file.getAbsolutePath())
                .putString("selected_archive_sha", entry.archiveSha256)
                .putString("selected_archive_title", entry.title())
                .putLong("replay_target_tick", target)
                .putString("replay_target_archive_sha", entry.archiveSha256).apply();
        overlayTargetDraft = null;
        if (overlayTargetLengthInput != null) {
            overlayTargetTextBinding = true;
            overlayTargetLengthInput.setText(Long.toString(target + 1L));
            overlayTargetTextBinding = false;
        }
        fillOverlayMetadata(entry);
    }

    private A9TasLibrary.Entry selectedOverlayEntry() {
        if (overlayRecordingSpinner == null) return null;
        int position = overlayRecordingSpinner.getSelectedItemPosition();
        return position >= 0 && position < overlayRecordings.size() ?
                overlayRecordings.get(position) : null;
    }

    private void adjustOverlayTargetLength(int delta) {
        A9TasLibrary.Entry entry = selectedOverlayEntry();
        if (entry == null || overlayTargetLengthInput == null) return;
        long current;
        try {
            current = Long.parseLong(overlayTargetLengthInput.getText().toString().trim());
        } catch (NumberFormatException ignored) {
            current = preferences.getLong("replay_target_tick", 0L) + 1L;
        }
        long adjusted = Math.max(1L,
                Math.min(entry.summary.frameCount, current + delta));
        preferences.edit().putLong("replay_target_tick", adjusted - 1L)
                .putString("replay_target_archive_sha", entry.archiveSha256).apply();
        overlayTargetDraft = null;
        overlayTargetTextBinding = true;
        overlayTargetLengthInput.setText(Long.toString(adjusted));
        overlayTargetTextBinding = false;
        overlayTargetLengthInput.setSelection(overlayTargetLengthInput.length());
        viewFeedback(overlayTargetLengthInput);
    }

    /** Publishes a valid typed length immediately.  Continuous mode reads the
     * preference at the Retry boundary, so requiring an extra "load" button
     * made a visibly edited value silently fall back to the previous Tick. */
    private void publishOverlayTargetDraft() {
        A9TasLibrary.Entry entry = selectedOverlayEntry();
        if (entry == null || overlayTargetDraft == null) return;
        long length;
        try {
            length = Long.parseLong(overlayTargetDraft.trim());
        } catch (NumberFormatException ignored) {
            return;
        }
        if (length < 1L || length > entry.summary.frameCount) return;
        preferences.edit()
                .putString("selected_archive", entry.file.getAbsolutePath())
                .putString("selected_archive_sha", entry.archiveSha256)
                .putString("selected_archive_title", entry.title())
                .putLong("replay_target_tick", length - 1L)
                .putString("replay_target_archive_sha", entry.archiveSha256)
                .apply();
    }

    private void inspectSelectedPrefix() {
        A9TasLibrary.Entry entry = selectedOverlayEntry();
        if (entry == null || overlayTargetLengthInput == null) {
            if (detailText != null) detailText.setText("请先选择录像");
            return;
        }
        long length;
        try {
            length = Long.parseLong(overlayTargetLengthInput.getText().toString().trim());
        } catch (NumberFormatException error) {
            if (detailText != null) detailText.setText("请输入有效 Tick 数");
            return;
        }
        if (length < 1L || length > entry.summary.frameCount) {
            if (detailText != null)
                detailText.setText("可用范围：1–" + entry.summary.frameCount + " Tick");
            return;
        }
        boolean saved = preferences.edit()
                .putString("selected_archive", entry.file.getAbsolutePath())
                .putString("selected_archive_sha", entry.archiveSha256)
                .putString("selected_archive_title", entry.title())
                .putLong("replay_target_tick", length - 1L)
                .putString("replay_target_archive_sha", entry.archiveSha256)
                .putBoolean("replay_pause_at_target", true)
                .commit();
        if (!saved) {
            if (detailText != null) detailText.setText("无法保存检查位置，请重试");
            return;
        }
        overlayTargetDraft = null;
        inspectPrefixButton.setEnabled(false);
        inspectPrefixButton.setText("已接收 · 正在加载…");
        if (statusText != null) statusText.setText("正在加载检查位置");
        if (detailText != null)
            detailText.setText("将加载前 " + length + " Tick，并在实际闭合 Tick 暂停");
        viewFeedback(inspectPrefixButton);
        disableOverlayTextInput();
        dispatch(TasForegroundService.ACTION_QUICK_REPLAY);
    }

    private static void viewFeedback(View view) {
        if (view != null)
            view.performHapticFeedback(android.view.HapticFeedbackConstants.KEYBOARD_TAP);
    }

    private ArrayAdapter<String> darkSpinnerAdapter(List<String> values) {
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(service,
                android.R.layout.simple_spinner_item, values) {
            @Override public View getView(int position, View convertView,
                                          android.view.ViewGroup parent) {
                return styleSpinnerView(super.getView(position, convertView, parent));
            }

            @Override public View getDropDownView(int position, View convertView,
                                                  android.view.ViewGroup parent) {
                return styleSpinnerView(super.getDropDownView(position, convertView, parent));
            }
        };
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        return adapter;
    }

    private View styleSpinnerView(View view) {
        if (view instanceof TextView) {
            TextView label = (TextView) view;
            label.setTextColor(Color.WHITE);
            label.setTextSize(12f);
            label.setPadding(dp(12), 0, dp(12), 0);
            label.setGravity(Gravity.CENTER_VERTICAL);
        }
        view.setBackgroundColor(0xFF20242D);
        return view;
    }

    private void fillOverlayMetadata(A9TasLibrary.Entry entry) {
        if (entry == null || overlayTitleInput == null) return;
        JSONObject manifest = entry.summary.manifest;
        JSONObject race = manifest.optJSONObject("race");
        overlayTitleInput.setText(entry.title());
        overlayMapInput.setText(race == null ? "" : race.optString("map", ""));
        overlayCarInput.setText(race == null ? "" : race.optString("car", ""));
        String mode = race == null ? "unknown" : race.optString("control_mode", "unknown");
        overlayControlModeSpinner.setSelection("manual".equals(mode) ? 1 :
                "touchdrive".equals(mode) ? 2 : 0);
        overlayNotesInput.setText(manifest.optString("notes", ""));
    }

    private void reloadOverlayLibraryAsync() {
        if (overlayLibraryReloading || overlayRecordingSpinner == null) return;
        overlayLibraryReloading = true;
        new Thread(() -> {
            try {
                A9TasLibrary.Listing listing = A9TasLibrary.list(service);
                List<A9TasLibrary.Entry> entries = new ArrayList<>(listing.valid);
                handler.post(() -> publishOverlayLibrary(entries));
            } catch (Exception error) {
                handler.post(() -> {
                    overlayLibraryReloading = false;
                    if (detailText != null)
                        detailText.setText("录像库读取失败 · " + error.getMessage());
                });
            }
        }, "a9tas-overlay-library-list").start();
    }

    private void publishOverlayLibrary(List<A9TasLibrary.Entry> entries) {
        overlayLibraryReloading = false;
        if (overlayRecordingSpinner == null || overlayRecordingAdapter == null) return;
        String preferred = preferences.getString("selected_archive", "");
        overlaySpinnerBinding = true;
        overlayRecordings.clear();
        overlayRecordings.addAll(entries);
        overlayRecordingAdapter.clear();
        int selectedIndex = -1;
        for (int index = 0; index < entries.size(); ++index) {
            A9TasLibrary.Entry entry = entries.get(index);
            overlayRecordingAdapter.add(entry.label());
            if (entry.file.getAbsolutePath().equals(preferred)) selectedIndex = index;
        }
        overlayRecordingAdapter.notifyDataSetChanged();
        if (selectedIndex < 0 && !entries.isEmpty()) selectedIndex = 0;
        if (selectedIndex >= 0) {
            overlayRecordingSpinner.setSelection(selectedIndex, false);
            fillOverlayMetadata(entries.get(selectedIndex));
        } else {
            overlayTitleInput.setText("");
            overlayMapInput.setText("");
            overlayCarInput.setText("");
            overlayNotesInput.setText("");
        }
        overlaySpinnerBinding = false;
        if (selectedIndex >= 0 && preferred.isEmpty())
            selectOverlayRecording(selectedIndex);
        refreshState();
    }

    private void saveOverlayMetadata() {
        if (overlayMetadataSaving || overlayRecordingSpinner == null) return;
        int index = overlayRecordingSpinner.getSelectedItemPosition();
        if (index < 0 || index >= overlayRecordings.size()) return;
        if (preferences.getBoolean("operation_active", false) &&
                !"record_waiting".equals(preferences.getString("operation_kind", ""))) {
            if (detailText != null)
                detailText.setText("录制/回放中会锁定录像；等待 Retry 时可以编辑");
            return;
        }
        A9TasLibrary.Entry entry = overlayRecordings.get(index);
        String title = overlayTitleInput.getText().toString();
        String map = overlayMapInput.getText().toString();
        String car = overlayCarInput.getText().toString();
        String notes = overlayNotesInput.getText().toString();
        String mode = String.valueOf(overlayControlModeSpinner.getSelectedItem());
        overlayMetadataSaving = true;
        if (statusText != null) statusText.setText("正在保存录像信息");
        setOverlayEditorEnabled(false);
        new Thread(() -> {
            try {
                A9TasLibrary.Entry edited = A9TasLibrary.editMetadata(
                        service, entry, title, map, car, mode, notes);
                SharedPreferences.Editor editor = preferences.edit()
                        .putString("selected_archive", edited.file.getAbsolutePath())
                        .putString("selected_archive_sha", edited.archiveSha256)
                        .putString("selected_archive_title", edited.title())
                        .putString("replay_target_archive_sha", edited.archiveSha256)
                        .putString("overlay_feedback", "录像信息已保存 · 帧数据未改变");
                if (entry.file.getAbsolutePath().equals(
                        preferences.getString("latest_archive", "")))
                    editor.putString("latest_archive_sha", edited.archiveSha256);
                if (!editor.commit()) throw new java.io.IOException("录像选择状态无法更新");
                handler.post(() -> {
                    overlayMetadataSaving = false;
                    disableOverlayTextInput();
                    overlayLibraryRevision = "";
                    if (statusText != null) statusText.setText("录像信息已保存");
                    if (detailText != null)
                        detailText.setText("名称、地图、车辆、操控模式与备注已更新 · 帧数据未改变");
                    reloadOverlayLibraryAsync();
                });
            } catch (Exception error) {
                handler.post(() -> {
                    overlayMetadataSaving = false;
                    disableOverlayTextInput();
                    if (statusText != null) statusText.setText("录像信息保存失败");
                    if (detailText != null) detailText.setText(error.getMessage());
                    refreshState();
                });
            }
        }, "a9tas-overlay-metadata-edit").start();
    }

    private void dispatch(String action) {
        if (TasForegroundService.ACTION_QUICK_RECORD.equals(action) && statusText != null) {
            statusText.setText("准备中");
            statusText.setTextColor(0xFFA9A7FF);
        }
        Intent intent = new Intent(service, TasForegroundService.class)
                .setAction(action)
                // Overlay actions originate over the live game surface.  Do not
                // relaunch the already-foreground game and wait a fixed second
                // before sending the requested pause/resume key.
                .putExtra(TasForegroundService.EXTRA_GAME_ALREADY_FOREGROUND, true);
        if (Build.VERSION.SDK_INT >= 26) service.startForegroundService(intent);
        else service.startService(intent);
        if (root != null) root.performHapticFeedback(
                android.view.HapticFeedbackConstants.KEYBOARD_TAP);
        handler.removeCallbacks(refresh);
        handler.postDelayed(refresh, 120L);
    }

    private void openMainActivity() {
        Intent intent = new Intent(service, MainActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK |
                        Intent.FLAG_ACTIVITY_SINGLE_TOP);
        service.startActivity(intent);
        showBubble();
    }

    private CheckBox checkBox(String label, String preference, boolean fallback) {
        CheckBox checkBox = new CheckBox(service);
        checkBox.setText(label);
        checkBox.setTextColor(0xFFE5E7EC);
        checkBox.setTextSize(12f);
        checkBox.setButtonTintList(new android.content.res.ColorStateList(
                new int[][] { new int[] { android.R.attr.state_checked }, new int[] {} },
                new int[] { 0xFF0A84FF, 0xFF777C87 }));
        checkBox.setChecked(preferences.getBoolean(preference, fallback));
        checkBox.setOnCheckedChangeListener((button, checked) ->
                preferences.edit().putBoolean(preference, checked).apply());
        LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(40));
        layout.topMargin = dp(3);
        checkBox.setLayoutParams(layout);
        return checkBox;
    }

    private void cycleReplaySpeed() {
        int current = preferences.getInt("replay_speed_factor", 1);
        int next = current == 1 ? 2 : current == 2 ? 4 : current == 4 ? 8 : 1;
        preferences.edit().putInt("replay_speed_factor", next).apply();
        if (speedButton != null) speedButton.setText("回放速度 · " + next + "×");
        if (root != null) root.performHapticFeedback(
                android.view.HapticFeedbackConstants.KEYBOARD_TAP);
    }

    private void cycleControlResponseMode() {
        boolean next = !preferences.getBoolean("control_low_latency", true);
        preferences.edit().putBoolean("control_low_latency", next)
                .putString("overlay_feedback", next ?
                        "已切换极速响应 · 更快识别暂停与 Retry" :
                        "已切换游戏流畅 · 降低后台轮询频率").apply();
        if (responseModeButton != null) {
            responseModeButton.setText(next ?
                    "响应模式 · 极速响应" : "响应模式 · 游戏流畅");
            viewFeedback(responseModeButton);
        }
        handler.removeCallbacks(refresh);
        handler.post(refresh);
    }

    private long overlayRefreshMillis() {
        return preferences.getBoolean("control_low_latency", true) ?
                250L : SMOOTH_REFRESH_MILLIS;
    }

    private void addSectionTitle(LinearLayout parent, String value) {
        TextView title = text(value, 12, 0xFFB4B8C2, true);
        LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        layout.topMargin = dp(16);
        parent.addView(title, layout);
    }

    private void addButton(LinearLayout parent, Button button, int topMargin) {
        LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(46));
        layout.topMargin = dp(topMargin);
        parent.addView(button, layout);
    }

    private Button button(String label, int color) {
        Button button = new Button(service);
        button.setText(label);
        button.setTextColor(Color.WHITE);
        button.setTextSize(13f);
        button.setAllCaps(false);
        button.setGravity(Gravity.CENTER);
        button.setPadding(dp(8), 0, dp(8), 0);
        button.setBackground(roundRect(color, 13));
        return button;
    }

    private TextView text(String value, int size, int color, boolean bold) {
        TextView text = new TextView(service);
        text.setText(value);
        text.setTextSize(size);
        text.setTextColor(color);
        if (bold) text.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        text.setGravity(Gravity.CENTER_VERTICAL);
        return text;
    }

    private GradientDrawable roundRect(int color, int radiusDp) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(dp(radiusDp));
        drawable.setStroke(dp(1), 0x553C4350);
        return drawable;
    }

    private int dp(int value) {
        return Math.round(value * service.getResources().getDisplayMetrics().density);
    }

    private final class DragTouchListener implements View.OnTouchListener {
        private final boolean tapExpands;
        private float downRawX;
        private float downRawY;
        private int downX;
        private int downY;
        private boolean moved;

        DragTouchListener(boolean tapExpands) { this.tapExpands = tapExpands; }

        @Override public boolean onTouch(View view, MotionEvent event) {
            if (params == null || root == null) return false;
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downRawX = event.getRawX();
                    downRawY = event.getRawY();
                    downX = params.x;
                    downY = params.y;
                    moved = false;
                    view.setPressed(true);
                    return true;
                case MotionEvent.ACTION_MOVE:
                    int dx = Math.round(event.getRawX() - downRawX);
                    int dy = Math.round(event.getRawY() - downRawY);
                    if (Math.abs(dx) > dp(8) || Math.abs(dy) > dp(8)) moved = true;
                    if (moved) {
                        int width = service.getResources().getDisplayMetrics().widthPixels;
                        int height = service.getResources().getDisplayMetrics().heightPixels;
                        params.x = Math.max(0, Math.min(downX + dx,
                                Math.max(0, width - Math.max(root.getWidth(), dp(58)))));
                        params.y = Math.max(dp(16), Math.min(downY + dy,
                                Math.max(dp(16), height - Math.max(root.getHeight(), dp(58)))));
                        windowManager.updateViewLayout(root, params);
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    view.setPressed(false);
                    if (!moved && event.getActionMasked() == MotionEvent.ACTION_UP) {
                        if (tapExpands) showPanel();
                        return true;
                    }
                    snapToEdge();
                    return true;
                default:
                    return false;
            }
        }
    }

    private void snapToEdge() {
        if (params == null || root == null) return;
        int width = service.getResources().getDisplayMetrics().widthPixels;
        int target = params.x + Math.max(root.getWidth(), dp(58)) / 2 < width / 2 ?
                dp(8) : Math.max(dp(8), width - Math.max(root.getWidth(), dp(58)) - dp(8));
        int start = params.x;
        ValueAnimator animator = ValueAnimator.ofInt(start, target);
        animator.setDuration(220L);
        animator.setInterpolator(new DecelerateInterpolator());
        animator.addUpdateListener(value -> {
            if (params == null || root == null) return;
            params.x = (Integer) value.getAnimatedValue();
            try { windowManager.updateViewLayout(root, params); }
            catch (IllegalArgumentException ignored) {}
        });
        animator.addListener(new android.animation.AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(android.animation.Animator animation) {
                if (params == null) return;
                preferences.edit().putInt("overlay_x", params.x)
                        .putInt("overlay_y", params.y).apply();
            }
        });
        animator.start();
    }
}
