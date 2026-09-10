package dev.a9tas.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.SystemClock;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Privacy-bounded, user-exportable diagnostics for devices without ADB. */
final class DiagnosticBundle {
    private static final int SCHEMA = 2;
    private static final long MAX_JOURNAL_BYTES = 256L * 1024L;
    private static final int MAX_TEXT_BYTES = 512 * 1024;
    private static final AtomicBoolean CRASH_HANDLER_INSTALLED = new AtomicBoolean(false);
    private static volatile Context applicationContext;

    static final class Created {
        final File file;
        final String sha256;
        Created(File file, String sha256) {
            this.file = file;
            this.sha256 = sha256;
        }
    }

    static final class Exported {
        final long bytes;
        final String sha256;
        Exported(long bytes, String sha256) {
            this.bytes = bytes;
            this.sha256 = sha256;
        }
    }

    private DiagnosticBundle() {}

    static void installCrashHandler(Context context) {
        applicationContext = context.getApplicationContext();
        if (!CRASH_HANDLER_INSTALLED.compareAndSet(false, true)) return;
        final Context app = applicationContext;
        final Thread.UncaughtExceptionHandler prior =
                Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            try {
                writeText(lastCrashFile(app), crashText(thread, error), false);
                recordFailure(app, "UNCAUGHT", error);
            } catch (Exception ignored) {}
            if (prior != null) prior.uncaughtException(thread, error);
        });
    }

    static String beginOperation(Context context, String operation) {
        String id = UUID.randomUUID().toString();
        String kind = bounded(operation, 128);
        String utc = UtcTimestamp.nowSeconds();
        context.getSharedPreferences("session", Context.MODE_PRIVATE).edit()
                .putString("diagnostic_operation_id", id)
                .putString("diagnostic_operation_kind", kind)
                .putString("diagnostic_operation_started_utc", utc).commit();
        try {
            JSONObject entry = baseEvent(context, "operation_begin");
            entry.put("operation", kind);
            appendJournal(context, entry.toString() + "\n");
        } catch (Exception ignored) {}
        return id;
    }

    static void recordState(Context context, String state, String detail) {
        try {
            JSONObject entry = baseEvent(context, "state");
            entry.put("state", bounded(state, 128));
            entry.put("detail", bounded(detail, 8192));
            appendJournal(context, entry.toString() + "\n");
        } catch (Exception ignored) {}
    }

    static String recordFailure(Context context, String operation, Throwable error) {
        String errorId = UUID.randomUUID().toString();
        try {
            JSONObject entry = baseEvent(context, "failure");
            entry.put("error_id", errorId);
            entry.put("operation", bounded(operation, 128));
            entry.put("exception", error == null ? "unknown" :
                    error.getClass().getName());
            entry.put("message", error == null ? "" :
                    bounded(error.getMessage(), 8192));
            entry.put("stack", error == null ? "" : bounded(stack(error), 32768));
            appendJournal(context, entry.toString() + "\n");
            context.getSharedPreferences("session", Context.MODE_PRIVATE).edit()
                    .putString("diagnostic_last_error_id", errorId)
                    .putString("diagnostic_last_error_operation", bounded(operation, 128))
                    .putString("diagnostic_last_error_utc", UtcTimestamp.nowSeconds()).commit();
        } catch (Exception ignored) {}
        return errorId;
    }

    static void recordRootReceipt(int exitCode, boolean timedOut,
                                  int outputLineCount, long durationMs) {
        recordRootReceipt(exitCode, timedOut, outputLineCount, durationMs, "root-script");
    }

    static void recordRootReceipt(int exitCode, boolean timedOut,
                                  int outputLineCount, long durationMs, String phase) {
        Context context = applicationContext;
        if (context == null) return;
        try {
            JSONObject entry = baseEvent(context, "root_receipt");
            entry.put("exit_code", exitCode);
            entry.put("timed_out", timedOut);
            entry.put("output_lines", outputLineCount);
            entry.put("duration_ms", durationMs);
            entry.put("phase", phase != null && phase.matches("[a-z0-9-]{1,80}")
                    ? phase : "root-script");
            appendJournal(context, entry.toString() + "\n");
        } catch (Exception ignored) {}
    }

    static void writeCompatibilityReport(Context context, JSONObject report)
            throws Exception {
        if (report == null) throw new IOException("compatibility report is unavailable");
        File target = compatibilityReportFile(context);
        writeText(target, report.toString(2), false);
        recordState(context, "COMPATIBILITY_REPORT_READY",
                "Read-only compatibility report created");
    }

    static Created create(Context context) throws Exception {
        File directory = diagnosticsCache(context);
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("unable to create diagnostics cache");
        removeExpiredExports(directory);
        String stamp = UtcTimestamp.nowSeconds().replace(":", "-");
        File output = new File(directory, "A9TAS-diagnostics-" + stamp + "-" +
                SystemClock.elapsedRealtime() + ".zip")
                .getCanonicalFile();
        if (!directory.getCanonicalFile().equals(output.getParentFile()))
            throw new IOException("invalid diagnostics output path");
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(output))) {
            addBytes(zip, "summary.json",
                    summary(context).toString(2).getBytes(StandardCharsets.UTF_8));
            addBytes(zip, "privacy.txt", (
                    "A9 TAS diagnostic bundle schema " + SCHEMA + "\n" +
                    "Included: app/device/runtime state, state history, and A9 TAS crash data.\n" +
                    "Excluded: license keys, accounts, recordings, game memory, screenshots, and other-app logs.\n")
                    .getBytes(StandardCharsets.UTF_8));
            addFileIfPresent(zip, "state-history.jsonl", journalFile(context));
            addFileIfPresent(zip, "state-history.previous.jsonl",
                    new File(diagnosticsFiles(context), "state-history.previous.jsonl"));
            addFileIfPresent(zip, "last-crash.txt", lastCrashFile(context));
            addFileIfPresent(zip, "compatibility-report.json",
                    compatibilityReportFile(context));
        }
        if (!output.isFile() || output.length() == 0)
            throw new IOException("diagnostic bundle was not created");
        return new Created(output, sha256(new FileInputStream(output)));
    }

    static Exported export(Context context, Created source, Uri destination)
            throws Exception {
        if (source == null || destination == null ||
                !"content".equals(destination.getScheme()))
            throw new IOException("diagnostic export destination is invalid");
        File root = diagnosticsCache(context).getCanonicalFile();
        File file = source.file.getCanonicalFile();
        if (!root.equals(file.getParentFile()) ||
                !file.getName().matches("A9TAS-diagnostics-[0-9TZ.-]+[.]zip") ||
                !file.isFile())
            throw new IOException("diagnostic source is outside the private cache");
        String current = sha256(new FileInputStream(file));
        if (!current.equals(source.sha256))
            throw new IOException("diagnostic bundle changed before export");
        long bytes = 0;
        try (ParcelFileDescriptor descriptor = context.getContentResolver()
                     .openFileDescriptor(destination, "rwt");
             InputStream input = new FileInputStream(file)) {
            if (descriptor == null) throw new IOException("document provider returned no file");
            try (FileOutputStream output = new FileOutputStream(descriptor.getFileDescriptor())) {
                byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (count == 0) continue;
                    output.write(buffer, 0, count);
                    bytes += count;
                }
                output.flush();
                output.getFD().sync();
            }
        }
        if (bytes != file.length()) throw new IOException("diagnostic export size mismatch");
        String exported;
        try (InputStream verification = context.getContentResolver()
                .openInputStream(destination)) {
            if (verification == null) throw new IOException("export cannot be verified");
            exported = sha256(verification);
        }
        if (!source.sha256.equals(exported))
            throw new IOException("exported diagnostic hash mismatch");
        return new Exported(bytes, exported);
    }

    private static JSONObject summary(Context context) throws Exception {
        JSONObject root = new JSONObject();
        root.put("schema", SCHEMA);
        root.put("generated_utc", UtcTimestamp.nowSeconds());
        PackageInfo info = context.getPackageManager().getPackageInfo(
                context.getPackageName(), 0);
        JSONObject app = new JSONObject();
        app.put("package", context.getPackageName());
        app.put("version_code", info.versionCode);
        app.put("version_name", info.versionName == null ? "" : info.versionName);
        // Repeated maintenance APKs can share a versionCode. Hash only our APK
        // at export time so a performance report identifies the installed build.
        try {
            app.put("apk_sha256", sha256(new FileInputStream(
                    context.getApplicationInfo().sourceDir)));
        } catch (Exception unavailable) {
            app.put("apk_sha256", "unavailable");
        }
        app.put("pid", Process.myPid());
        root.put("app", app);
        JSONObject device = new JSONObject();
        device.put("sdk", Build.VERSION.SDK_INT);
        device.put("release", Build.VERSION.RELEASE == null ? "" : Build.VERSION.RELEASE);
        device.put("manufacturer", bounded(Build.MANUFACTURER, 128));
        device.put("model", bounded(Build.MODEL, 128));
        device.put("device", bounded(Build.DEVICE, 128));
        device.put("hardware", bounded(Build.HARDWARE, 128));
        device.put("primary_abi", Build.SUPPORTED_ABIS.length == 0 ? "" :
                Build.SUPPORTED_ABIS[0]);
        JSONArray abis = new JSONArray();
        for (String abi : Build.SUPPORTED_ABIS) abis.put(abi);
        device.put("supported_abis", abis);
        root.put("device", device);

        SharedPreferences preferences = context.getSharedPreferences(
                "session", Context.MODE_PRIVATE);
        JSONObject session = new JSONObject();
        Map<String, ?> all = preferences.getAll();
        for (String key : SESSION_KEYS) {
            Object value = all.get(key);
            if (value instanceof String || value instanceof Number ||
                    value instanceof Boolean)
                session.put(key, value);
        }
        root.put("session", session);
        root.put("elapsed_realtime_ms", SystemClock.elapsedRealtime());
        return root;
    }

    private static final String[] SESSION_KEYS = {
            "state", "detail", "updated", "operation_kind", "operation_active",
            "operation_ticks", "operation_limit", "cancel_pending",
            "prepared_ready", "prepared_pid", "prepared_start_ticks",
            "prepared_process", "prepared_package", "prepared_profile_id",
            "prepared_native_sha", "prepared_host_machine", "prepared_native_bridge",
            "prepared_bridge_set", "prepared_libc_sha", "prepared_runtime_backend",
            "prepared_experimental_bypass", "experimental_identity_bypass",
            "session_hooks_installed", "brush_archived_record_ready",
            "brush_session_enabled", "auto_retry_record", "record_pause_interrupt",
            "continuous_load_selected", "record_waiting_retry", "replay_speed_factor",
                "replay_pause_at_target", "branch_pending", "branch_replay_ready",
                "branch_runtime_prearmed", "resident_retry_record_armed",
                "resident_retry_auto_loop",
            "branch_auto_handoff", "branch_manual_resume", "branch_resume_delay_seconds",
            "latest_recording_ticks", "latest_recording_sha", "latest_recording_title",
            "latest_recording_map", "latest_recording_car",
            "latest_recording_control_mode", "latest_recording_game_version",
            "latest_recording_package", "latest_recording_native_sha",
            "latest_recording_build_id", "latest_recording_profile_sha",
            "last_replay_ticks", "last_replay_fixed_delta_us", "last_replay_recording_sha",
            "selected_archive_title", "replay_target_tick", "startup_selftest",
            "practice_proof_profile", "apk_version_code_seen",
            "diagnostic_operation_id", "diagnostic_operation_kind",
            "diagnostic_operation_started_utc", "diagnostic_last_error_id",
            "diagnostic_last_error_operation", "diagnostic_last_error_utc",
            "compatibility_last_utc", "compatibility_candidate_count",
            "compatibility_supported_count", "compatibility_experimental_candidate_count"
    };

    private static JSONObject baseEvent(Context context, String kind) throws Exception {
        JSONObject entry = new JSONObject();
        entry.put("utc", UtcTimestamp.nowSeconds());
        entry.put("elapsed_ms", SystemClock.elapsedRealtime());
        entry.put("pid", Process.myPid());
        entry.put("thread", bounded(Thread.currentThread().getName(), 128));
        entry.put("kind", kind);
        SharedPreferences preferences = context.getSharedPreferences(
                "session", Context.MODE_PRIVATE);
        entry.put("operation_id", bounded(preferences.getString(
                "diagnostic_operation_id", ""), 64));
        entry.put("operation_kind", bounded(preferences.getString(
                "diagnostic_operation_kind", ""), 128));
        return entry;
    }

    private static synchronized void appendJournal(Context context, String text)
            throws Exception {
        File file = journalFile(context);
        File directory = file.getParentFile();
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("unable to create diagnostics directory");
        if (file.isFile() && file.length() > MAX_JOURNAL_BYTES) {
            File old = new File(directory, "state-history.previous.jsonl");
            if (old.exists() && !old.delete()) throw new IOException("unable to rotate diagnostics");
            if (!file.renameTo(old)) throw new IOException("unable to rotate diagnostics");
        }
        writeText(file, text, true);
    }

    private static void addFileIfPresent(ZipOutputStream zip, String name, File file)
            throws Exception {
        if (!file.isFile()) return;
        try (InputStream input = new FileInputStream(file)) {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] buffer = new byte[16 * 1024];
            int count;
            int total = 0;
            while ((count = input.read(buffer)) != -1 && total < MAX_TEXT_BYTES) {
                int accepted = Math.min(count, MAX_TEXT_BYTES - total);
                bytes.write(buffer, 0, accepted);
                total += accepted;
            }
            addBytes(zip, name, bytes.toByteArray());
        }
    }

    private static void addBytes(ZipOutputStream zip, String name, byte[] data)
            throws Exception {
        ZipEntry entry = new ZipEntry(name);
        entry.setTime(0L);
        zip.putNextEntry(entry);
        zip.write(data);
        zip.closeEntry();
    }

    private static void removeExpiredExports(File directory) {
        File[] files = directory.listFiles();
        if (files == null) return;
        long cutoff = System.currentTimeMillis() - 7L * 24L * 60L * 60L * 1000L;
        for (File file : files)
            if (file.isFile() &&
                    file.getName().matches("A9TAS-diagnostics-[0-9TZ.-]+[.]zip") &&
                    file.lastModified() > 0L && file.lastModified() < cutoff)
                file.delete();
    }

    private static void writeText(File file, String text, boolean append) throws Exception {
        File directory = file.getParentFile();
        if (directory != null && !directory.isDirectory() && !directory.mkdirs())
            throw new IOException("unable to create diagnostics directory");
        try (FileOutputStream output = new FileOutputStream(file, append)) {
            output.write((text == null ? "" : text).getBytes(StandardCharsets.UTF_8));
            output.flush();
        }
    }

    private static String crashText(Thread thread, Throwable error) {
        return "utc=" + UtcTimestamp.nowSeconds() + "\n" +
                "thread=" + (thread == null ? "unknown" : thread.getName()) + "\n" +
                stack(error);
    }

    private static String stack(Throwable error) {
        if (error == null) return "";
        StringWriter text = new StringWriter();
        error.printStackTrace(new PrintWriter(text));
        return text.toString();
    }

    private static String bounded(String value, int limit) {
        if (value == null) return "";
        String clean = value.replace('\u0000', '?');
        return clean.length() <= limit ? clean : clean.substring(0, limit);
    }

    private static File diagnosticsFiles(Context context) {
        return new File(context.getFilesDir(), "diagnostics");
    }

    private static File diagnosticsCache(Context context) {
        return new File(context.getCacheDir(), "diagnostics-export");
    }

    private static File journalFile(Context context) {
        return new File(diagnosticsFiles(context), "state-history.jsonl");
    }

    private static File lastCrashFile(Context context) {
        return new File(diagnosticsFiles(context), "last-crash.txt");
    }

    private static File compatibilityReportFile(Context context) {
        return new File(diagnosticsFiles(context), "compatibility-report.json");
    }

    private static String sha256(InputStream input) throws Exception {
        try (InputStream stream = input) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = stream.read(buffer)) != -1)
                if (count != 0) digest.update(buffer, 0, count);
            StringBuilder output = new StringBuilder(64);
            for (byte value : digest.digest())
                output.append(String.format(Locale.ROOT, "%02x", value & 0xff));
            return output.toString();
        }
    }
}
