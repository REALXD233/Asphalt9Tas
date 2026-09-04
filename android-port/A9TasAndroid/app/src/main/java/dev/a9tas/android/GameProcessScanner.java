package dev.a9tas.android;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

final class GameProcessScanner {
    private static final String UNKNOWN_SHA =
            "0000000000000000000000000000000000000000000000000000000000000000";

    static final class Candidate {
        final int pid;
        final long startTicks;
        final String processName;
        final String packageName;
        final String libraryPath;
        final String nativeSha256;
        final String hostMachine;
        final boolean nativeBridge;
        final String bridgeSet;
        final String libcSha256;
        final BuildProfileRegistry.Profile profile;
        final ArtifactRegistry.Backend backend;
        final boolean generatedIdentityBinding;

        Candidate(int pid, long startTicks, String processName, String packageName,
                  String libraryPath, String nativeSha256, String hostMachine,
                  boolean nativeBridge, String bridgeSet, String libcSha256,
                  BuildProfileRegistry.Profile profile, ArtifactRegistry.Backend backend,
                  boolean generatedIdentityBinding) {
            this.pid = pid;
            this.startTicks = startTicks;
            this.processName = processName;
            this.packageName = packageName;
            this.libraryPath = libraryPath;
            this.nativeSha256 = nativeSha256;
            this.hostMachine = hostMachine;
            this.nativeBridge = nativeBridge;
            this.bridgeSet = bridgeSet;
            this.libcSha256 = libcSha256;
            this.profile = profile;
            this.backend = backend;
            this.generatedIdentityBinding = generatedIdentityBinding;
        }

        boolean runtimeSupported() { return backend != null; }

        boolean supported() {
            return startTicks > 0 && nativeIdentityAvailable() && runtimeSupported() &&
                    (!backend.requiresLibcIdentity() || libcIdentityAvailable()) &&
                    profile != null;
        }

        boolean nativeIdentityAvailable() { return !UNKNOWN_SHA.equals(nativeSha256); }

        boolean libcIdentityAvailable() { return !UNKNOWN_SHA.equals(libcSha256); }

        String runtimeLabel() {
            if (runtimeSupported()) return backend.runtimeLabel();
            if (!libcIdentityAvailable()) return hostMachine + " · libc identity unavailable";
            if ("arm64".equals(hostMachine)) return "native ARM64 · adapter pending";
            if ("x86_64".equals(hostMachine) && nativeBridge)
                return "x86_64 NativeBridge (" + bridgeSet + ") · backend pending";
            return hostMachine + " · unsupported runtime";
        }

        String compatibilityReceipt() {
            return "A9TAS_RUNTIME_V1 host=" + hostMachine +
                    " bridges=" + bridgeSet + " libc_sha256=" +
                    (libcIdentityAvailable() ? libcSha256 : "unavailable") +
                    " native_sha256=" +
                    (nativeIdentityAvailable() ? nativeSha256 : "unavailable") + " backend=" +
                    (backend == null ? "none" : backend.id) +
                    " start_ticks=" + startTicks +
                    " native_hash_known=" + (nativeIdentityAvailable() ? 1 : 0) +
                    " native_identity_source=" + (generatedIdentityBinding ?
                            "autogen_session" : "identity_helper") +
                    " libc_hash_known=" + (libcIdentityAvailable() ? 1 : 0) +
                    " libc_binding=" + (backend == null ? "unmatched" : backend.libcBinding);
        }

        @Override public String toString() {
            String build = !nativeIdentityAvailable() ? "game build hash unavailable" :
                    profile == null ? "unsupported game build" : profile.label;
            return packageName + " · PID " + pid + " · " + build + " · " + runtimeLabel();
        }
    }

