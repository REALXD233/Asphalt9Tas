package dev.a9tas.android;

import android.content.Context;
import android.os.Process;
import java.io.*;
import java.util.*;

/** Non-recursive fallback for ROMs without ExternalStorageProvider. */
final class SharedRecordingBrowser {
    static boolean allowed(String path) {
        if (path == null || path.indexOf('\n') >= 0 || path.indexOf('\r') >= 0 ||
                path.indexOf('\0') >= 0) return false;
        for (String dir : new String[]{"/sdcard/Documents/", "/sdcard/Download/"}) {
            if (path.startsWith(dir)) {
                String name = path.substring(dir.length());
                return !name.contains("/") && name.toLowerCase(Locale.ROOT).endsWith(".a9tas");
            }
        }
        return false;
    }
    static String quote(String path) { return "'" + path.replace("'", "'\\''") + "'"; }
    static List<String> list() throws Exception {
        RootShell.Result result = RootShell.runFixedScript(
                "for d in /sdcard/Documents /sdcard/Download; do " +
                "for f in \"$d\"/*; do [ -f \"$f\" ] || continue; " +
                "case \"$f\" in *.[aA]9[tT][aA][sS]) printf '%s\\n' \"$f\";; esac; done; done", 15L,
                "recording-browse");
        if (!result.ok()) throw new IOException("无法读取共享目录，请确认已授予 Root");
        List<String> files = new ArrayList<>();
        for (String line : result.output) {
            if (allowed(line) && files.size() < 200 && !files.contains(line)) files.add(line);
        }
        Collections.sort(files);
        return files;
    }
    static A9TasLibrary.Entry importFile(Context context, String path) throws Exception {
        if (!allowed(path)) throw new IOException("仅支持 Documents／Download 中的 .a9tas 文件");
        File stage = File.createTempFile("shared-import-", ".pending", context.getCacheDir());
        String from = quote(path), to = quote(stage.getCanonicalPath());
        try {
            RootShell.Result result = RootShell.runFixedScript(
                    "set -eu; [ -f " + from + " ]; n=$(wc -c < " + from + "); " +
                    "[ \"$n\" -gt 0 ]; [ \"$n\" -le 536936608 ]; " +
                    "cp " + from + " " + to + "; chown " + Process.myUid() + ":" +
                    Process.myUid() + " " + to + "; chmod 0600 " + to, 60L, "recording-import-copy");
            if (!result.ok()) throw new IOException("读取存档失败：请检查文件大小及 Root 权限");
            return RecordingImporter.importStream(context, new FileInputStream(stage));
        } finally { stage.delete(); }
    }
}
