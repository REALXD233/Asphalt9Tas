package dev.a9tas.android;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import android.util.Base64;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Collections;
import java.util.ArrayList;
import java.util.List;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

final class BuildProfileRegistry {
    private static final String BUNDLE_SCHEMA = "A9_PROFILE_BUNDLE_V1";
    private static final String IMPORTED_SCHEMA = "A9_IMPORTED_PROFILE_V1";
    private static final int CORE_SIZE = 384;
    private static final int VEHICLE_RVA_COUNT = 42;

    static final class Profile {
        final String id;
        final String label;
        final String nativeSha256;
        final String buildId;
        final String profileAsset;
        final String profileSha256;
        final long practiceVptr0Rva;
        final long practiceVptr588Rva;
        final long practiceVptr6c0Rva;
        final long practiceVptr718Rva;
        final long practiceVptr748Rva;
        final File importedFile;

        Profile(String id, String label, String nativeSha256, String buildId,
                String profileAsset, String profileSha256,
                long practiceVptr0Rva, long practiceVptr588Rva,
                long practiceVptr6c0Rva, long practiceVptr718Rva,
                long practiceVptr748Rva) {
            this(id, label, nativeSha256, buildId, profileAsset, profileSha256,
                    practiceVptr0Rva, practiceVptr588Rva, practiceVptr6c0Rva,
                    practiceVptr718Rva, practiceVptr748Rva, null);
        }

        Profile(String id, String label, String nativeSha256, String buildId,
                String profileAsset, String profileSha256,
                long practiceVptr0Rva, long practiceVptr588Rva,
                long practiceVptr6c0Rva, long practiceVptr718Rva,
                long practiceVptr748Rva, File importedFile) {
            this.id = id;
            this.label = label;
            this.nativeSha256 = nativeSha256;
            this.buildId = buildId;
            this.profileAsset = profileAsset;
            this.profileSha256 = profileSha256;
            this.practiceVptr0Rva = practiceVptr0Rva;
            this.practiceVptr588Rva = practiceVptr588Rva;
            this.practiceVptr6c0Rva = practiceVptr6c0Rva;
            this.practiceVptr718Rva = practiceVptr718Rva;
            this.practiceVptr748Rva = practiceVptr748Rva;
            this.importedFile = importedFile;
        }

        InputStream open(Context context) throws IOException {
            return importedFile == null ? context.getAssets().open(profileAsset) :
                    new FileInputStream(importedFile);
        }

        boolean imported() { return importedFile != null; }
    }

    private final Map<String, Profile> byNativeSha;
    private final List<Profile> profiles;

    private BuildProfileRegistry(Map<String, Profile> byNativeSha, List<Profile> profiles) {
        this.byNativeSha = Collections.unmodifiableMap(byNativeSha);
        this.profiles = Collections.unmodifiableList(profiles);
    }

    static BuildProfileRegistry load(Context context) throws Exception {
        String text = new String(readAll(context.getAssets().open("profiles/registry.json")),
                StandardCharsets.UTF_8);
        JSONObject root = new JSONObject(text);
        if (root.getInt("schema") != 1) throw new IOException("unsupported profile registry");
        JSONArray entries = root.getJSONArray("profiles");
        Map<String, Profile> profiles = new HashMap<>();
        List<Profile> ordered = new ArrayList<>();
        for (int index = 0; index < entries.length(); ++index) {
            JSONObject item = entries.getJSONObject(index);
            Profile profile = new Profile(
                    item.getString("id"), item.getString("label"),
                    exactSha(item.getString("native_sha256")),
                    item.getString("build_id").toLowerCase(Locale.ROOT),
                    item.getString("profile_asset"),
                    exactSha(item.getString("profile_sha256")),
                    exactRva(item.getJSONObject("practice_proof")
                            .getString("vptr0_rva")),
                    exactRva(item.getJSONObject("practice_proof")
                            .getString("vptr588_rva")),
                    exactRva(item.getJSONObject("practice_proof")
                            .getString("vptr6c0_rva")),
                    exactRva(item.getJSONObject("practice_proof")
                            .getString("vptr718_rva")),
                    exactRva(item.getJSONObject("practice_proof")
                            .getString("vptr748_rva")));
            if (!profile.id.matches("[A-Za-z0-9_.-]{1,96}") ||
                    profile.label.isEmpty() || profile.label.length() > 160 ||
                    !profile.buildId.matches("[0-9a-f]{40}") ||
                    !profile.profileAsset.matches("profiles/[A-Za-z0-9_.-]+"))
                throw new IOException("invalid BuildProfile registry entry");
            if (profiles.put(profile.nativeSha256, profile) != null)
                throw new IOException("duplicate native hash");
            ordered.add(profile);
        }
        loadImported(context, profiles, ordered);
        return new BuildProfileRegistry(profiles, ordered);
    }