    private static final String FIXED_SCAN_SCRIPT =
            "printf 'A9TAS_SCAN_V2\\tbegin\\n'; total_count=0; app_count=0; found_count=0; " +
            "scan_one() { d=$1; scan_mode=$2; p=${d#/proc/}; " +
            "case \"$p\" in ''|*[!0-9]*) return;; esac; " +
            "total_count=$((total_count+1)); " +
            "c=$(tr '\\000' '\\n' <\"$d/cmdline\" 2>/dev/null | head -n1); " +
            "[ -n \"$c\" ] || return; " +
            // All known national A9 channel packages match one of these name
            // families. This is only a fast pass: an exact libAsphalt9.so maps
            // check remains mandatory and a profile-independent full fallback
            // runs when the fast pass finds no game.
            "if [ \"$scan_mode\" = fast ]; then case \"$c\" in " +
            "*kuang.kybc*|*[Aa]sphalt*|*[Gg]loft[Aa]9*) ;; *) return;; esac; fi; " +
            // Asphalt is an installed application and therefore runs under an
            // Android application UID. Filtering kernel/system processes before
            // opening their often-large maps files is critical on Android 16,
            // where a full /proc maps sweep can exceed the former 20 s budget.
            "u=$(sed -n 's/^Uid:[[:space:]]*\\([0-9][0-9]*\\).*/\\1/p' \"$d/status\" 2>/dev/null); " +
            "case \"$u\" in ''|*[!0-9]*) return;; esac; [ \"$u\" -ge 10000 ] || return; app_count=$((app_count+1)); " +
            "m=$(grep -m1 'libAsphalt9\\.so' \"$d/maps\" 2>/dev/null); " +
            "[ -n \"$m\" ] || return; found_count=$((found_count+1)); " +
            "f=${m#* /}; z=0000000000000000000000000000000000000000000000000000000000000000; " +
            "h=$z; " +
            "st=$(sed 's/^[^)]*) //' \"$d/stat\" 2>/dev/null); set -- $st; s=${20}; " +
            "[ -n \"$s\" ] || s=0; " +
            "e=$(readlink \"$d/exe\" 2>/dev/null); " +
            "x=$(od -An -j18 -N2 -tx1 \"$e\" 2>/dev/null | tr -d ' \\n'); " +
            "case \"$x\" in 3e00) a=x86_64;; b700) a=arm64;; 0300) a=x86;; 2800) a=arm;; *) a=unknown;; esac; " +
            "b=; if grep -q '/libhoudini[.]so' \"$d/maps\" 2>/dev/null; then b=libhoudini.so; fi; " +
            "if grep -q '/libnb[.]so' \"$d/maps\" 2>/dev/null; then [ -z \"$b\" ] || b=\"$b+\"; b=\"${b}libnb.so\"; fi; " +
            "if grep -q '/libndk_translation[.]so' \"$d/maps\" 2>/dev/null; then [ -z \"$b\" ] || b=\"$b+\"; b=\"${b}libndk_translation.so\"; fi; " +
            "[ -n \"$b\" ] || b=none; if [ \"$b\" = none ]; then n=0; else n=1; fi; " +
            "lm=$(grep -m1 '/libc[.]so' \"$d/maps\" 2>/dev/null); lc=${lm#* /}; " +
            "l=$z; " +
            "printf '%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\t%s\\n' \"$p\" \"$s\" \"$c\" \"$h\" \"$f\" \"$a\" \"$n\" \"$b\" \"$l\" \"$lc\"; " +
            "}; " +
            "for d in /proc/[0-9]*; do scan_one \"$d\" fast; done; " +
            "if [ \"$found_count\" -eq 0 ]; then " +
            "printf 'A9TAS_SCAN_V2\\tfallback\\tproc=%s\\tapps=%s\\n' \"$total_count\" \"$app_count\"; " +
            "total_count=0; app_count=0; " +
            "for d in /proc/[0-9]*; do scan_one \"$d\" all; done; fi; " +
            "printf 'A9TAS_SCAN_V2\\tend\\tproc=%s\\tapps=%s\\tfound=%s\\n' \"$total_count\" \"$app_count\" \"$found_count\"";

    private GameProcessScanner() {}

    static List<Candidate> scan(Context context, BuildProfileRegistry registry,
                                ArtifactRegistry runtimeRegistry) throws Exception {
        RootShell.Result result = RootShell.runFixedScript(FIXED_SCAN_SCRIPT, 45L);
        if (!result.ok()) throw new IOException(result.timedOut
                ? "root scan timed out" + scanProgress(result.output)
                : "root scan failed (exit " + result.exitCode + ")" +
                        scanProgress(result.output));
        List<Candidate> candidates = new ArrayList<>();
        for (String line : result.output) {
            String[] fields = line.split("\\t", -1);
            if (fields.length != 10 || !fields[0].matches("[0-9]+") ||
                    !fields[1].matches("[0-9]+") ||
                    !fields[3].matches("[0-9a-fA-F]{64}") ||
                    !fields[5].matches("x86_64|arm64|x86|arm|unknown") ||
                    !fields[6].matches("[01]") ||
                    !fields[7].matches("none|libhoudini[.]so|libnb[.]so|libndk_translation[.]so|" +
                            "libhoudini[.]so[+]libnb[.]so|" +
                            "libhoudini[.]so[+]libndk_translation[.]so|" +
                            "libnb[.]so[+]libndk_translation[.]so|" +
                            "libhoudini[.]so[+]libnb[.]so[+]libndk_translation[.]so") ||
                    !fields[8].matches("[0-9a-fA-F]{64}")) continue;
            Map<String, String> recovered = recoverHashes(context, runtimeRegistry,
                    fields[5], fields[4], fields[9]);
            int pid = Integer.parseInt(fields[0]);
            long startTicks = Long.parseLong(fields[1]);
            String process = fields[2];
            String packageName = process.contains(":")
                    ? process.substring(0, process.indexOf(':')) : process;
            String sha = recovered.getOrDefault(fields[4], fields[3]).toLowerCase(Locale.ROOT);
            boolean generatedBinding = false;
            if (UNKNOWN_SHA.equals(sha)) {
                String bound = generatedIdentityBinding(context, registry, pid, startTicks,
                        fields[4], fields[5], fields[7]);
                if (bound != null) {
                    sha = bound;
                    generatedBinding = true;
                }
            }
            String libcSha = recovered.getOrDefault(fields[9], fields[8]).toLowerCase(Locale.ROOT);
            ArtifactRegistry.Backend backend = runtimeRegistry.find(
                    fields[5], fields[7], libcSha);
            candidates.add(new Candidate(pid, startTicks, process, packageName,
                    fields[4], sha, fields[5], "1".equals(fields[6]),
                    fields[7], libcSha, registry.find(sha), backend, generatedBinding));
        }
        return candidates;
    }

