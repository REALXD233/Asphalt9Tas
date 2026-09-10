package dev.a9tas.android;

import android.content.Context;
import android.os.Process;
import android.util.Base64;

import org.json.JSONObject;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Locale;
import java.util.UUID;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Read-only, package/channel-neutral ARM64 ELF resolver. */
final class Arm64ProfileAutoGenerator {
    private static final Pattern RECEIPT = Pattern.compile(
            "(?m)^A9_PROFILE_AUTOGEN_V1 passed=1 native_sha256=([0-9a-f]{64}) " +
            "build_id=([0-9a-f]{40}) profile_sha256=([0-9a-f]{64}) " +
            "vptr0=([0-9a-f]+) vptr588=([0-9a-f]+) vptr6c0=([0-9a-f]+) " +
            "vptr718=([0-9a-f]+) vptr748=([0-9a-f]+) catalog_sha256=([0-9a-f]{64})$");

    private Arm64ProfileAutoGenerator() {}

    static boolean supports(GameProcessScanner.Candidate candidate,
                            ArtifactRegistry.Backend backend) {
        if (candidate == null || backend == null ||
                backend.profileAutogenDeviceName == null) return false;
        return ("arm64".equals(candidate.hostMachine) && !candidate.nativeBridge &&
                "none".equals(candidate.bridgeSet)) ||
                ("x86_64".equals(candidate.hostMachine) && candidate.nativeBridge);
    }

    static BuildProfileRegistry.Profile generate(Context context,
            GameProcessScanner.Candidate candidate,
            ArtifactRegistry registry) throws Exception {
        if (candidate == null || candidate.pid <= 0 || candidate.startTicks <= 0 ||
                candidate.libraryPath.isEmpty())
            throw new IOException("所选游戏进程身份或核心路径不可用");
        ArtifactRegistry.Backend backend = candidate.backend != null ? candidate.backend :
                registry.findExperimental(candidate.hostMachine, candidate.bridgeSet);
        if (!supports(candidate, backend))
            throw new IOException("此环境没有 ARM64 核心定位器；支持原生 ARM64 与 x86_64 转译环境");
        ArtifactRegistry.Artifact resolver = find(backend, backend.profileAutogenDeviceName);
        ArtifactRegistry.IdentityHelper identity = registry.identityHelperFor(candidate.hostMachine);
        if (identity == null) throw new IOException("ARM64 身份工具不可用");

        File stage = new File(context.getCacheDir(), resolver.deviceName);
        writeVerified(context.getAssets().open(resolver.asset), stage, resolver.sha256);
        File identityStage = new File(context.getCacheDir(), identity.deviceName);
        writeVerified(context.getAssets().open(identity.asset), identityStage, identity.sha256);
        String nonce = UUID.randomUUID().toString().replace("-", "");
        String remoteResolver = "/data/local/tmp/a9tas-profile-autogen-" + nonce;
        String remoteIdentity = "/data/local/tmp/a9tas-profile-identity-" + nonce;
        String remoteOutput = "/data/local/tmp/a9tas-profile-output-" + nonce + ".bin";
        File localOutput = new File(context.getCacheDir(), "generated-profile-" + nonce + ".bin");
        String cleanup = "rm -f " + quote(remoteResolver) + " " +
                quote(remoteIdentity) + " " + quote(remoteOutput);
        String script = "set -eu; cleanup(){ " + cleanup + "; }; " +
                "trap cleanup EXIT HUP INT TERM; cleanup; " +
                "cp " + quote(stage.getCanonicalPath()) + " " + quote(remoteResolver) + "; " +
                "cp " + quote(identityStage.getCanonicalPath()) + " " + quote(remoteIdentity) + "; " +
                "chmod 0700 " + quote(remoteResolver) + " " + quote(remoteIdentity) + "; " +
                quote(remoteIdentity) + " --expect " + resolver.sha256 + " " +
                quote(remoteResolver) + "; " +
                "st=$(sed 's/^[^)]*) //' /proc/" + candidate.pid + "/stat); set -- $st; " +
                "[ \"${20}\" = \"" + candidate.startTicks + "\" ]; " +
                "grep -Fq " + quote(candidate.libraryPath) + " /proc/" + candidate.pid + "/maps; " +
                quote(remoteResolver) + " " + quote(candidate.libraryPath) + " " +
                quote(remoteOutput) + "; " +
                "cp " + quote(remoteOutput) + " " + quote(localOutput.getCanonicalPath()) + "; " +
                "chown " + Process.myUid() + ":" + Process.myUid() + " " +
                quote(localOutput.getCanonicalPath()) + "; chmod 0600 " +
                quote(localOutput.getCanonicalPath()) + ";";
        try {
            RootShell.Result result = RootShell.runFixedScript(script, 420L, "profile-autogen");
            String output = String.join("\n", result.output);
            if (!result.ok()) throw new IOException(result.timedOut ?
                    "Profile 自动定位超时" : "Profile 自动定位失败：" + tail(output));
            Matcher receipt = RECEIPT.matcher(output);
            if (!receipt.find())
                throw new IOException("Profile 自动定位回执缺失：" + tail(output));
            byte[] blob = read(localOutput);
            String blobSha = sha256(blob);
            if (!blobSha.equals(receipt.group(3)))
                throw new IOException("自动生成的 Profile 文件哈希不匹配");
            if (candidate.nativeIdentityAvailable() &&
                    !candidate.nativeSha256.equals(receipt.group(1)))
                throw new IOException("游戏核心在 Profile 生成期间发生变化");
            JSONObject bundle = new JSONObject()
                    .put("schema", "A9_PROFILE_BUNDLE_V1")
                    .put("label", "ARM64 core " + receipt.group(1).substring(0, 16) +
                            " · 自动定位")
                    .put("native_sha256", receipt.group(1))
                    .put("build_id", receipt.group(2))
                    .put("profile_sha256", blobSha)
                    .put("profile_base64", Base64.encodeToString(blob, Base64.NO_WRAP))
                    .put("generator", new JSONObject()
                            .put("name", "arm64-signature-resolver-v1")
                            .put("catalog_sha256", receipt.group(9)))
                    .put("practice_proof", new JSONObject()
                            .put("vptr0_rva", "0x" + receipt.group(4))
                            .put("vptr588_rva", "0x" + receipt.group(5))
                            .put("vptr6c0_rva", "0x" + receipt.group(6))
                            .put("vptr718_rva", "0x" + receipt.group(7))
                            .put("vptr748_rva", "0x" + receipt.group(8)));
            return BuildProfileRegistry.importBundle(context, new ByteArrayInputStream(
                    (bundle.toString(2) + "\n").getBytes(StandardCharsets.UTF_8)));
        } finally {
            if (localOutput.exists()) localOutput.delete();
        }
    }

