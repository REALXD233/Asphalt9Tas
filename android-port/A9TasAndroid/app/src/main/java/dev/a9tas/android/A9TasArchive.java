package dev.a9tas.android;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.EOFException;
import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;

/** Strict file-backed decoder for the canonical G9 A9TAS1 container. */
final class A9TasArchive {
    static final int HEADER_SIZE = 160;
    static final int A9G4R2_HEADER_SIZE = 64;
    static final int A9G4R2_FRAME_SIZE = 144;
    static final int A9G4R2_INTERVAL_SIZE = 16;
    static final int TARGET_END = -1;
    private static final long MAX_MANIFEST = 64L * 1024L;
    private static final long MAX_RECORDING = 512L * 1024L * 1024L;
    private static final byte[] MAGIC = {'A','9','T','A','S','1',0,0};
    private static final byte[] SOURCE_MAGIC = {'A','9','G','4','R','2',0,0};

    static final class Summary {
        final int sourceVersion;
        final long frameCount;
        final long intervalCount;
        final long fixedDeltaUs;
        final long sessionId;
        final long generation;
        final long targetTick;
        final long nitroCalls;
        final long barrelFrames;
        final String recordingSha256;
        final JSONObject manifest;

        Summary(int sourceVersion, long frameCount, long intervalCount,
                long fixedDeltaUs, long sessionId, long generation, long targetTick,
                long nitroCalls, long barrelFrames, String recordingSha256,
                JSONObject manifest) {
            this.sourceVersion = sourceVersion;
            this.frameCount = frameCount;
            this.intervalCount = intervalCount;
            this.fixedDeltaUs = fixedDeltaUs;
            this.sessionId = sessionId;
            this.generation = generation;
            this.targetTick = targetTick;
            this.nitroCalls = nitroCalls;
            this.barrelFrames = barrelFrames;
            this.recordingSha256 = recordingSha256;
            this.manifest = manifest;
        }
    }

    private A9TasArchive() {}

