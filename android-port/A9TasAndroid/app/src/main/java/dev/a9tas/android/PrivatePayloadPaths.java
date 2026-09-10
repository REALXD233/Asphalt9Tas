package dev.a9tas.android;

/** Resolve app storage without assuming the device's storage volume or Android user. */
final class PrivatePayloadPaths {
    private PrivatePayloadPaths() {}

    static String resolveScript(int pid, String pkg, String reportedDataDir, int reportedUid) {
        StringBuilder s = new StringBuilder("user=$((uid / 100000)); logical_app_data=; stage_app_data=;")
                .append("a9tas_data_dir(){ [ -d \"$1\" ]; };")
                .append("a9tas_pick_data(){ [ -z \"$stage_app_data\" ] || return 0; ")
                .append("case \"$1\" in /*) ;; *) return 0;; esac; ")
                .append("if a9tas_data_dir \"$1\"; then logical_app_data=$1; stage_app_data=$1; ")
                .append("elif a9tas_data_dir /proc/").append(pid).append("/root\"$1\"; then ")
                .append("logical_app_data=$1; stage_app_data=/proc/").append(pid)
                .append("/root\"$1\"; fi; return 0; };");
        // ApplicationInfo knows adopted storage and vendor-specific data paths.
        // Do not use the calling user's dataDir for another Android user's process.
        if (reportedDataDir != null && reportedDataDir.startsWith("/") && reportedUid >= 0) {
            s.append("if [ \"$uid\" = ").append(reportedUid).append(" ]; then a9tas_pick_data ")
                    .append("'").append(reportedDataDir.replace("'", "'\"'\"'"))
                    .append("'; fi;");
        }
        s.append("a9tas_pick_data /data/user/$user/").append(pkg).append(';')
                .append("if [ \"$user\" = 0 ]; then a9tas_pick_data /data/data/").append(pkg).append("; fi;")
                // Android's backing CE mirror can remain visible to root when
                // app-data isolation hides /data/user. It is a durable path,
                // unlike /proc/<old pid>/root, which dies at force-stop.
                .append("a9tas_pick_data /data_mirror/data_ce/null/$user/").append(pkg).append(';')
                .append("if [ -z \"$stage_app_data\" ]; then ")
                .append("echo 'G10_ERROR private_data_unavailable: no visible app data directory' ")
                .append("\"uid=$uid user=$user\"; exit 73; fi;")
                .append("echo G10_STAGE private_data_resolved \"logical=$logical_app_data staging=$stage_app_data\";");
        return s.toString();
    }
}
