package dev.a9tas.android;

import android.content.Context;
import android.os.Build;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.List;

/** Read-only environment preflight. It never installs a hook or writes game state. */
final class CompatibilityReport {
    private static final String PROBE_SCRIPT =
            "u=$(id -u 2>/dev/null || echo unavailable); " +
            "printf 'A9COMPAT_V2\\troot_uid\\t%s\\n' \"$u\"; " +
            "for k in ro.build.version.sdk ro.build.version.release ro.product.cpu.abi " +
            "ro.product.cpu.abilist ro.hardware ro.build.type; do " +
            "v=$(getprop \"$k\" 2>/dev/null); " +
            "printf 'A9COMPAT_V2\\tprop.%s\\t%s\\n' \"$k\" \"$v\"; done; " +
            "e=$(getenforce 2>/dev/null || echo unavailable); " +
            "printf 'A9COMPAT_V2\\tselinux\\t%s\\n' \"$e\"; " +
            "for t in sh sed grep tr head readlink od cp chmod rm; do " +
            "if command -v \"$t\" >/dev/null 2>&1; then v=1; else v=0; fi; " +
            "printf 'A9COMPAT_V2\\ttool.%s\\t%s\\n' \"$t\" \"$v\"; done";

    static final class Result {
        final int candidates;
        final int supported;
        final boolean rootAvailable;
        final String summary;

        Result(int candidates, int supported, boolean rootAvailable, String summary) {
            this.candidates = candidates;
            this.supported = supported;
            this.rootAvailable = rootAvailable;
            this.summary = summary;
        }
    }

    private CompatibilityReport() {}

    static Result run(Context context, BuildProfileRegistry profiles,
                      ArtifactRegistry artifacts) throws Exception {
        JSONObject report = new JSONObject();
        report.put("schema", 2);
        report.put("generated_utc", UtcTimestamp.nowSeconds());
        report.put("read_only", true);
        report.put("preflight_installs_hooks", false);
        report.put("preflight_game_state_writes", 0);

        JSONObject app = new JSONObject();
        app.put("package", context.getPackageName());
        app.put("sdk", Build.VERSION.SDK_INT);
        app.put("primary_abi", Build.SUPPORTED_ABIS.length == 0 ? "" :
                Build.SUPPORTED_ABIS[0]);
        JSONArray abis = new JSONArray();
        for (String abi : Build.SUPPORTED_ABIS) abis.put(abi);
        app.put("supported_abis", abis);
        report.put("app_environment", app);

        JSONObject root = new JSONObject();
        boolean rootAvailable = false;
        try {
            RootShell.Result receipt = RootShell.runFixedScript(PROBE_SCRIPT, 15L);
            root.put("exit_code", receipt.exitCode);
            root.put("timed_out", receipt.timedOut);
            for (String line : receipt.output) {
                String[] fields = line.split("\\t", 3);
                if (fields.length != 3 || !"A9COMPAT_V2".equals(fields[0]) ||
                        !fields[1].matches("[A-Za-z0-9._-]{1,96}")) continue;
                root.put(fields[1], bounded(fields[2], 512));
            }
            rootAvailable = receipt.ok() && "0".equals(root.optString("root_uid"));
        } catch (Exception error) {
            root.put("error", bounded(error.getMessage(), 1024));
        }
        root.put("available", rootAvailable);
        report.put("root_probe", root);

        JSONArray candidateArray = new JSONArray();
        int supported = 0;
        int experimentalRunnable = 0;
        String scanError = "";
        if (rootAvailable) {
            try {
                List<GameProcessScanner.Candidate> candidates = GameProcessScanner.scan(
                        context, profiles, artifacts);
                for (GameProcessScanner.Candidate candidate : candidates) {
                    JSONObject item = new JSONObject();
                    item.put("package", bounded(candidate.packageName, 256));
                    item.put("process", bounded(candidate.processName, 256));
                    item.put("pid", candidate.pid);
                    item.put("start_ticks", candidate.startTicks);
                    item.put("host_machine", candidate.hostMachine);
                    item.put("native_bridge", candidate.nativeBridge);
                    item.put("bridge_set", candidate.bridgeSet);
                    item.put("native_sha256", candidate.nativeIdentityAvailable() ?
                            candidate.nativeSha256 : "unavailable");
                    item.put("libc_sha256", candidate.libcIdentityAvailable() ?
                            candidate.libcSha256 : "unavailable");
                    item.put("profile_id", candidate.profile == null ? "" :
                            candidate.profile.id);
                    item.put("backend_id", candidate.backend == null ? "" :
                            candidate.backend.id);
                    ArtifactRegistry.Backend fallback = artifacts.findExperimental(
                            candidate.hostMachine, candidate.bridgeSet);
                    // An unknown native hash intentionally has no exact profile
                    // on the Candidate.  It is still runnable through the
                    // user-selected compatibility profile when this ABI/backend
                    // exists.  Requiring candidate.profile here made the report
                    // incorrectly claim that unknown channel builds had no
                    // experimental route even though MainActivity could run them.
                    boolean experimental = candidate.startTicks > 0 &&
                            profiles.size() > 0 && fallback != null;
                    item.put("experimental_backend_id", fallback == null ? "" : fallback.id);
                    item.put("experimental_runtime_candidate", experimental);
                    item.put("supported", candidate.supported());
                    item.put("receipt", candidate.compatibilityReceipt());
                    if (candidate.supported()) supported++;
                    if (experimental) experimentalRunnable++;
                    candidateArray.put(item);
                }
            } catch (Exception error) {
                scanError = bounded(error.getMessage(), 2048);
            }
        }
        report.put("game_candidates", candidateArray);
        report.put("candidate_count", candidateArray.length());
        report.put("supported_count", supported);
        report.put("experimental_runtime_candidate_count", experimentalRunnable);
        if (!scanError.isEmpty()) report.put("scan_error", scanError);
        DiagnosticBundle.writeCompatibilityReport(context, report);
        context.getSharedPreferences("session", Context.MODE_PRIVATE).edit()
                .putString("compatibility_last_utc", UtcTimestamp.nowSeconds())
                .putInt("compatibility_candidate_count", candidateArray.length())
                .putInt("compatibility_supported_count", supported)
                .putInt("compatibility_experimental_candidate_count",
                        experimentalRunnable).commit();

        String summary = !rootAvailable ? "Root 不可用" :
                scanError.isEmpty() ? "发现 " + candidateArray.length() +
                        " 个游戏进程；精确兼容 " + supported +
                        " 个；实验运行链候选 " + experimentalRunnable + " 个" :
                        "Root 可用，但游戏扫描失败：" + scanError;
        return new Result(candidateArray.length(), supported, rootAvailable, summary);
    }

    private static String bounded(String value, int limit) {
        if (value == null) return "";
        String clean = value.replace('\u0000', '?');
        return clean.length() <= limit ? clean : clean.substring(0, limit);
    }
}