    static Profile importBundle(Context context, InputStream input) throws Exception {
        byte[] documentBytes = readAll(input);
        if (documentBytes.length == 0 || documentBytes.length > 256 * 1024)
            throw new IOException("Profile bundle size is invalid");
        JSONObject bundle = new JSONObject(new String(documentBytes, StandardCharsets.UTF_8));
        if (!BUNDLE_SCHEMA.equals(bundle.optString("schema")))
            throw new IOException("unsupported Profile bundle");
        String label = bundle.getString("label").trim();
        if (label.isEmpty() || label.length() > 160)
            throw new IOException("invalid Profile label");
        byte[] blob;
        try {
            blob = Base64.decode(bundle.getString("profile_base64"), Base64.DEFAULT);
        } catch (IllegalArgumentException error) {
            throw new IOException("Profile bundle base64 is invalid", error);
        }
        ParsedCore core = parseProfile(blob);
        String blobSha = hex(MessageDigest.getInstance("SHA-256").digest(blob));
        if (!core.nativeSha.equals(exactSha(bundle.getString("native_sha256"))) ||
                !core.buildId.equals(bundle.getString("build_id").toLowerCase(Locale.ROOT)) ||
                !blobSha.equals(exactSha(bundle.getString("profile_sha256"))))
            throw new IOException("Profile bundle identity does not match its payload");
        JSONObject proof = bundle.getJSONObject("practice_proof");
        long v0 = exactRva(proof.getString("vptr0_rva"));
        long v588 = exactRva(proof.getString("vptr588_rva"));
        long v6c0 = exactRva(proof.getString("vptr6c0_rva"));
        long v718 = exactRva(proof.getString("vptr718_rva"));
        long v748 = exactRva(proof.getString("vptr748_rva"));
        for (long value : new long[]{v0, v588, v6c0, v718, v748})
            if (Long.compareUnsigned(value, core.imageSize) >= 0)
                throw new IOException("practice proof lies outside the game image");

        BuildProfileRegistry existing = load(context);
        Profile collision = existing.find(core.nativeSha);
        if (collision != null) {
            if (!collision.profileSha256.equals(blobSha))
                throw new IOException("a different Profile already owns this native build");
            return collision;
        }

        File directory = importedDirectory(context);
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("unable to create imported Profile store");
        String stem = core.nativeSha.substring(0, 16);
        File binary = new File(directory, stem + ".a9profile.bin");
        File metadata = new File(directory, stem + ".json");
        writeAtomic(binary, blob);
        JSONObject stored = new JSONObject()
                .put("schema", IMPORTED_SCHEMA)
                .put("id", "imported-" + stem)
                .put("label", label)
                .put("native_sha256", core.nativeSha)
                .put("build_id", core.buildId)
                .put("profile_sha256", blobSha)
                .put("binary", binary.getName())
                .put("practice_proof", new JSONObject()
                        .put("vptr0_rva", hexRva(v0))
                        .put("vptr588_rva", hexRva(v588))
                        .put("vptr6c0_rva", hexRva(v6c0))
                        .put("vptr718_rva", hexRva(v718))
                        .put("vptr748_rva", hexRva(v748)));
        writeAtomic(metadata, (stored.toString(2) + "\n").getBytes(StandardCharsets.UTF_8));
        Profile imported = load(context).find(core.nativeSha);
        if (imported == null || !imported.imported())
            throw new IOException("imported Profile did not publish atomically");
        return imported;
    }

    static byte[] exportBundle(Context context, Profile profile) throws Exception {
        if (profile == null) throw new IOException("no BuildProfile selected");
        byte[] blob = readAll(profile.open(context));
        ParsedCore core = parseProfile(blob);
        String blobSha = hex(MessageDigest.getInstance("SHA-256").digest(blob));
        if (!core.nativeSha.equals(profile.nativeSha256) ||
                !core.buildId.equals(profile.buildId) ||
                !blobSha.equals(profile.profileSha256))
            throw new IOException("BuildProfile changed before export");
        JSONObject bundle = new JSONObject()
                .put("schema", BUNDLE_SCHEMA)
                .put("label", profile.label)
                .put("native_sha256", core.nativeSha)
                .put("build_id", core.buildId)
                .put("profile_sha256", blobSha)
                .put("profile_base64", Base64.encodeToString(blob, Base64.NO_WRAP))
                .put("practice_proof", new JSONObject()
                        .put("vptr0_rva", hexRva(profile.practiceVptr0Rva))
                        .put("vptr588_rva", hexRva(profile.practiceVptr588Rva))
                        .put("vptr6c0_rva", hexRva(profile.practiceVptr6c0Rva))
                        .put("vptr718_rva", hexRva(profile.practiceVptr718Rva))
                        .put("vptr748_rva", hexRva(profile.practiceVptr748Rva)));
        return (bundle.toString(2) + "\n").getBytes(StandardCharsets.UTF_8);
    }

