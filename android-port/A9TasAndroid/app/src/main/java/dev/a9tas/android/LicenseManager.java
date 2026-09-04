package dev.a9tas.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.provider.Settings;
import android.util.Base64;

import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.MessageDigest;
import java.security.PublicKey;
import java.security.Signature;
import java.security.spec.X509EncodedKeySpec;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;

/** Offline, signed, time-limited research license. The APK contains no signing secret. */
final class LicenseManager {
    private static final String PREFIX = "A9L1";
    private static final long CLOCK_SKEW_SECONDS = 300L;
    // The activity polls operation state while it is visible. Persisting the
    // anti-rollback watermark on every poll caused roughly one fsync per
    // second. A one-minute cadence remains comfortably inside the accepted
    // five-minute clock skew while avoiding needless flash writes.
    private static final long CLOCK_PERSIST_INTERVAL_SECONDS = 60L;
    private static final long MAX_VALIDITY_SECONDS = 366L * 24L * 60L * 60L;
    private static final String PUBLIC_KEY_BASE64 =
            "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE1iC4s9FoEXjkOZaPkv1qJ4JhdvohqrwBthVEtTOIwLZwJBLFUzxubIOrtGreH4+EDKUg9TW8CO9u6nMF+XJqwA==";

    static final class Status {
        final boolean valid;
        final String message;
        final long expires;

        Status(boolean valid, String message, long expires) {
            this.valid = valid;
            this.message = message;
            this.expires = expires;
        }
    }

    private LicenseManager() {}

    static String deviceCode(Context context) throws Exception {
        String androidId = Settings.Secure.getString(
                context.getContentResolver(), Settings.Secure.ANDROID_ID);
        String source = safe(androidId) + "\n" + context.getPackageName() + "\n" +
                safe(Build.BRAND) + "\n" + safe(Build.DEVICE) + "\n" +
                safe(Build.HARDWARE) + "\n" + safe(Build.MODEL);
        byte[] hash = MessageDigest.getInstance("SHA-256").digest(
                source.getBytes(StandardCharsets.UTF_8));
        return hex(Arrays.copyOf(hash, 16));
    }

    static Status current(Context context) {
        if (DeveloperBuild.enabled(context))
            return new Status(true, "开发者兼容测试版（免卡密）",
                    DeveloperBuild.EXPIRES_UTC_SECONDS);
        String token = context.getSharedPreferences("license", Context.MODE_PRIVATE)
                .getString("token", "");
        if (token.isEmpty()) return new Status(false, "未激活", 0L);
        try {
            return verify(context, token, true);
        } catch (Exception error) {
            return new Status(false, error.getMessage() == null ?
                    "许可证无效" : error.getMessage(), 0L);
        }
    }

    static Status activate(Context context, String token) throws Exception {
        String normalized = token == null ? "" : token.trim();
        Status status = verify(context, normalized, false);
        if (!status.valid) throw new IllegalArgumentException(status.message);
        if (!context.getSharedPreferences("license", Context.MODE_PRIVATE).edit()
                .putString("token", normalized)
                .putLong("max_seen_epoch", System.currentTimeMillis() / 1000L)
                .commit())
            throw new IllegalStateException("许可证保存失败");
        return status;
    }

    static void clear(Context context) {
        context.getSharedPreferences("license", Context.MODE_PRIVATE).edit().clear().apply();
    }

    static void requireValid(Context context) throws Exception {
        Status status = current(context);
        if (!status.valid) throw new SecurityException("许可证不可用：" + status.message);
    }

    private static Status verify(Context context, String token, boolean updateClock)
            throws Exception {
        if (token.length() < 40 || token.length() > 2048)
            throw new IllegalArgumentException("卡密长度无效");
        String[] parts = token.split("[.]", -1);
        if (parts.length != 3 || !PREFIX.equals(parts[0]))
            throw new IllegalArgumentException("卡密格式无效");
        byte[] payload = decode(parts[1]);
        byte[] signatureBytes = decode(parts[2]);
        if (payload.length == 0 || payload.length > 1024 ||
                signatureBytes.length < 64 || signatureBytes.length > 80)
            throw new IllegalArgumentException("卡密载荷无效");
        PublicKey key = KeyFactory.getInstance("EC").generatePublic(new X509EncodedKeySpec(
                Base64.decode(PUBLIC_KEY_BASE64, Base64.DEFAULT)));
        Signature verifier = Signature.getInstance("SHA256withECDSA");
        verifier.initVerify(key);
        verifier.update(payload);
        if (!verifier.verify(signatureBytes))
            throw new IllegalArgumentException("签名无效");

        JSONObject json = new JSONObject(new String(payload, StandardCharsets.UTF_8));
        if (json.length() != 6 || !json.has("v") || !json.has("id") ||
                !json.has("nbf") || !json.has("exp") || !json.has("feature") ||
                !json.has("device") || json.getInt("v") != 1 ||
                !json.getString("id").matches("[0-9a-f]{16}") ||
                !"tas".equals(json.getString("feature")))
            throw new IllegalArgumentException("卡密声明无效");
        long notBefore = json.getLong("nbf");
        long expires = json.getLong("exp");
        if (notBefore <= 0 || expires <= notBefore ||
                expires - notBefore > MAX_VALIDITY_SECONDS)
            throw new IllegalArgumentException("卡密有效期无效");
        String expectedDevice = json.getString("device");
        if (!("*".equals(expectedDevice) ||
                expectedDevice.matches("[0-9a-f]{32}") &&
                        expectedDevice.equals(deviceCode(context))))
            throw new IllegalArgumentException("卡密不属于此设备");

        long now = System.currentTimeMillis() / 1000L;
        SharedPreferences preferences =
                context.getSharedPreferences("license", Context.MODE_PRIVATE);
        long maxSeen = preferences.getLong("max_seen_epoch", 0L);
        if (maxSeen > 0 && now + CLOCK_SKEW_SECONDS < maxSeen)
            throw new IllegalArgumentException("检测到系统时间回拨");
        if (now + CLOCK_SKEW_SECONDS < notBefore)
            throw new IllegalArgumentException("卡密尚未生效");
        if (now > expires) throw new IllegalArgumentException("卡密已过期");
        if (updateClock && now - maxSeen >= CLOCK_PERSIST_INTERVAL_SECONDS)
            preferences.edit().putLong("max_seen_epoch", now).apply();
        return new Status(true, "研究许可有效", expires);
    }

    private static byte[] decode(String value) {
        return Base64.decode(value, Base64.URL_SAFE | Base64.NO_WRAP | Base64.NO_PADDING);
    }

    private static String safe(String value) { return value == null ? "" : value; }

    private static String hex(byte[] bytes) {
        StringBuilder output = new StringBuilder(bytes.length * 2);
        for (byte value : bytes)
            output.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return output.toString();
    }
}
