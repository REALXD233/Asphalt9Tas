package dev.a9tas.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.system.ErrnoException;
import android.system.Os;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;

/** Canonical app-private A9TAS1 writer and recording library. */
final class A9TasLibrary {
    private static final byte[] MAGIC = {'A','9','T','A','S','1',0,0};
    private static final int VERSION = 1;
    private static final int FLAGS = 7;
    // Listing acceleration only. Playback always verifies the selected file.
    private static final LinkedHashMap<String, CachedEntry> LIST_CACHE =
            new LinkedHashMap<>(64, 0.75f, true);
    private static final class CachedEntry {
        final Entry entry;
        final long size, modified;
        CachedEntry(Entry entry) {
            this.entry = entry;
            size = entry.file.length();
            modified = entry.file.lastModified();
        }
    }

    private static Entry remember(Entry entry) {
        synchronized (LIST_CACHE) {
            LIST_CACHE.put(entry.file.getAbsolutePath(), new CachedEntry(entry));
            while (LIST_CACHE.size() > 64)
                LIST_CACHE.remove(LIST_CACHE.keySet().iterator().next());
        }
        return entry;
    }

    private static Entry readEntry(File file, boolean allowListingCache) throws Exception {
        if (allowListingCache) synchronized (LIST_CACHE) {
            CachedEntry cached = LIST_CACHE.get(file.getAbsolutePath());
            if (cached != null && cached.size == file.length() &&
                    cached.modified == file.lastModified()) return cached.entry;
        }
        return remember(new Entry(file, A9TasArchive.inspect(file), sha256(file)));
    }

    static final class Metadata {
        final String title;
        final String createdUtc;
        final String packageName;
        final String gameVersion;
        final String nativeSha256;
        final String buildId;
        final String profileSha256;
        final String mapName;
        final String carName;
        final String controlMode;
        final String notes;

        Metadata(String title, String createdUtc, String packageName, String gameVersion,
                 String nativeSha256, String buildId, String profileSha256,
                 String mapName, String carName, String controlMode, String notes) {
            this.title = title;
            this.createdUtc = createdUtc;
            this.packageName = packageName;
            this.gameVersion = gameVersion;
            this.nativeSha256 = nativeSha256;
            this.buildId = buildId;
            this.profileSha256 = profileSha256;
            this.mapName = mapName;
            this.carName = carName;
            this.controlMode = controlMode;
            this.notes = notes == null ? "" : notes;
        }
    }

    static final class Entry {
        final File file;
        final A9TasArchive.Summary summary;
        final String archiveSha256;

        Entry(File file, A9TasArchive.Summary summary, String archiveSha256) {
            this.file = file;
            this.summary = summary;
            this.archiveSha256 = archiveSha256;
        }

        String label() {
            JSONObject race = summary.manifest.optJSONObject("race");
            String map = race == null ? "?" : race.optString("map", "?");
            String car = race == null ? "?" : race.optString("car", "?");
            return summary.manifest.optString("title", file.getName()) + " · " +
                    summary.frameCount + " ticks · " + map + " / " + car;
        }

        String title() {
            return summary.manifest.optString("title", file.getName());
        }

        String createdUtc() {
            return summary.manifest.optString("created_utc", "");
        }

        String lineageLabel() {
            String notes = summary.manifest.optString("notes", "");
            if (notes.isEmpty()) return "原始录像";
            try {
                JSONObject lineage = new JSONObject(notes);
                String kind = lineage.optString("kind", "");
                long tick = lineage.optLong("source_tick", -1L);
                String parent = lineage.optString("parent_recording_id", "");
                String parentShort = parent.length() >= 8 ? parent.substring(0, 8) : parent;
                if ("branch".equals(kind) && tick >= 0)
                    return "分支 · 父版本 " + parentShort + " · T" + tick;
                if ("trim".equals(kind) && tick >= 0)
                    return "裁剪 · 父版本 " + parentShort + " · T" + tick;
            } catch (Exception ignored) {
                // User-authored legacy notes remain valid but have no structured lineage.
            }
            return "备注录像";
        }

        boolean matches(String query) {
            String clean = query == null ? "" : query.trim().toLowerCase(Locale.ROOT);
            if (clean.isEmpty()) return true;
            JSONObject race = summary.manifest.optJSONObject("race");
            String map = race == null ? "" : race.optString("map", "");
            String car = race == null ? "" : race.optString("car", "");
            String haystack = title() + "\n" + map + "\n" + car + "\n" +
                    lineageLabel() + "\n" +
                    summary.manifest.optString("recording_id", "");
            return haystack.toLowerCase(Locale.ROOT).contains(clean);
        }
    }

    static final class Listing {
        final List<Entry> valid;
        final List<String> invalid;

        Listing(List<Entry> valid, List<String> invalid) {
            this.valid = Collections.unmodifiableList(valid);
            this.invalid = Collections.unmodifiableList(invalid);
        }
    }

    private A9TasLibrary() {}