    private static String scanProgress(List<String> output) {
        for (int index = output.size() - 1; index >= 0; --index) {
            String line = output.get(index);
            if (line.startsWith("A9TAS_SCAN_V2\t"))
                return " (" + line.replace('\t', ' ') + ")";
        }
        return " (root shell produced no scan marker)";
    }

    private static String generatedIdentityBinding(Context context,
            BuildProfileRegistry registry, int pid, long startTicks, String libraryPath,
            String machine, String bridgeSet) {
        android.content.SharedPreferences binding = context.getSharedPreferences(
                "profile_autogen_binding", Context.MODE_PRIVATE);
        String sha = binding.getString("native_sha256", "");
        String profileId = binding.getString("profile_id", "");
        BuildProfileRegistry.Profile profile = registry.find(sha);
        if (binding.getInt("pid", -1) != pid ||
                binding.getLong("start_ticks", -1) != startTicks ||
                !libraryPath.equals(binding.getString("library_path", "")) ||
                !machine.equals(binding.getString("host_machine", "")) ||
                !bridgeSet.equals(binding.getString("bridge_set", "")) ||
                profile == null || !profile.id.equals(profileId)) return null;
        return profile.nativeSha256;
    }

    private static Map<String, String> recoverHashes(Context context,
                                                      ArtifactRegistry registry,
                                                      String machine,
                                                      String... paths) throws Exception {
        Map<String, String> hashes = new HashMap<>();
        ArtifactRegistry.IdentityHelper helper = registry.identityHelperFor(machine);
        if (helper == null) return hashes;
        byte[] bytes = BuildProfileRegistry.readAll(context.getAssets().open(helper.asset));
        String stagedSha = hex(MessageDigest.getInstance("SHA-256").digest(bytes));
        if (!helper.sha256.equals(stagedSha)) throw new IOException("identity helper asset changed");
        File stage = new File(context.getCacheDir(), helper.deviceName);
        try (FileOutputStream output = new FileOutputStream(stage, false)) {
            output.write(bytes);
            output.getFD().sync();
        }
        if (!stage.setReadable(true, false)) throw new IOException("identity helper staging failed");
        String target = "/data/local/tmp/" + helper.deviceName;
        StringBuilder script = new StringBuilder("set -eu; cp ")
                .append(quote(stage.getCanonicalPath())).append(' ').append(quote(target)).append(';')
                .append("chmod 0700 ").append(quote(target)).append(';')
                .append(quote(target)).append(" --expect ").append(helper.sha256).append(' ')
                .append(quote(target)).append(';');
        for (String path : paths) {
            if (path == null || path.isEmpty()) continue;
            script.append(quote(target)).append(' ').append(quote(path)).append(" || true;");
        }
        script.append("rm -f ").append(quote(target)).append(';');
        RootShell.Result result = RootShell.runFixedScript(script.toString(), 180L);
        if (!result.ok()) return hashes;
        for (String output : result.output) {
            String[] fields = output.split("\\t", 3);
            if (fields.length == 3 && fields[0].equals("A9TAS_HASH_V1") &&
                    fields[1].matches("[0-9a-f]{64}"))
                hashes.put(fields[2], fields[1]);
        }
        return hashes;
    }

    private static String quote(String value) throws IOException {
        if (value.indexOf('\0') >= 0 || value.indexOf('\n') >= 0 || value.indexOf('\r') >= 0)
            throw new IOException("unsafe shell value");
        return "'" + value.replace("'", "'\\''") + "'";
    }

    private static String hex(byte[] bytes) {
        StringBuilder output = new StringBuilder(bytes.length * 2);
        for (byte value : bytes)
            output.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return output.toString();
    }
}