    private static void loadImported(Context context, Map<String, Profile> bySha,
                                     List<Profile> ordered) {
        File directory = importedDirectory(context);
        File[] files = directory.listFiles((dir, name) -> name.matches("[0-9a-f]{16}[.]json"));
        if (files == null) return;
        java.util.Arrays.sort(files, (left, right) -> left.getName().compareTo(right.getName()));
        for (File metadata : files) {
            try {
                JSONObject item = new JSONObject(new String(readAll(new FileInputStream(metadata)),
                        StandardCharsets.UTF_8));
                if (!IMPORTED_SCHEMA.equals(item.getString("schema"))) continue;
                String nativeSha = exactSha(item.getString("native_sha256"));
                if (bySha.containsKey(nativeSha)) continue;
                String binaryName = item.getString("binary");
                if (!binaryName.matches("[0-9a-f]{16}[.]a9profile[.]bin")) continue;
                File binary = new File(directory, binaryName).getCanonicalFile();
                if (!binary.getParentFile().equals(directory.getCanonicalFile()) || !binary.isFile())
                    continue;
                byte[] blob = readAll(new FileInputStream(binary));
                ParsedCore core = parseProfile(blob);
                String profileSha = exactSha(item.getString("profile_sha256"));
                if (!nativeSha.equals(core.nativeSha) ||
                        !profileSha.equals(hex(MessageDigest.getInstance("SHA-256").digest(blob))) ||
                        !core.buildId.equals(item.getString("build_id").toLowerCase(Locale.ROOT)))
                    continue;
                JSONObject proof = item.getJSONObject("practice_proof");
                Profile profile = new Profile(item.getString("id"), item.getString("label"),
                        nativeSha, core.buildId, "", profileSha,
                        exactRva(proof.getString("vptr0_rva")),
                        exactRva(proof.getString("vptr588_rva")),
                        exactRva(proof.getString("vptr6c0_rva")),
                        exactRva(proof.getString("vptr718_rva")),
                        exactRva(proof.getString("vptr748_rva")), binary);
                if (!profile.id.matches("imported-[0-9a-f]{16}") ||
                        profile.label.isEmpty() || profile.label.length() > 160) continue;
                bySha.put(nativeSha, profile);
                ordered.add(profile);
            } catch (Exception ignored) {
                // One damaged optional import must not make the whole APK unusable.
            }
        }
    }

    private static final class ParsedCore {
        final String nativeSha;
        final String buildId;
        final long imageSize;
        ParsedCore(String nativeSha, String buildId, long imageSize) {
            this.nativeSha = nativeSha;
            this.buildId = buildId;
            this.imageSize = imageSize;
        }
    }

    private static ParsedCore parseProfile(byte[] blob) throws IOException {
        if (blob == null || blob.length <= CORE_SIZE || blob.length > 64 * 1024)
            throw new IOException("Profile binary size is invalid");
        ByteBuffer input = ByteBuffer.wrap(blob).order(ByteOrder.LITTLE_ENDIAN);
        byte[] magic = new byte[8]; input.get(magic);
        if (!java.util.Arrays.equals(magic, new byte[]{'A','9','B','P','R','1',0,0}) ||
                input.getInt(8) != 1 || input.getInt(12) != CORE_SIZE ||
                input.getInt(16) != 0x1f || input.getInt(20) != 0)
            throw new IOException("invalid A9BPR1 core header");
        long imageSize = input.getLong(24);
        if (imageSize < 0x100000L) throw new IOException("invalid game image size");
        for (int offset = 32; offset < 256; offset += 8) {
            long rva = input.getLong(offset);
            if (rva < 0x1000L || Long.compareUnsigned(rva, imageSize) >= 0 || (rva & 3L) != 0)
                throw new IOException("Profile contains an invalid runtime RVA");
        }
        byte[] nativeSha = java.util.Arrays.copyOfRange(blob, 256, 288);
        byte[] sourceSha = java.util.Arrays.copyOfRange(blob, 288, 320);
        byte[] buildId = java.util.Arrays.copyOfRange(blob, 320, 340);
        if (allZero(nativeSha) || allZero(sourceSha) || allZero(buildId))
            throw new IOException("Profile identity is empty");
        for (int index = 340; index < CORE_SIZE; ++index)
            if (blob[index] != 0) throw new IOException("Profile reserved bytes are nonzero");
        if (blob.length > CORE_SIZE) {
            if (blob.length < CORE_SIZE + 32 ||
                    !java.util.Arrays.equals(java.util.Arrays.copyOfRange(blob, CORE_SIZE,
                            CORE_SIZE + 8), new byte[]{'A','9','B','P','A','X','1',0}) ||
                    input.getInt(CORE_SIZE + 8) != 1 ||
                    input.getInt(CORE_SIZE + 12) != 32 ||
                    input.getInt(CORE_SIZE + 16) != VEHICLE_RVA_COUNT ||
                    input.getInt(CORE_SIZE + 28) != 0 ||
                    input.getInt(CORE_SIZE + 24) != blob.length)
                throw new IOException("invalid A9BPAX1 annex header");
            int lifecycleCount = input.getInt(CORE_SIZE + 20);
            if (lifecycleCount <= 0 || lifecycleCount > 4096 ||
                    CORE_SIZE + 32 + 8L * (VEHICLE_RVA_COUNT + lifecycleCount) != blob.length)
                throw new IOException("invalid Profile annex length");
            int allQwords = VEHICLE_RVA_COUNT + lifecycleCount;
            int qwordStart = CORE_SIZE + 32;
            for (int index = 0; index < allQwords; ++index) {
                long value = input.getLong(qwordStart + index * 8);
                if (value < 0x1000 || Long.compareUnsigned(value, imageSize) >= 0 ||
                        (value & 3L) != 0)
                    throw new IOException("Profile annex contains an invalid RVA");
            }
            long previous = 0;
            int start = CORE_SIZE + 32 + VEHICLE_RVA_COUNT * 8;
            for (int index = 0; index < lifecycleCount; ++index) {
                long value = input.getLong(start + index * 8);
                if (value < 0x1000 || Long.compareUnsigned(value, imageSize) >= 0 ||
                        (value & 3L) != 0 || index > 0 && Long.compareUnsigned(value, previous) <= 0)
                    throw new IOException("invalid lifecycle vtable annex");
                previous = value;
            }
        }
        return new ParsedCore(hex(nativeSha), hex(buildId), imageSize);
    }