    private static ArtifactRegistry.Artifact find(ArtifactRegistry.Backend backend,
                                                   String name) throws IOException {
        for (ArtifactRegistry.Artifact item : backend.artifacts)
            if (item.deviceName.equals(name)) return item;
        throw new IOException("Profile resolver artifact is missing");
    }

    private static void writeVerified(InputStream source, File output,
                                      String expectedSha) throws Exception {
        try (InputStream input = source; FileOutputStream sink = new FileOutputStream(output, false)) {
            byte[] buffer = new byte[64 * 1024]; int count;
            while ((count = input.read(buffer)) >= 0) sink.write(buffer, 0, count);
            sink.getFD().sync();
        }
        if (!expectedSha.equals(sha256(output)))
            throw new IOException("Profile resolver asset identity changed");
    }

    private static byte[] read(File file) throws IOException {
        if (!file.isFile() || file.length() <= 0 || file.length() > 64 * 1024)
            throw new IOException("generated Profile size is invalid");
        try (FileInputStream input = new FileInputStream(file)) {
            return BuildProfileRegistry.readAll(input);
        }
    }

    private static String sha256(byte[] bytes) throws Exception {
        StringBuilder output = new StringBuilder(64);
        for (byte value : MessageDigest.getInstance("SHA-256").digest(bytes))
            output.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return output.toString();
    }

    private static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (FileInputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024]; int count;
            while ((count = input.read(buffer)) >= 0) digest.update(buffer, 0, count);
        }
        StringBuilder output = new StringBuilder(64);
        for (byte value : digest.digest())
            output.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return output.toString();
    }

    private static String quote(String value) throws IOException {
        if (value.indexOf('\0') >= 0 || value.indexOf('\n') >= 0 || value.indexOf('\r') >= 0)
            throw new IOException("unsafe shell path");
        return "'" + value.replace("'", "'\\''") + "'";
    }

    private static String tail(String value) {
        return value.length() <= 800 ? value : value.substring(value.length() - 800);
    }
}
