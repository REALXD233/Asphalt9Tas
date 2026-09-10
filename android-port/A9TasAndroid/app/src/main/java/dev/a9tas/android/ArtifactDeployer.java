package dev.a9tas.android;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** Product port of the already-proven G4 PrepareProcess stage. */
final class ArtifactDeployer {
    private static final String PROFILE_DEVICE_NAME =
            "a9tas_g8_runtime_build_profile_v1.bin";
    static final class Receipt {
        final int pid;
        final long startTicks;
        final String raw;

        Receipt(int pid, long startTicks, String raw) {
            this.pid = pid;
            this.startTicks = startTicks;
            this.raw = raw;
        }
    }

    private ArtifactDeployer() {}

    static Receipt prepare(Context context, GameProcessScanner.Candidate selected,
                           BuildProfileRegistry.Profile profile,
                           boolean experimental) throws Exception {
        requireSafe(selected, profile, experimental);
        ArtifactRegistry registry = ArtifactRegistry.loadAndVerify(context);
        ArtifactRegistry.Backend backend = experimental ?
                registry.findAnyById(selected.backend.id) :
                registry.findById(selected.backend.id);
        if (backend == null || !backend.id.equals(selected.backend.id) ||
                !backend.hostMachine.equals(selected.hostMachine) ||
                !backend.bridgeSet.equals(selected.bridgeSet) ||
                !experimental && !backend.matches(selected.hostMachine, selected.bridgeSet,
                        selected.libcSha256))
            throw new IOException("runtime backend identity changed");
        ArtifactRegistry.IdentityHelper helper = registry.identityHelperFor(backend.hostMachine);
        if (helper == null) throw new IOException("identity helper unavailable for runtime");
        File stage = new File(context.getFilesDir(), "fixed-artifacts/" + backend.id);
        if (!stage.isDirectory() && !stage.mkdirs()) throw new IOException("stage directory failed");

        Map<String, File> files = new HashMap<>();
        for (ArtifactRegistry.Artifact artifact : backend.artifacts) {
            if (PROFILE_DEVICE_NAME.equals(artifact.deviceName))
                throw new IOException("BuildProfile must not be fixed in the common runtime set");
            files.put(artifact.deviceName, stageVerifiedAsset(context, stage,
                    artifact.asset, artifact.deviceName, artifact.sha256));
        }
        File selectedProfile = stageVerifiedProfile(context, stage,
                profile, PROFILE_DEVICE_NAME);
        File selectedHelper = stageVerifiedAsset(context, stage,
                helper.asset, helper.deviceName, helper.sha256);

        Intent launch = context.getPackageManager().getLaunchIntentForPackage(selected.packageName);
        ComponentName component = launch == null ? null : launch.getComponent();
        if (component == null) throw new IOException("launcher activity unavailable");
        String flattened = component.flattenToString();
        if (!flattened.matches("[A-Za-z0-9_.]+/[A-Za-z0-9_.$]+"))
            throw new IOException("unsafe launcher component");

        String helperTarget = "/data/local/tmp/" + helper.deviceName;
        StringBuilder script = new StringBuilder("set -eu;")
                .append("cp ").append(quote(selectedHelper.getAbsolutePath())).append(' ')
                .append(quote(helperTarget)).append(';')
                .append("chmod 0700 ").append(quote(helperTarget)).append(';')
                .append(quote(helperTarget)).append(" --expect ")
                .append(helper.sha256).append(' ').append(quote(helperTarget)).append(';');
        for (ArtifactRegistry.Artifact artifact : backend.artifacts) {
            File source = files.get(artifact.deviceName);
            String target = "/data/local/tmp/" + artifact.deviceName;
            script.append("echo G10_STAGE copy_").append(artifact.deviceName).append(';')
                    .append("cp ").append(quote(source.getAbsolutePath())).append(' ')
                    .append(quote(target)).append(';')
                    .append("chmod ").append(artifact.mode).append(' ')
                    .append(quote(target)).append(';')
                    .append(quote(helperTarget)).append(" --expect ")
                    .append(artifact.sha256).append(' ').append(quote(target)).append(';');
        }
        String profileTarget = "/data/local/tmp/" + PROFILE_DEVICE_NAME;
        script.append("echo G10_STAGE copy_selected_build_profile;")
                .append("cp ").append(quote(selectedProfile.getAbsolutePath())).append(' ')
                .append(quote(profileTarget)).append(';')
                .append("chmod 0644 ").append(quote(profileTarget)).append(';')
                .append(quote(helperTarget)).append(" --expect ")
                .append(profile.profileSha256).append(' ').append(quote(profileTarget)).append(';');
        String process = selected.processName;
        String pkg = selected.packageName;
        String carrier = "/data/local/tmp/" + backend.carrierDeviceName;
        String log = "/data/local/tmp/a9tas-g10-early-carrier.log";
        script.append("a9tas_find_pids(){ for d in /proc/[0-9]*; do ")
                .append("candidate=${d#/proc/}; case \"$candidate\" in ''|*[!0-9]*) continue;; esac; ")
                .append("c=$(tr '\\000' '\\n' <\"$d/cmdline\" 2>/dev/null | head -n1); ")
                .append("[ \"$c\" = ").append(quote(process))
                .append(" ] && printf '%s\\n' \"${d#/proc/}\"; done; };");
        boolean privatePayload = "target_app_cache".equals(backend.payloadStaging);
        if (privatePayload) {
            String localPayload = "/data/local/tmp/" + backend.payloadDeviceName;
            String reportedDataDir = null;
            int reportedUid = -1;
            try {
                android.content.pm.ApplicationInfo info = context.getPackageManager()
                        .getApplicationInfo(pkg, 0);
                reportedDataDir = info.dataDir;
                reportedUid = info.uid;
            } catch (android.content.pm.PackageManager.NameNotFoundException ignored) {
                // Root-discovered games may not be visible to PackageManager.
            }
            script.append("echo G10_STAGE target_private_payload;")
                    .append("[ -r /proc/").append(selected.pid).append("/status ];")
                    .append("old_st=$(sed 's/^[^)]*) //' /proc/").append(selected.pid)
                    .append("/stat); set -- $old_st; old_s=${20}; [ \"$old_s\" = ")
                    .append(selected.startTicks).append(" ];")
                    .append("uid=; while read key first rest; do case \"$key\" in Uid:) uid=$first; break;; esac; done </proc/")
                    .append(selected.pid).append("/status; case \"$uid\" in ''|*[!0-9]*) exit 73;; esac;")
                    .append("echo G10_STAGE private_identity_bound;")
                    .append(PrivatePayloadPaths.resolveScript(selected.pid, pkg, reportedDataDir, reportedUid))
                    .append("stage_app_cache=\"$stage_app_data/cache\"; ")
                    .append("logical_app_cache=\"$logical_app_data/cache\"; ")
                    .append("mkdir -p \"$stage_app_cache\" || { echo G10_ERROR private_cache_mkdir_failed; exit 73; };")
                    .append("chown $uid:$uid \"$stage_app_cache\" || { echo G10_ERROR private_cache_chown_failed; exit 73; };")
                    .append("chmod 0771 \"$stage_app_cache\" || { echo G10_ERROR private_cache_chmod_failed; exit 73; };")
                    .append("echo G10_STAGE private_cache_ready;")
                    .append("stage_payload_dir=\"$stage_app_cache/a9tas\"; ")
                    .append("stage_payload_target=\"$stage_payload_dir/")
                    .append(backend.payloadDeviceName).append("\"; ")
                    .append("payload_target=\"$logical_app_cache/a9tas/")
                    .append(backend.payloadDeviceName).append("\";")
                    .append("payload_pending=\"$stage_payload_target.pending\";")
                    .append("mkdir -p \"$stage_payload_dir\"; chown $uid:$uid \"$stage_payload_dir\";")
                    .append("chmod 0700 \"$stage_payload_dir\";")
                    .append("echo G10_STAGE private_payload_dir_ready;")
                    // The previous payload can still be executable-mapped by
                    // the selected process.  Truncating that inode with cp
                    // fails with ETXTBSY on native ARM64 Android.  Verify a
                    // same-directory replacement first and atomically rename
                    // it over the pathname; the old mapping remains valid
                    // until the subsequent force-stop while the new process
                    // receives the new inode.
                    .append("rm -f \"$payload_pending\";")
                    .append("cp ").append(quote(localPayload)).append(" \"$payload_pending\";")
                    .append("chown $uid:$uid \"$payload_pending\"; chmod 0500 \"$payload_pending\";")
                    .append("echo G10_STAGE private_payload_pending_ready;")
                    .append(quote(helperTarget)).append(" --expect ")
                    .append(findArtifact(backend, backend.payloadDeviceName).sha256)
                    .append(" \"$payload_pending\";")
                    .append("mv -f \"$payload_pending\" \"$stage_payload_target\";")
                    .append("chown $uid:$uid \"$stage_payload_target\"; chmod 0500 \"$stage_payload_target\";")
                    .append("echo G10_STAGE private_payload_published;")
                    // Some rooted Android 7 ROMs do not expose restorecon in the
                    // root shell PATH.  It is an optimization for assigning the
                    // expected app-data label, not an identity gate: the carrier
                    // receipt below is the authoritative proof that the target
                    // process could actually map the payload.  Try it when it is
                    // present and let the carrier decide compatibility otherwise.
                    .append("if command -v restorecon >/dev/null 2>&1; then ")
                    .append("restorecon -RFD \"$stage_payload_dir\" >/dev/null 2>&1 || true; fi;")
                    .append(quote(helperTarget)).append(" --expect ")
                    .append(findArtifact(backend, backend.payloadDeviceName).sha256)
                    .append(" \"$stage_payload_target\";");
        }
        script.append("echo G10_STAGE artifacts_verified;")
                .append("echo G10_STAGE force_stop;")
                .append("am force-stop ").append(quote(pkg)).append(" >/dev/null;")
                .append("i=0; while [ -n \"$(a9tas_find_pids)\" ] && [ $i -lt 20 ]; ")
                .append("do sleep 1; i=$((i+1)); done;")
                .append("[ -z \"$(a9tas_find_pids)\" ];")
                .append("echo G10_STAGE old_process_stopped;")
                // These are host receipts or derived staging files belonging
                // to processes that have just been proven stopped. Fixed
                // runtime artifacts and the app-private recording library do
                // not match these explicit prefixes.
                .append("rm -f /data/local/tmp/a9tas-g10-record-* ")
                .append("/data/local/tmp/a9tas-g10-replay-session-* ")
                .append("/data/local/tmp/a9tas-g10-replay-*.a9g4r2 ")
                .append("/data/local/tmp/a9tas-g10-session-*;")
                .append("echo G10_STAGE stale_session_files_cleaned;")
                // ActivityManager removes the old task asynchronously after
                // force-stop. Starting in the same millisecond can create a
                // ProcessRecord that the pending remove-task work kills.
                .append("sleep 1; echo G10_STAGE old_task_settled;")
                .append("rm -f ").append(quote(log)).append(" /data/local/tmp/a9tas-g4-install-proven-begin-v1;")
                .append(": >").append(quote(log)).append("; chmod 0644 ").append(quote(log)).append(';')
                .append("echo G10_STAGE arm_carrier;")
                // The prepare shell remains alive until the carrier receipt is
                // consumed, so nohup is unnecessary and is not available on a
                // number of otherwise compatible Android/toolbox builds.
                .append("( ").append(quote(carrier)).append(' ').append(quote(process))
                .append(privatePayload ? " \"$payload_target\"" : "")
                .append(" >>").append(quote(log)).append(" 2>&1 </dev/null ) &")
                .append("echo G10_STAGE start_activity;")
                .append("am start -n ").append(quote(flattened)).append(" >/dev/null;")
                .append("echo G10_STAGE wait_carrier;")
                .append("i=0; while ! grep -Eq '").append(backend.carrierReceiptPrefix)
                .append(" passed=[01]' ")
                .append(quote(log)).append(" 2>/dev/null && [ $i -lt 120 ]; do sleep 1; i=$((i+1)); done;")
                .append("cat ").append(quote(log)).append(';')
                .append("grep -q '").append(backend.carrierReceiptPrefix)
                .append(" passed=1' ").append(quote(log)).append(';')
                .append("echo G10_STAGE identity_receipt;")
                // The carrier receipt is the authoritative process identity.  Rescanning
                // /proc here used to create a race on Android 7: a short-lived process or
                // a disappearing /proc directory could turn an already successful load
                // into a false Prepare failure.  Bind every post-load check to the exact
                // pid/start_ticks emitted after the carrier has detached instead.
                .append("receipt_pid=; receipt_start=; ")
                .append("while read receipt_prefix receipt_rest; do ")
                .append("[ \"$receipt_prefix\" = '").append(backend.carrierReceiptPrefix)
                .append("' ] || continue; receipt_pass=; candidate_pid=; candidate_start=; ")
                .append("for receipt_field in $receipt_rest; do case \"$receipt_field\" in ")
                .append("passed=*) receipt_pass=${receipt_field#passed=};; ")
                .append("pid=*) candidate_pid=${receipt_field#pid=};; ")
                .append("start_ticks=*) candidate_start=${receipt_field#start_ticks=};; esac; done; ")
                .append("if [ \"$receipt_pass\" = 1 ]; then receipt_pid=$candidate_pid; ")
                .append("receipt_start=$candidate_start; fi; done <").append(quote(log)).append(';')
                .append("case \"$receipt_pid\" in ''|*[!0-9]*) exit 74;; esac; ")
                .append("case \"$receipt_start\" in ''|*[!0-9]*) exit 75;; esac; p=$receipt_pid;")
                .append("a9tas_identity_ready(){ ")
                .append("[ -r /proc/$p/status ] && [ -r /proc/$p/maps ] && ")
                .append("current=$(tr '\\000' '\\n' </proc/$p/cmdline 2>/dev/null | sed -n '1p') && ")
                .append("[ \"$current\" = ").append(quote(process)).append(" ] && ")
                .append("grep -q '^TracerPid:[[:space:]]*0$' /proc/$p/status && ")
                .append("grep -q '/").append(backend.payloadDeviceName)
                .append("' /proc/$p/maps && ")
                .append(backend.bootstrapDeviceName == null ? "" :
                        "grep -q '/data/local/tmp/" + backend.bootstrapDeviceName +
                                "' /proc/$p/maps && ")
                .append("lib_line=$(grep -m1 'libAsphalt9[.]so' /proc/$p/maps) && ")
                .append("lib=${lib_line#* /} && [ -n \"$lib\" ] && [ \"$lib\" != \"$lib_line\" ] && ")
                .append("st=$(sed 's/^[^)]*) //' /proc/$p/stat) && set -- $st && ")
                .append("s=${20} && [ \"$s\" = \"$receipt_start\" ]; };")
                .append("i=0; until a9tas_identity_ready; do i=$((i+1)); ")
                .append("[ $i -lt 10 ] || exit 76; sleep 1; done;")
                .append("echo G10_STAGE identity_bound;");
        if (!experimental) script.append(quote(helperTarget)).append(" --expect ")
                .append(profile.nativeSha256).append(" \"$lib\";");
        script.append("echo G10_PREPARE passed=1 pid=$p start_ticks=$s native_sha256=")
                .append(profile.nativeSha256).append(" profile_id=").append(profile.id)
                .append(" experimental=").append(experimental ? 1 : 0).append(';');

        RootShell.Result result = RootShell.runFixedScript(script.toString(), 155L);
        String raw = String.join("\n", result.output);
        if (!result.ok()) throw new IOException(result.timedOut
                ? "prepare timed out" : "prepare failed (exit " + result.exitCode + "): " + tail(raw));
        java.util.regex.Matcher match = java.util.regex.Pattern.compile(
                "(?m)^G10_PREPARE passed=1 pid=([0-9]+) start_ticks=([0-9]+) ").matcher(raw);
        if (!match.find()) throw new IOException("prepare receipt missing");
        return new Receipt(Integer.parseInt(match.group(1)), Long.parseLong(match.group(2)), raw);
    }