    private static boolean allZero(byte[] value) {
        int combined = 0;
        for (byte item : value) combined |= item;
        return combined == 0;
    }

    private static File importedDirectory(Context context) {
        return new File(context.getFilesDir(), "imported-profiles");
    }

    private static void writeAtomic(File output, byte[] bytes) throws IOException {
        File pending = new File(output.getAbsolutePath() + ".pending");
        try (FileOutputStream sink = new FileOutputStream(pending, false)) {
            sink.write(bytes);
            sink.getFD().sync();
        }
        if (output.exists() && !output.delete()) throw new IOException("stale Profile file");
        if (!pending.renameTo(output)) throw new IOException("Profile publish failed");
    }

    private static String hexRva(long value) {
        return "0x" + Long.toUnsignedString(value, 16);
    }

    private static String hex(byte[] bytes) {
        StringBuilder output = new StringBuilder(bytes.length * 2);
        for (byte value : bytes)
            output.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return output.toString();
    }

    Profile find(String nativeSha256) {
        if (nativeSha256 == null) return null;
        return byNativeSha.get(nativeSha256.toLowerCase(Locale.ROOT));
    }

    Profile findById(String id) {
        if (id == null) return null;
        for (Profile profile : profiles) if (profile.id.equals(id)) return profile;
        return null;
    }

    /**
     * Resolves the game build without allowing a stale/manual experimental choice to
     * override an exact identity observed from the currently mapped libAsphalt9.so.
     * The requested id is only a fallback for a build which is genuinely unknown to
     * this registry.
     */
    Profile resolve(String nativeSha256, String requestedId, boolean experimental) {
        Profile exact = find(nativeSha256);
        if (exact != null) return exact;
        return experimental ? findById(requestedId) : null;
    }

    boolean usesExperimentalFallback(String nativeSha256, String requestedId,
                                     boolean experimental) {
        return find(nativeSha256) == null && experimental && findById(requestedId) != null;
    }

    List<Profile> all() { return profiles; }

    int size() { return byNativeSha.size(); }

    private static String exactSha(String value) throws IOException {
        String normalized = value.toLowerCase(Locale.ROOT);
        if (!normalized.matches("[0-9a-f]{64}")) throw new IOException("invalid sha256");
        return normalized;
    }

    private static long exactRva(String value) throws IOException {
        if (value == null || !value.matches("0x[0-9a-fA-F]{1,16}"))
            throw new IOException("invalid practice proof RVA");
        try {
            long rva = Long.parseUnsignedLong(value.substring(2), 16);
            if (rva == 0 || Long.compareUnsigned(rva, 0x40000000L) >= 0)
                throw new IOException("practice proof RVA outside game image");
            return rva;
        } catch (NumberFormatException error) {
            throw new IOException("invalid practice proof RVA", error);
        }
    }

    static byte[] readAll(InputStream input) throws IOException {
        try (InputStream stream = input; ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[16 * 1024];
            int count;
            while ((count = stream.read(buffer)) >= 0) output.write(buffer, 0, count);
            return output.toByteArray();
        }
    }
}
