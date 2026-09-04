package dev.a9tas.android;

import android.content.Context;
import android.content.pm.PackageInfo;

/** Product identity for the short-lived, debug-signed compatibility build. */
final class DeveloperBuild {
    private static final String VERSION_SUFFIX = "-devtest";
    static final long EXPIRES_UTC_SECONDS = 1793491200L; // 2026-11-01 00:00 UTC

    private DeveloperBuild() {}

    static boolean enabled(Context context) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(
                    context.getPackageName(), 0);
            String version = info.versionName == null ? "" : info.versionName;
            long now = System.currentTimeMillis() / 1000L;
            return version.endsWith(VERSION_SUFFIX) && now <= EXPIRES_UTC_SECONDS;
        } catch (Exception ignored) {
            return false;
        }
    }
}