    static Summary inspect(File file) throws Exception {
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            if (input.length() < HEADER_SIZE) fail("archive.short_header");
            ByteBuffer header = little(readExact(input, HEADER_SIZE));
            requireMagic(header, MAGIC, "archive.header_identity");
            long version = u32(header), headerSize = u32(header), flags = u32(header);
            long manifestSize = u32(header), recordingSize = header.getLong();
            long frameCount = u32(header), intervalCount = u32(header);
            long fixedDeltaUs = u32(header), sourceVersion = u32(header);
            long sessionId = header.getLong(), generation = u32(header), targetTick = u32(header);
            byte[] manifestHash = take(header, 32), recordingHash = take(header, 32);
            byte[] reserved = take(header, 32);
            if (version != 1 || headerSize != HEADER_SIZE || flags != 7 ||
                    manifestSize < 1 || manifestSize > MAX_MANIFEST ||
                    recordingSize < 1 || recordingSize > MAX_RECORDING ||
                    !allZero(reserved) || input.length() != HEADER_SIZE + manifestSize + recordingSize)
                fail("archive.header_identity");

            byte[] manifestBytes = readExact(input, Math.toIntExact(manifestSize));
            if (!MessageDigest.isEqual(hash(manifestBytes), manifestHash)) fail("archive.manifest_hash");
            long recordingOffset = HEADER_SIZE + manifestSize;
            if (!MessageDigest.isEqual(hashRange(input, recordingOffset, recordingSize), recordingHash))
                fail("archive.recording_hash");
            String manifestText = strictUtf8(manifestBytes);
            JSONObject manifest = new JSONObject(manifestText);
            if (!canonical(manifest).equals(manifestText)) fail("archive.manifest_not_canonical");

            SourceSummary source = inspectSource(input, recordingOffset, recordingSize);
            if (frameCount != source.frameCount || intervalCount != source.intervalCount ||
                    fixedDeltaUs != source.fixedDeltaUs || sourceVersion != source.version ||
                    sessionId != source.sessionId || generation != source.generation ||
                    targetTick >= source.frameCount)
                fail("archive.source_identity");
            validateManifest(manifest, source, targetTick);
            return new Summary(source.version, source.frameCount, source.intervalCount,
                    source.fixedDeltaUs, source.sessionId, source.generation, targetTick,
                    source.nitroCalls, source.barrelFrames, hex(recordingHash), manifest);
        }
    }

    static final class SourceSummary {
        int version;
        long frameCount, intervalCount, fixedDeltaUs, sessionId, generation;
        long nitroCalls, barrelFrames;
    }

    static SourceSummary inspectSource(File file) throws Exception {
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            if (input.length() < A9G4R2_HEADER_SIZE || input.length() > MAX_RECORDING)
                fail("a9g4r2.file_size");
            return inspectSource(input, 0, input.length());
        }
    }

    private static SourceSummary inspectSource(RandomAccessFile input, long offset, long size)
            throws Exception {
        input.seek(offset);
        ByteBuffer header = little(readExact(input, A9G4R2_HEADER_SIZE));
        requireMagic(header, SOURCE_MAGIC, "a9g4r2.header_identity");
        SourceSummary source = new SourceSummary();
        source.version = Math.toIntExact(u32(header));
        long headerSize = u32(header), frameSize = u32(header), intervalSize = u32(header);
        source.frameCount = u32(header);
        source.intervalCount = u32(header);
        source.fixedDeltaUs = u32(header);
        long flags = u32(header);
        source.sessionId = header.getLong();
        source.generation = u32(header);
        long reserved0 = u32(header);
        byte[] reserved = take(header, 8);
        boolean legacy = source.version == 2 && flags == 0x0f;
        boolean current = source.version == 3 && flags == 0x1f;
        boolean sparse = source.version == 4 && flags == 0x3f &&
                (source.fixedDeltaUs == 8333 || source.fixedDeltaUs == 6944);
        long expected = A9G4R2_HEADER_SIZE + source.frameCount * A9G4R2_FRAME_SIZE +
                source.intervalCount * A9G4R2_INTERVAL_SIZE;
        if (!(legacy || current || sparse) || headerSize != A9G4R2_HEADER_SIZE ||
                frameSize != A9G4R2_FRAME_SIZE || intervalSize != A9G4R2_INTERVAL_SIZE ||
                source.frameCount == 0 || (source.intervalCount == 0 && !sparse) ||
                source.fixedDeltaUs < 1_000 || source.fixedDeltaUs > 100_000 ||
                source.sessionId == 0 || source.generation == 0 || reserved0 != 0 ||
                !allZero(reserved) || expected != size)
            fail("a9g4r2.header_identity");

        for (long index = 0; index < source.frameCount; ++index) {
            ByteBuffer frame = little(readExact(input, A9G4R2_FRAME_SIZE));
            long tick = frame.getLong(), monotonicNs = frame.getLong();
            float steering = frame.getFloat(), brake = frame.getFloat(), accelerator = frame.getFloat();
            long nitro = u32(frame), skip = u32(frame);
            int respawn = frame.get() & 0xff;
            byte[] padding = take(frame, 3);
            float ax = frame.getFloat(), ay = frame.getFloat(), az = frame.getFloat();
            float rbx0 = frame.getFloat(), rbx1 = frame.getFloat();
            byte[] transform = take(frame, 64), linear = take(frame, 12);
            long frameFlags = u32(frame), frameReserved = u32(frame);
            boolean barrelFinite = finite(ax) && finite(ay) && finite(az) && finite(rbx0) && finite(rbx1);
            if (tick != index || monotonicNs != index * source.fixedDeltaUs * 1_000L ||
                    !bounded(steering, 1.0f) || !bounded(brake, 1.05f) ||
                    Float.floatToRawIntBits(accelerator) != 0 || nitro > 2 ||
                    skip != (legacy ? 0x78 : 0x48) || respawn != 0 || !allZero(padding) ||
                    (legacy && (ax != 0 || ay != 0 || az != 0 || rbx0 != 0 || rbx1 != 0)) ||
                    ((current || sparse) && !barrelFinite) || !finiteFloats(transform) || !finiteFloats(linear) ||
                    frameFlags != 7 || frameReserved != 0)
                fail("a9g4r2.frame." + index);
            source.nitroCalls += nitro;
            if (ax != 0 || ay != 0 || az != 0 || rbx0 != 0 || rbx1 != 0) source.barrelFrames++;
        }

        long lastTick = -1, nextOrdinal = 0;
        boolean[] covered = new boolean[Math.toIntExact(source.frameCount)];
        for (long index = 0; index < source.intervalCount; ++index) {
            ByteBuffer interval = little(readExact(input, A9G4R2_INTERVAL_SIZE));
            long tick = interval.getLong(), ordinal = u32(interval);
            float output = Float.intBitsToFloat(interval.getInt());
            if (tick >= source.frameCount || !finite(output) || output < 0.001f || output > 0.1f)
                fail("a9g4r2.interval_value." + index);
            if (tick != lastTick) {
                if (tick <= lastTick) fail("a9g4r2.interval_order." + index);
                lastTick = tick;
                nextOrdinal = 0;
            }
            if (ordinal != nextOrdinal++) fail("a9g4r2.interval_ordinal." + index);
            covered[Math.toIntExact(tick)] = true;
        }
        if (!sparse)
            for (boolean value : covered) if (!value) fail("a9g4r2.missing_tick_interval");
        return source;
    }

    private static void validateManifest(JSONObject manifest, SourceSummary source, long target)
            throws Exception {
        requireKeys(manifest, "schema", "recording_id", "title", "created_utc", "game",
                "race", "recording", "notes");
        if (!"a9tas.recording.v1".equals(manifest.getString("schema"))) fail("manifest.schema");
        String id = text(manifest, "recording_id", 36, true);
        if (!UUID.fromString(id).toString().equals(id)) fail("manifest.recording_id.canonical");
        text(manifest, "title", 120, true);
        String created = text(manifest, "created_utc", 32, true);
        if (!created.endsWith("Z")) fail("manifest.created_utc.utc");
        if (!UtcTimestamp.valid(created)) fail("manifest.created_utc.format");
        text(manifest, "notes", 2_000, false);

        JSONObject game = manifest.getJSONObject("game");
        requireKeys(game, "package", "version", "native_sha256", "build_id", "build_profile_sha256");
        if (!text(game, "package", 191, true).matches("[A-Za-z0-9_]+(?:\\.[A-Za-z0-9_]+)+"))
            fail("manifest.game.package");
        text(game, "version", 64, true);
        if (!text(game, "native_sha256", 64, true).matches("[0-9a-f]{64}") ||
                !text(game, "build_profile_sha256", 64, true).matches("[0-9a-f]{64}") ||
                !text(game, "build_id", 40, true).matches("[0-9a-f]{40}"))
            fail("manifest.game.identity");

        JSONObject race = manifest.getJSONObject("race");
        requireKeys(race, "map", "car", "control_mode");
        text(race, "map", 120, true);
        text(race, "car", 120, true);
        if (!new HashSet<>(Arrays.asList("manual", "touchdrive", "unknown"))
                .contains(race.getString("control_mode")))
            fail("manifest.race.control_mode");

        JSONObject recording = manifest.getJSONObject("recording");
        requireKeys(recording, "format", "format_version", "frame_count", "interval_count",
                "fixed_delta_us", "session_id", "generation", "target_tick");
        if (!"A9G4R2".equals(recording.getString("format")) ||
                exactLong(recording, "format_version") != source.version ||
                exactLong(recording, "frame_count") != source.frameCount ||
                exactLong(recording, "interval_count") != source.intervalCount ||
                exactLong(recording, "fixed_delta_us") != source.fixedDeltaUs ||
                exactLong(recording, "session_id") != source.sessionId ||
                exactLong(recording, "generation") != source.generation ||
                exactLong(recording, "target_tick") != target)
            fail("manifest.recording_identity");
    }

    private static long exactLong(JSONObject object, String key) throws Exception {
        Object value = object.get(key);
        if (!(value instanceof Number) || value instanceof Double || value instanceof Float)
            fail("manifest.recording_types");
        return ((Number) value).longValue();
    }

    private static String text(JSONObject object, String key, int max, boolean nonempty)
            throws Exception {
        Object raw = object.get(key);
        if (!(raw instanceof String)) fail("manifest." + key);
        String value = (String) raw;
        if ((nonempty && value.isEmpty()) || value.length() > max) fail("manifest." + key);
        for (int i = 0; i < value.length(); ++i) if (value.charAt(i) < 0x20) fail("manifest.control");
        return value;
    }

    private static void requireKeys(JSONObject object, String... expected) throws IOException {
        Set<String> actual = new HashSet<>();
        Iterator<String> keys = object.keys();
        while (keys.hasNext()) actual.add(keys.next());
        if (!actual.equals(new HashSet<>(Arrays.asList(expected)))) fail("manifest.keys");
    }

    static String canonical(Object value) throws Exception {
        if (value == null || value == JSONObject.NULL) return "null";
        if (value instanceof JSONObject) {
            JSONObject object = (JSONObject) value;
            List<String> keys = new ArrayList<>();
            Iterator<String> iterator = object.keys();
            while (iterator.hasNext()) keys.add(iterator.next());
            Collections.sort(keys);
            StringBuilder out = new StringBuilder("{");
            for (int index = 0; index < keys.size(); ++index) {
                if (index != 0) out.append(',');
                String key = keys.get(index);
                out.append(JSONObject.quote(key)).append(':').append(canonical(object.get(key)));
            }
            return out.append('}').toString();
        }
        if (value instanceof JSONArray) {
            JSONArray array = (JSONArray) value;
            StringBuilder out = new StringBuilder("[");
            for (int index = 0; index < array.length(); ++index) {
                if (index != 0) out.append(',');
                out.append(canonical(array.get(index)));
            }
            return out.append(']').toString();
        }
        if (value instanceof String) return JSONObject.quote((String) value);
        if (value instanceof Boolean) return value.toString();
        if (value instanceof Number) return JSONObject.numberToString((Number) value);
        fail("manifest.value_type");
        return "";
    }

    private static byte[] hashRange(RandomAccessFile input, long offset, long size) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        input.seek(offset);
        byte[] buffer = new byte[64 * 1024];
        long remaining = size;
        while (remaining > 0) {
            int count = input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
            if (count < 0) throw new EOFException();
            digest.update(buffer, 0, count);
            remaining -= count;
        }
        return digest.digest();
    }

    private static byte[] hash(byte[] bytes) throws Exception {
        return MessageDigest.getInstance("SHA-256").digest(bytes);
    }

    private static byte[] readExact(RandomAccessFile input, int size) throws IOException {
        byte[] bytes = new byte[size];
        input.readFully(bytes);
        return bytes;
    }

    private static ByteBuffer little(byte[] bytes) {
        return ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
    }

    private static long u32(ByteBuffer buffer) { return Integer.toUnsignedLong(buffer.getInt()); }

    private static byte[] take(ByteBuffer buffer, int size) {
        byte[] result = new byte[size];
        buffer.get(result);
        return result;
    }

    private static void requireMagic(ByteBuffer buffer, byte[] expected, String error)
            throws IOException {
        if (!Arrays.equals(take(buffer, expected.length), expected)) fail(error);
    }

    private static boolean allZero(byte[] bytes) {
        for (byte value : bytes) if (value != 0) return false;
        return true;
    }

    private static boolean bounded(float value, float limit) {
        return finite(value) && Math.abs(value) <= limit;
    }

    private static boolean finite(float value) {
        return Float.isFinite(value) && Math.abs(value) <= 1_000_000.0f;
    }

    private static boolean finiteFloats(byte[] bytes) {
        ByteBuffer values = little(bytes);
        while (values.hasRemaining()) if (!finite(values.getFloat())) return false;
        return true;
    }

    private static String strictUtf8(byte[] bytes) throws IOException {
        java.nio.charset.CharsetDecoder decoder = StandardCharsets.UTF_8.newDecoder();
        decoder.onMalformedInput(java.nio.charset.CodingErrorAction.REPORT);
        decoder.onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT);
        try { return decoder.decode(ByteBuffer.wrap(bytes)).toString(); }
        catch (java.nio.charset.CharacterCodingException error) { throw new IOException("archive.manifest_json", error); }
    }

    private static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) out.append(String.format(Locale.ROOT, "%02x", value & 0xff));
        return out.toString();
    }

    private static void fail(String code) throws IOException { throw new IOException(code); }
}