    private static ArtifactRegistry.Artifact findArtifact(ArtifactRegistry.Backend backend,
                                                          String deviceName) throws IOException {
        for (ArtifactRegistry.Artifact artifact : backend.artifacts)
            if (artifact.deviceName.equals(deviceName)) return artifact;
        throw new IOException("backend artifact role missing: " + deviceName);
    }

    private static File stageVerifiedAsset(Context context, File directory,
                                           String asset, String deviceName,
                                           String expectedSha) throws Exception {
        return stageVerifiedInput(context.getAssets().open(asset), directory,
                deviceName, expectedSha);
    }

    private static File stageVerifiedProfile(Context context, File directory,
                                             BuildProfileRegistry.Profile profile,
                                             String deviceName) throws Exception {
        return stageVerifiedInput(profile.open(context), directory,
                deviceName, profile.profileSha256);
    }

    private static File stageVerifiedInput(InputStream source, File directory,
                                           String deviceName,
                                           String expectedSha) throws Exception {
        File output = new File(directory, deviceName);
        File temporary = new File(directory, deviceName + ".pending");
        // Fixed artifacts are immutable and hash-pinned. Reuse an already
        // verified app-private copy instead of decompressing and fsyncing the
        // same multi-megabyte assets before every game preparation.
        if (output.isFile() && expectedSha.equals(sha256(output))) {
            source.close();
            if (temporary.exists() && !temporary.delete())
                throw new IOException("stale staged temporary file: " + deviceName);
            return output;
        }
        try (InputStream input = source;
             FileOutputStream sink = new FileOutputStream(temporary, false)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) >= 0) sink.write(buffer, 0, count);
            sink.getFD().sync();
        }
        if (!expectedSha.equals(sha256(temporary)))
            throw new IOException("staged hash mismatch: " + deviceName);
        if (output.exists() && !output.delete()) throw new IOException("stale stage file");
        if (!temporary.renameTo(output)) throw new IOException("atomic stage publish failed");
        return output;
    }

    private static void requireSafe(GameProcessScanner.Candidate selected,
                                    BuildProfileRegistry.Profile profile,
                                    boolean experimental) throws IOException {
        if (selected == null || profile == null || selected.profile == null ||
                selected.profile != profile ||
                selected.backend == null ||
                !selected.backend.hostMachine.equals(selected.hostMachine) ||
                !selected.backend.bridgeSet.equals(selected.bridgeSet) ||
                !experimental && (!selected.runtimeSupported() ||
                        !selected.backend.matches(selected.hostMachine, selected.bridgeSet,
                                selected.libcSha256)) ||
                !selected.packageName.matches("[A-Za-z0-9_]+(?:\\.[A-Za-z0-9_]+)+") ||
                !selected.processName.matches("[A-Za-z0-9._:-]{1,191}") ||
                (!experimental &&
                        !selected.nativeSha256.equals(profile.nativeSha256)))
            throw new IOException("selected process/profile identity rejected");
    }

    private static String quote(String value) throws IOException {
        if (value.indexOf('\0') >= 0 || value.indexOf('\n') >= 0 || value.indexOf('\r') >= 0)
            throw new IOException("unsafe shell value");
        return "'" + value.replace("'", "'\\''") + "'";
    }

    private static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = new java.io.FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) >= 0) digest.update(buffer, 0, count);
        }
        StringBuilder result = new StringBuilder(64);
        for (byte value : digest.digest()) result.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return result.toString();
    }

    private static String tail(String value) {
        return value.length() <= 320 ? value : value.substring(value.length() - 320);
    }
}