    static Metadata metadataFromSession(Context context) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        String packageName = preferences.getString("prepared_package", "");
        String nativeSha = preferences.getString("prepared_native_sha", "");
        String profileId = preferences.getString("prepared_profile_id", "");
        boolean experimental = preferences.getBoolean("prepared_experimental_bypass", false);
        BuildProfileRegistry profiles = BuildProfileRegistry.load(context);
        BuildProfileRegistry.Profile profile = profiles.resolve(nativeSha, profileId, experimental);
        if (!safePackage(packageName) || profile == null || !profile.id.equals(profileId))
            throw new IOException("recording BuildProfile identity is unavailable");
        PackageInfo packageInfo = context.getPackageManager().getPackageInfo(packageName, 0);
        String version = packageInfo.versionName == null ?
                Integer.toString(packageInfo.versionCode) : packageInfo.versionName;
        String created = UtcTimestamp.nowSeconds();
        String title = clean(preferences.getString("recording_title", ""), 120,
                "A9 recording " + created);
        String map = clean(preferences.getString("recording_map", ""), 120, "Unknown map");
        String car = clean(preferences.getString("recording_car", ""), 120, "Unknown car");
        String mode = preferences.getString("recording_control_mode", "unknown");
        if (!new HashSet<>(Arrays.asList("manual", "touchdrive", "unknown")).contains(mode))
            throw new IOException("recording control mode is invalid");
        return new Metadata(title, created, packageName, clean(version, 64, "unknown"),
                nativeSha, profile.buildId, profile.profileSha256, map, car, mode, "");
    }

    static Metadata metadataForLatestRaw(Context context) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        if (!preferences.getBoolean("latest_recording_metadata_ready", false))
            return metadataFromSession(context);
        return new Metadata(
                clean(preferences.getString("latest_recording_title", ""), 120, "A9 recording"),
                clean(preferences.getString("latest_recording_created_utc", ""), 32,
                        UtcTimestamp.nowSeconds()),
                requirePackage(preferences.getString("latest_recording_package", "")),
                clean(preferences.getString("latest_recording_game_version", ""), 64, "unknown"),
                requireHex(preferences.getString("latest_recording_native_sha", ""), 64),
                requireHex(preferences.getString("latest_recording_build_id", ""), 40),
                requireHex(preferences.getString("latest_recording_profile_sha", ""), 64),
                clean(preferences.getString("latest_recording_map", ""), 120, "Unknown map"),
                clean(preferences.getString("latest_recording_car", ""), 120, "Unknown car"),
                requireMode(preferences.getString("latest_recording_control_mode", "unknown")), "");
    }

    static void persistRawMetadataSnapshot(SharedPreferences.Editor editor, Metadata metadata) {
        editor.putBoolean("latest_recording_metadata_ready", true)
                .putString("latest_recording_title", metadata.title)
                .putString("latest_recording_created_utc", metadata.createdUtc)
                .putString("latest_recording_package", metadata.packageName)
                .putString("latest_recording_game_version", metadata.gameVersion)
                .putString("latest_recording_native_sha", metadata.nativeSha256)
                .putString("latest_recording_build_id", metadata.buildId)
                .putString("latest_recording_profile_sha", metadata.profileSha256)
                .putString("latest_recording_map", metadata.mapName)
                .putString("latest_recording_car", metadata.carName)
                .putString("latest_recording_control_mode", metadata.controlMode);
    }

    static Entry pack(Context context, File source, String expectedSourceSha,
                      Metadata metadata) throws Exception {
        File rawRoot = new File(context.getFilesDir(), "recordings").getCanonicalFile();
        File raw = source.getCanonicalFile();
        if (!rawRoot.equals(raw.getParentFile()) ||
                !raw.getName().matches("recording-[0-9]+-[0-9]+[.]a9g4r2") || !raw.isFile())
            throw new IOException("raw recording is outside the private library");
        if (expectedSourceSha == null || !expectedSourceSha.matches("[0-9a-f]{64}"))
            throw new IOException("raw recording identity changed before packaging");

        A9TasArchive.SourceSummary sourceSummary = A9TasArchive.inspectSource(raw);
        return packVerifiedSource(context, raw, expectedSourceSha, metadata, sourceSummary);
    }

    /** Packages an already hash-verified private source without copying it first. */
    private static Entry packVerifiedSource(Context context, File raw,
                                            String expectedSourceSha,
                                            Metadata metadata,
                                            A9TasArchive.SourceSummary sourceSummary)
            throws Exception {
        long targetTick = sourceSummary.frameCount - 1;
        String recordingId = UUID.randomUUID().toString();
        JSONObject manifest = manifest(recordingId, metadata, sourceSummary, targetTick);
        byte[] manifestBytes = A9TasArchive.canonical(manifest).getBytes(StandardCharsets.UTF_8);
        if (manifestBytes.length == 0 || manifestBytes.length > 64 * 1024)
            throw new IOException("canonical manifest size is invalid");
        byte[] manifestHash = MessageDigest.getInstance("SHA-256").digest(manifestBytes);
        byte[] recordingHash = fromHex(expectedSourceSha);

        File directory = new File(context.getFilesDir(), "library");
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("unable to create A9TAS library");
        File output = new File(directory, recordingId + ".a9tas");
        File pending = new File(directory, "." + recordingId + ".pending");
        if (output.exists() || pending.exists() && !pending.delete())
            throw new IOException("archive destination collision");

        ByteBuffer header = ByteBuffer.allocate(A9TasArchive.HEADER_SIZE)
                .order(ByteOrder.LITTLE_ENDIAN);
        header.put(MAGIC).putInt(VERSION).putInt(A9TasArchive.HEADER_SIZE).putInt(FLAGS)
                .putInt(manifestBytes.length).putLong(raw.length())
                .putInt(Math.toIntExact(sourceSummary.frameCount))
                .putInt(Math.toIntExact(sourceSummary.intervalCount))
                .putInt(Math.toIntExact(sourceSummary.fixedDeltaUs))
                .putInt(sourceSummary.version).putLong(sourceSummary.sessionId)
                .putInt(Math.toIntExact(sourceSummary.generation))
                .putInt(Math.toIntExact(targetTick)).put(manifestHash).put(recordingHash)
                .put(new byte[32]);
        if (header.position() != A9TasArchive.HEADER_SIZE)
            throw new IOException("A9TAS1 header size mismatch");

        try {
            MessageDigest archiveDigest = MessageDigest.getInstance("SHA-256");
            MessageDigest sourceDigest = MessageDigest.getInstance("SHA-256");
            try (FileOutputStream destination = new FileOutputStream(pending, false);
                 InputStream input = new FileInputStream(raw)) {
                destination.write(header.array());
                destination.write(manifestBytes);
                archiveDigest.update(header.array());
                archiveDigest.update(manifestBytes);
                byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (count != 0) {
                        destination.write(buffer, 0, count);
                        sourceDigest.update(buffer, 0, count);
                        archiveDigest.update(buffer, 0, count);
                    }
                }
                destination.flush();
                destination.getFD().sync();
            }
            if (!MessageDigest.isEqual(recordingHash, sourceDigest.digest()))
                throw new IOException("raw recording identity changed during packaging");
            A9TasArchive.Summary verified = A9TasArchive.inspect(pending);
            if (verified.frameCount != sourceSummary.frameCount ||
                    verified.intervalCount != sourceSummary.intervalCount ||
                    !verified.recordingSha256.equals(expectedSourceSha) ||
                    !recordingId.equals(verified.manifest.getString("recording_id")))
                throw new IOException("packaged archive identity mismatch");
            if (!pending.renameTo(output)) throw new IOException("atomic archive publish failed");
            // renameTo within the same private directory preserves the bytes
            // already verified above.  Re-parsing every frame after publication
            // only duplicated I/O on slow Android storage.
            return remember(new Entry(output, verified, hex(archiveDigest.digest())));
        } catch (Exception error) {
            if (pending.exists()) pending.delete();
            throw error;
        }
    }

    /**
     * Atomically promotes a sealed pause checkpoint into the canonical library.
     * The draft remains untouched until both the raw copy and A9TAS1 archive
     * have been verified, so removing the old confirmation card never turns a
     * UI shortcut into silent data loss.
     */
    static Entry promoteDraft(Context context, File source, String expectedSourceSha,
                              int expectedTicks, Metadata metadata) throws Exception {
        File draftRoot = new File(context.getFilesDir(), "drafts").getCanonicalFile();
        File draft = source.getCanonicalFile();
        if (!draftRoot.equals(draft.getParentFile()) ||
                !draft.getName().matches("attempt-[0-9]+-[0-9]+[.]a9g4r2") ||
                !draft.isFile())
            throw new IOException("checkpoint is outside the private draft directory");
        if (expectedSourceSha == null || !expectedSourceSha.matches("[0-9a-f]{64}"))
            throw new IOException("checkpoint identity changed before promotion");
        A9TasArchive.SourceSummary summary = A9TasArchive.inspectSource(draft);
        if (expectedTicks < 1 || summary.frameCount != expectedTicks)
            throw new IOException("checkpoint Tick count changed before promotion");
        Entry archived = null;
        try {
            archived = packVerifiedSource(context, draft, expectedSourceSha,
                    metadata, summary);
            if (draft.isFile() && !draft.delete())
                throw new IOException("promoted checkpoint draft could not be retired");
            return archived;
        } catch (Exception error) {
            if (archived != null && archived.file.isFile()) archived.file.delete();
            throw error;
        }
    }

    static Listing list(Context context) throws Exception {
        File directory = new File(context.getFilesDir(), "library").getCanonicalFile();
        if (!directory.exists()) return new Listing(new ArrayList<>(), new ArrayList<>());
        if (!directory.isDirectory()) throw new IOException("A9TAS library is not a directory");
        File[] files = directory.listFiles((parent, name) -> name.endsWith(".a9tas"));
        if (files == null) throw new IOException("unable to enumerate A9TAS library");
        Arrays.sort(files, Comparator.comparingLong(File::lastModified).reversed());
        List<Entry> valid = new ArrayList<>();
        List<String> invalid = new ArrayList<>();
        Set<String> ids = new HashSet<>();
        for (File file : files) {
            try {
                Entry entry = readEntry(file, true);
                A9TasArchive.Summary summary = entry.summary;
                String id = summary.manifest.getString("recording_id");
                if (!ids.add(id)) throw new IOException("library.duplicate_recording_id");
                valid.add(entry);
            } catch (Exception error) {
                invalid.add(file.getName() + ":" +
                        (error.getMessage() == null ? error.getClass().getSimpleName() : error.getMessage()));
            }
        }
        return new Listing(valid, invalid);
    }

    static List<Entry> view(List<Entry> source, String query, String order) {
        List<Entry> result = new ArrayList<>();
        if (source != null) for (Entry entry : source)
            if (entry != null && entry.matches(query)) result.add(entry);
        Comparator<Entry> comparator;
        if ("oldest".equals(order)) {
            comparator = Comparator.comparing(Entry::createdUtc)
                    .thenComparing(entry -> entry.file.getName());
        } else if ("name_asc".equals(order)) {
            comparator = Comparator.comparing(Entry::title, String.CASE_INSENSITIVE_ORDER)
                    .thenComparing(Comparator.comparing(Entry::createdUtc).reversed());
        } else if ("name_desc".equals(order)) {
            comparator = Comparator.comparing(Entry::title, String.CASE_INSENSITIVE_ORDER)
                    .reversed().thenComparing(
                            Comparator.comparing(Entry::createdUtc).reversed());
        } else if ("longest".equals(order)) {
            comparator = Comparator.comparingLong((Entry entry) -> entry.summary.frameCount)
                    .reversed().thenComparing(
                            Comparator.comparing(Entry::createdUtc).reversed());
        } else if ("shortest".equals(order)) {
            comparator = Comparator.comparingLong((Entry entry) -> entry.summary.frameCount)
                    .thenComparing(Comparator.comparing(Entry::createdUtc).reversed());
        } else {
            comparator = Comparator.comparing(Entry::createdUtc).reversed()
                    .thenComparing(entry -> entry.file.getName());
        }
        result.sort(comparator);
        return result;
    }

    static int trashBatch(Context context, List<Entry> entries) throws Exception {
        if (entries == null || entries.isEmpty()) throw new IOException("没有可移入回收站的录像");
        File library = new File(context.getFilesDir(), "library").getCanonicalFile();
        File trashRoot = new File(context.getFilesDir(), "library-trash").getCanonicalFile();
        if (!trashRoot.isDirectory() && !trashRoot.mkdirs())
            throw new IOException("无法创建录像回收站");
        File batch = new File(trashRoot, "batch-" + System.currentTimeMillis());
        if (!batch.mkdir()) throw new IOException("无法创建批量删除事务");

        List<File> moved = new ArrayList<>();
        try {
            Set<String> paths = new HashSet<>();
            for (Entry entry : entries) {
                File source = entry.file.getCanonicalFile();
                if (!library.equals(source.getParentFile()) || !source.isFile() ||
                        !entry.archiveSha256.equals(sha256(source)) ||
                        !paths.add(source.getAbsolutePath()))
                    throw new IOException("批量删除前录像身份发生变化");
                A9TasArchive.Summary verified = A9TasArchive.inspect(source);
                if (!entry.summary.manifest.getString("recording_id").equals(
                        verified.manifest.getString("recording_id")))
                    throw new IOException("批量删除前录像版本发生变化");
                File destination = new File(batch, source.getName());
                if (destination.exists()) throw new IOException("回收站目标冲突");
            }
            for (Entry entry : entries) {
                File source = entry.file.getCanonicalFile();
                File destination = new File(batch, source.getName());
                Os.rename(source.getAbsolutePath(), destination.getAbsolutePath());
                if (source.exists() || !destination.isFile())
                    throw new IOException("录像移入回收站未提交");
                moved.add(destination);
            }
            SharedPreferences preferences = context.getSharedPreferences(
                    "session", Context.MODE_PRIVATE);
            SharedPreferences.Editor editor = preferences.edit()
                    .putString("last_trash_batch", batch.getAbsolutePath())
                    .putInt("last_trash_count", moved.size());
            for (Entry entry : entries) {
                String path = entry.file.getAbsolutePath();
                if (path.equals(preferences.getString("selected_archive", "")))
                    editor.remove("selected_archive").remove("selected_archive_sha");
                if (path.equals(preferences.getString("latest_archive", "")))
                    editor.remove("latest_archive").remove("latest_archive_sha")
                            .remove("latest_archive_id");
            }
            if (!editor.commit()) throw new IOException("录像回收站状态发布失败");
            return moved.size();
        } catch (Exception error) {
            for (int index = moved.size() - 1; index >= 0; --index) {
                File destination = moved.get(index);
                File source = new File(library, destination.getName());
                try {
                    if (destination.isFile() && !source.exists())
                        Os.rename(destination.getAbsolutePath(), source.getAbsolutePath());
                } catch (Exception ignored) {}
            }
            batch.delete();
            throw error;
        }
    }

    static int restoreLastTrashBatch(Context context) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        String path = preferences.getString("last_trash_batch", "");
        File trashRoot = new File(context.getFilesDir(), "library-trash").getCanonicalFile();
        File library = new File(context.getFilesDir(), "library").getCanonicalFile();
        File batch = new File(path).getCanonicalFile();
        if (!trashRoot.equals(batch.getParentFile()) || !batch.getName().matches("batch-[0-9]+") ||
                !batch.isDirectory()) throw new IOException("没有可撤销的批量删除");
        File[] files = batch.listFiles((parent, name) -> name.matches("[0-9a-f-]{36}[.]a9tas"));
        if (files == null || files.length == 0) throw new IOException("回收站批次为空");
        for (File file : files) {
            A9TasArchive.inspect(file);
            if (new File(library, file.getName()).exists())
                throw new IOException("录像库中已存在同 ID 版本，无法撤销");
        }
        List<File> restored = new ArrayList<>();
        try {
            for (File file : files) {
                File destination = new File(library, file.getName());
                Os.rename(file.getAbsolutePath(), destination.getAbsolutePath());
                restored.add(destination);
            }
        } catch (Exception error) {
            for (int index = restored.size() - 1; index >= 0; --index) {
                File destination = restored.get(index);
                try {
                    Os.rename(destination.getAbsolutePath(),
                            new File(batch, destination.getName()).getAbsolutePath());
                } catch (Exception ignored) {}
            }
            throw error;
        }
        if (!batch.delete()) throw new IOException("回收站批次清理失败");
        if (!preferences.edit().remove("last_trash_batch").remove("last_trash_count").commit())
            throw new IOException("撤销状态发布失败");
        return restored.size();
    }

    static int lastTrashCount(Context context) {
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        String path = preferences.getString("last_trash_batch", "");
        int count = preferences.getInt("last_trash_count", 0);
        return count > 0 && !path.isEmpty() && new File(path).isDirectory() ? count : 0;
    }

    static Entry rename(Context context, Entry entry, String requestedTitle) throws Exception {
        JSONObject manifest = entry.summary.manifest;
        JSONObject race = manifest.getJSONObject("race");
        return editMetadata(context, entry, requestedTitle,
                race.getString("map"), race.getString("car"),
                race.getString("control_mode"), manifest.optString("notes", ""));
    }

    /**
     * Rewrites only user-editable manifest fields while preserving the
     * authenticated recording bytes, recording id and source SHA-256.
     */
    static Entry editMetadata(Context context, Entry entry, String requestedTitle,
                              String requestedMap, String requestedCar,
                              String requestedControlMode, String requestedNotes)
            throws Exception {
        String title = clean(requestedTitle, 120, "A9 recording");
        String map = clean(requestedMap, 120, "Unknown map");
        String car = clean(requestedCar, 120, "Unknown car");
        String controlMode = requireMode(requestedControlMode);
        String notes = cleanOptional(requestedNotes, 2_000);

        JSONObject manifest = new JSONObject(entry.summary.manifest.toString());
        JSONObject race = manifest.getJSONObject("race");
        manifest.put("title", title).put("notes", notes);
        race.put("map", map).put("car", car).put("control_mode", controlMode);
        return rewriteManifest(context, entry, manifest);
    }

    private static Entry rewriteManifest(Context context, Entry entry,
                                         JSONObject manifest) throws Exception {
        File directory = new File(context.getFilesDir(), "library").getCanonicalFile();
        File archive = entry.file.getCanonicalFile();
        if (!directory.equals(archive.getParentFile()) ||
                !archive.getName().matches("[0-9a-f-]{36}[.]a9tas") || !archive.isFile())
            throw new IOException("recording is outside the private library");
        if (!entry.archiveSha256.equals(sha256(archive)))
            throw new IOException("recording changed before rename");

        A9TasArchive.Summary before = entry.summary;
        if (!before.manifest.getString("recording_id").equals(
                manifest.getString("recording_id")))
            throw new IOException("recording id cannot be edited");
        byte[] manifestBytes = A9TasArchive.canonical(manifest)
                .getBytes(StandardCharsets.UTF_8);
        if (manifestBytes.length == 0 || manifestBytes.length > 64 * 1024)
            throw new IOException("canonical manifest size is invalid");

        byte[] manifestHash = MessageDigest.getInstance("SHA-256").digest(manifestBytes);
        byte[] recordingHash = fromHex(before.recordingSha256);
        long recordingOffset;
        long recordingSize;
        try (RandomAccessFile source = new RandomAccessFile(archive, "r")) {
            byte[] headerBytes = new byte[A9TasArchive.HEADER_SIZE];
            source.readFully(headerBytes);
            ByteBuffer oldHeader = ByteBuffer.wrap(headerBytes).order(ByteOrder.LITTLE_ENDIAN);
            oldHeader.position(20);
            long oldManifestSize = Integer.toUnsignedLong(oldHeader.getInt());
            recordingSize = oldHeader.getLong();
            recordingOffset = A9TasArchive.HEADER_SIZE + oldManifestSize;
            if (recordingSize <= 0 || recordingOffset + recordingSize != source.length())
                throw new IOException("recording layout changed before rename");
        }

        ByteBuffer header = ByteBuffer.allocate(A9TasArchive.HEADER_SIZE)
                .order(ByteOrder.LITTLE_ENDIAN);
        header.put(MAGIC).putInt(VERSION).putInt(A9TasArchive.HEADER_SIZE).putInt(FLAGS)
                .putInt(manifestBytes.length).putLong(recordingSize)
                .putInt(Math.toIntExact(before.frameCount))
                .putInt(Math.toIntExact(before.intervalCount))
                .putInt(Math.toIntExact(before.fixedDeltaUs)).putInt(before.sourceVersion)
                .putLong(before.sessionId).putInt(Math.toIntExact(before.generation))
                .putInt(Math.toIntExact(before.targetTick)).put(manifestHash).put(recordingHash)
                .put(new byte[32]);
        if (header.position() != A9TasArchive.HEADER_SIZE)
            throw new IOException("A9TAS1 header size mismatch");

        File pending = new File(directory, "." + archive.getName() + ".metadata.pending");
        if (pending.exists() && !pending.delete())
            throw new IOException("stale metadata transaction cannot be cleared");
        try {
            MessageDigest archiveDigest = MessageDigest.getInstance("SHA-256");
            try (RandomAccessFile source = new RandomAccessFile(archive, "r");
                 FileOutputStream destination = new FileOutputStream(pending, false)) {
                destination.write(header.array());
                destination.write(manifestBytes);
                archiveDigest.update(header.array());
                archiveDigest.update(manifestBytes);
                source.seek(recordingOffset);
                byte[] buffer = new byte[64 * 1024];
                long remaining = recordingSize;
                while (remaining > 0) {
                    int count = source.read(buffer, 0, (int) Math.min(buffer.length, remaining));
                    if (count < 0) throw new IOException("recording ended during metadata edit");
                    destination.write(buffer, 0, count);
                    archiveDigest.update(buffer, 0, count);
                    remaining -= count;
                }
                destination.flush();
                destination.getFD().sync();
            }
            A9TasArchive.Summary verified = A9TasArchive.inspect(pending);
            if (!A9TasArchive.canonical(manifest).equals(
                        A9TasArchive.canonical(verified.manifest)) ||
                    !before.manifest.getString("recording_id").equals(
                            verified.manifest.getString("recording_id")) ||
                    !before.recordingSha256.equals(verified.recordingSha256) ||
                    before.frameCount != verified.frameCount)
                throw new IOException("edited archive identity mismatch");
            atomicReplace(pending, archive);
            Entry result = remember(new Entry(archive, verified, hex(archiveDigest.digest())));
            if (!A9TasArchive.canonical(manifest).equals(
                        A9TasArchive.canonical(result.summary.manifest)) ||
                    !before.recordingSha256.equals(result.summary.recordingSha256))
                throw new IOException("published metadata identity mismatch");
            return result;
        } finally {
            if (pending.exists()) pending.delete();
        }
    }

    static void delete(Context context, Entry entry) throws Exception {
        File directory = new File(context.getFilesDir(), "library").getCanonicalFile();
        File archive = entry.file.getCanonicalFile();
        if (!directory.equals(archive.getParentFile()) ||
                !archive.getName().matches("[0-9a-f-]{36}[.]a9tas") || !archive.isFile())
            throw new IOException("recording is outside the private library");
        if (!entry.archiveSha256.equals(sha256(archive)))
            throw new IOException("recording changed before delete");
        A9TasArchive.Summary verified = A9TasArchive.inspect(archive);
        if (!entry.summary.manifest.getString("recording_id").equals(
                verified.manifest.getString("recording_id")))
            throw new IOException("recording identity changed before delete");
        if (!archive.delete()) throw new IOException("recording delete failed");
        if (archive.exists()) throw new IOException("recording delete was not committed");

        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        SharedPreferences.Editor editor = preferences.edit();
        String path = archive.getAbsolutePath();
        if (path.equals(preferences.getString("selected_archive", "")))
            editor.remove("selected_archive").remove("selected_archive_sha");
        if (path.equals(preferences.getString("latest_archive", "")))
            editor.remove("latest_archive").remove("latest_archive_sha")
                    .remove("latest_archive_id");
        if (!editor.commit()) throw new IOException("recording selection cleanup failed");
    }

    private static void atomicReplace(File pending, File destination) throws IOException {
        try {
            // Both paths are canonical siblings in the app-private library. rename(2)
            // atomically replaces the destination using an API available on Android 7.
            Os.rename(pending.getAbsolutePath(), destination.getAbsolutePath());
        } catch (ErrnoException error) {
            throw new IOException("atomic recording publish failed", error);
        }
        if (pending.exists() || !destination.isFile())
            throw new IOException("atomic recording publish was not committed");
    }

    static File materializeReplaySource(Context context, Entry entry,
                                        long targetTick) throws Exception {
        if (entry == null) throw new IOException("no A9TAS1 recording is selected");
        File library = new File(context.getFilesDir(), "library").getCanonicalFile();
        File archive = entry.file.getCanonicalFile();
        if (!library.equals(archive.getParentFile()) || !archive.isFile() ||
                !entry.archiveSha256.equals(sha256(archive)))
            throw new IOException("selected archive identity changed");
        A9TasArchive.Summary summary = A9TasArchive.inspect(archive);
        if (targetTick < 0 || targetTick >= summary.frameCount)
            throw new IOException("target tick is outside the recording");

        File directory = new File(context.getFilesDir(), "replay-staging");
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("unable to create replay staging directory");
        File output = new File(directory, summary.recordingSha256 + ".a9g4r2");
        if (output.isFile() && summary.recordingSha256.equals(sha256(output))) {
            A9TasArchive.SourceSummary source = A9TasArchive.inspectSource(output);
            if (source.frameCount == summary.frameCount &&
                    source.intervalCount == summary.intervalCount)
                return materializeTargetPrefix(output, summary, targetTick, directory);
        }
        File pending = new File(output.getAbsolutePath() + ".pending");
        if (pending.exists() && !pending.delete())
            throw new IOException("stale replay staging file cannot be removed");
        try {
            try (RandomAccessFile input = new RandomAccessFile(archive, "r")) {
                input.seek(20);
                byte[] lengths = new byte[12];
                input.readFully(lengths);
                ByteBuffer size = ByteBuffer.wrap(lengths).order(ByteOrder.LITTLE_ENDIAN);
                long manifestSize = Integer.toUnsignedLong(size.getInt());
                long recordingSize = size.getLong();
                if (manifestSize < 1 || manifestSize > 64 * 1024 ||
                        recordingSize < 1 || recordingSize > 512L * 1024L * 1024L)
                    throw new IOException("archive lengths changed before replay extraction");
                input.seek(A9TasArchive.HEADER_SIZE + manifestSize);
                try (FileOutputStream destination = new FileOutputStream(pending, false)) {
                    byte[] buffer = new byte[64 * 1024];
                    long remaining = recordingSize;
                    while (remaining > 0) {
                        int count = input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
                        if (count < 0) throw new IOException("archive ended during replay extraction");
                        if (count != 0) {
                            destination.write(buffer, 0, count);
                            remaining -= count;
                        }
                    }
                    destination.flush();
                    destination.getFD().sync();
                }
            }
            if (!summary.recordingSha256.equals(sha256(pending)))
                throw new IOException("materialized replay source hash mismatch");
            A9TasArchive.SourceSummary source = A9TasArchive.inspectSource(pending);
            if (source.frameCount != summary.frameCount ||
                    source.intervalCount != summary.intervalCount)
                throw new IOException("materialized replay source identity mismatch");
            if (output.exists() && !output.delete())
                throw new IOException("old replay staging source cannot be replaced");
            if (!pending.renameTo(output)) throw new IOException("replay staging publish failed");
            return materializeTargetPrefix(output, summary, targetTick, directory);
        } catch (Exception error) {
            if (pending.exists()) pending.delete();
            throw error;
        }
    }

    /**
     * Builds an exact A9G4R2 prefix for AluTasV2's inclusive target tick.
     * The canonical A9TAS1 archive and its full raw payload remain unchanged.
     */
    private static File materializeTargetPrefix(File full,
                                                A9TasArchive.Summary summary,
                                                long targetTick,
                                                File directory) throws Exception {
        long targetFramesLong = targetTick + 1;
        if (targetFramesLong == summary.frameCount) return full;
        if (targetFramesLong <= 0 || targetFramesLong > summary.frameCount)
            throw new IOException("target tick is outside the recording");
        int targetFrames = Math.toIntExact(targetFramesLong);
        File output = new File(directory, summary.recordingSha256 + "-target-" +
                targetTick + ".a9g4r2");
        if (output.isFile()) {
            try {
                A9TasArchive.SourceSummary cached = A9TasArchive.inspectSource(output);
                if (cached.frameCount == targetFrames) return output;
            } catch (Exception ignored) {
                // Replace an incomplete or stale derived view below.
            }
        }

        int intervalCount = 0;
        long intervalOffset = A9TasArchive.A9G4R2_HEADER_SIZE +
                summary.frameCount * A9TasArchive.A9G4R2_FRAME_SIZE;
        byte[] interval = new byte[A9TasArchive.A9G4R2_INTERVAL_SIZE];
        boolean sparseIntervals;
        try (RandomAccessFile input = new RandomAccessFile(full, "r")) {
            // full has already passed archive/source validation. Read its
            // explicit format version without rescanning every frame.
            input.seek(8);
            sparseIntervals = Integer.reverseBytes(input.readInt()) == 4;
            input.seek(intervalOffset);
            for (long index = 0; index < summary.intervalCount; ++index) {
                input.readFully(interval);
                long tick = ByteBuffer.wrap(interval).order(ByteOrder.LITTLE_ENDIAN).getLong();
                if (tick >= targetFrames) break;
                intervalCount++;
            }
        }
        if (intervalCount == 0 && !sparseIntervals)
            throw new IOException("target prefix has no Physics Interval samples");

        File pending = new File(output.getAbsolutePath() + ".pending");
        if (pending.exists() && !pending.delete())
            throw new IOException("stale target replay staging file cannot be removed");
        try {
            try (RandomAccessFile input = new RandomAccessFile(full, "r");
                 FileOutputStream destination = new FileOutputStream(pending, false)) {
                byte[] header = new byte[A9TasArchive.A9G4R2_HEADER_SIZE];
                input.readFully(header);
                ByteBuffer edited = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
                edited.putInt(24, targetFrames);
                edited.putInt(28, intervalCount);
                destination.write(header);
                copyExact(input, destination,
                        (long) targetFrames * A9TasArchive.A9G4R2_FRAME_SIZE);
                input.seek(intervalOffset);
                copyExact(input, destination,
                        (long) intervalCount * A9TasArchive.A9G4R2_INTERVAL_SIZE);
                destination.flush();
                destination.getFD().sync();
            }
            A9TasArchive.SourceSummary derived = A9TasArchive.inspectSource(pending);
            if (derived.frameCount != targetFrames ||
                    derived.intervalCount != intervalCount)
                throw new IOException("target replay prefix identity mismatch");
            if (output.exists() && !output.delete())
                throw new IOException("old target replay prefix cannot be replaced");
            if (!pending.renameTo(output))
                throw new IOException("target replay prefix publish failed");
            return output;
        } catch (Exception error) {
            if (pending.exists()) pending.delete();
            throw error;
        }
    }

    private static void copyExact(RandomAccessFile input, FileOutputStream output,
                                  long byteCount) throws Exception {
        byte[] buffer = new byte[64 * 1024];
        long remaining = byteCount;
        while (remaining > 0) {
            int count = input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
            if (count < 0) throw new IOException("source ended while deriving target replay");
            if (count != 0) {
                output.write(buffer, 0, count);
                remaining -= count;
            }
        }
    }

    static Entry selected(Context context) throws Exception {
        SharedPreferences preferences = context.getSharedPreferences("session", Context.MODE_PRIVATE);
        String selectedPath = preferences.getString("selected_archive",
                preferences.getString("latest_archive", ""));
        String selectedSha = preferences.getString("selected_archive_sha",
                preferences.getString("latest_archive_sha", ""));
        if (selectedPath.isEmpty() || !selectedSha.matches("[0-9a-f]{64}"))
            throw new IOException("select a verified A9TAS1 recording first");
        return verified(context, selectedPath, selectedSha);
    }

    static Entry verified(Context context, String archivePath,
                          String archiveSha256) throws Exception {
        if (archivePath == null || archivePath.isEmpty() ||
                archiveSha256 == null || !archiveSha256.matches("[0-9a-f]{64}"))
            throw new IOException("resident replay archive identity is incomplete");
        File requested = new File(archivePath).getCanonicalFile();
        File directory = new File(context.getFilesDir(), "library").getCanonicalFile();
        if (!directory.equals(requested.getParentFile()) ||
                !requested.getName().matches("[0-9a-f-]{36}[.]a9tas") || !requested.isFile())
            throw new IOException("recording is outside the private library");
        Entry entry = readEntry(requested, false);
        if (entry.archiveSha256.equals(archiveSha256)) return entry;
        throw new IOException("resident replay archive is no longer valid");
    }

    static File latestRaw(Context context) throws Exception {
        File directory = new File(context.getFilesDir(), "recordings").getCanonicalFile();
        File[] files = directory.listFiles((parent, name) ->
                name.matches("recording-[0-9]+-[0-9]+[.]a9g4r2"));
        if (files == null || files.length == 0) return null;
        Arrays.sort(files, Comparator.comparingLong(File::lastModified).reversed());
        return files[0];
    }

    static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = new FileInputStream(file)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) if (count != 0) digest.update(buffer, 0, count);
        }
        StringBuilder result = new StringBuilder(64);
        for (byte value : digest.digest())
            result.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return result.toString();
    }

    private static JSONObject manifest(String id, Metadata metadata,
                                       A9TasArchive.SourceSummary source, long target) throws Exception {
        JSONObject game = new JSONObject().put("package", metadata.packageName)
                .put("version", metadata.gameVersion)
                .put("native_sha256", metadata.nativeSha256)
                .put("build_id", metadata.buildId)
                .put("build_profile_sha256", metadata.profileSha256);
        JSONObject race = new JSONObject().put("map", metadata.mapName)
                .put("car", metadata.carName).put("control_mode", metadata.controlMode);
        JSONObject recording = new JSONObject().put("format", "A9G4R2")
                .put("format_version", source.version).put("frame_count", source.frameCount)
                .put("interval_count", source.intervalCount)
                .put("fixed_delta_us", source.fixedDeltaUs).put("session_id", source.sessionId)
                .put("generation", source.generation).put("target_tick", target);
        return new JSONObject().put("schema", "a9tas.recording.v1")
                .put("recording_id", id).put("title", metadata.title)
                .put("created_utc", metadata.createdUtc).put("game", game)
                .put("race", race).put("recording", recording)
                .put("notes", cleanOptional(metadata.notes, 2_000));
    }

    private static String clean(String value, int maximum, String fallback) throws IOException {
        String result = value == null || value.trim().isEmpty() ? fallback : value.trim();
        if (result.length() > maximum) throw new IOException("recording metadata is too long");
        for (int index = 0; index < result.length(); ++index)
            if (result.charAt(index) < 0x20) throw new IOException("recording metadata has control characters");
        return result;
    }

    private static String cleanOptional(String value, int maximum) throws IOException {
        String result = value == null ? "" : value.trim();
        if (result.length() > maximum) throw new IOException("recording notes are too long");
        for (int index = 0; index < result.length(); ++index)
            if (result.charAt(index) < 0x20)
                throw new IOException("recording notes have control characters");
        return result;
    }

    private static byte[] fromHex(String value) throws IOException {
        if (value == null || !value.matches("[0-9a-f]{64}")) throw new IOException("invalid SHA-256");
        byte[] result = new byte[32];
        for (int index = 0; index < result.length; ++index)
            result[index] = (byte) Integer.parseInt(value.substring(index * 2, index * 2 + 2), 16);
        return result;
    }

    private static String hex(byte[] bytes) {
        char[] out = new char[bytes.length * 2];
        final char[] digits = "0123456789abcdef".toCharArray();
        for (int i = 0; i < bytes.length; ++i) {
            out[i * 2] = digits[(bytes[i] & 255) >>> 4];
            out[i * 2 + 1] = digits[bytes[i] & 15];
        }
        return new String(out);
    }

    private static boolean safePackage(String value) {
        return value != null && value.matches("[A-Za-z0-9_]+(?:[.][A-Za-z0-9_]+)+");
    }

    private static String requirePackage(String value) throws IOException {
        if (!safePackage(value)) throw new IOException("recording package snapshot is invalid");
        return value;
    }

    private static String requireHex(String value, int length) throws IOException {
        String result = value == null ? "" : value.toLowerCase(Locale.ROOT);
        if (!result.matches("[0-9a-f]{" + length + "}"))
            throw new IOException("recording build snapshot is invalid");
        return result;
    }

    private static String requireMode(String value) throws IOException {
        if (!new HashSet<>(Arrays.asList("manual", "touchdrive", "unknown")).contains(value))
            throw new IOException("recording control snapshot is invalid");
        return value;
    }
}
