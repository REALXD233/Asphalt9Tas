package dev.a9tas.android;

import android.content.Context;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Immutable prefix/branch editing for the canonical A9G4R2 recording stream. */
final class A9TasBranchEditor {
    private static final int MAX_FRAMES = 24000;

    private A9TasBranchEditor() {}

    static A9TasLibrary.Entry splice(Context context, A9TasLibrary.Entry base,
                                     long baseTargetTick, File suffix,
                                     String suffixSha) throws Exception {
        if (base == null || suffix == null || suffixSha == null ||
                !suffixSha.equals(A9TasLibrary.sha256(suffix)))
            throw new IOException("branch source identity changed");
        File prefix = A9TasLibrary.materializeReplaySource(context, base, baseTargetTick);
        A9TasArchive.SourceSummary prefixSummary = A9TasArchive.inspectSource(prefix);
        A9TasArchive.SourceSummary suffixSummary = A9TasArchive.inspectSource(suffix);
        if (prefixSummary.version != suffixSummary.version ||
                prefixSummary.fixedDeltaUs != suffixSummary.fixedDeltaUs)
            throw new IOException("branch segments use different recording clocks");
        long totalFramesLong = prefixSummary.frameCount + suffixSummary.frameCount;
        long totalIntervalsLong = prefixSummary.intervalCount + suffixSummary.intervalCount;
        if (totalFramesLong <= 0 || totalFramesLong > MAX_FRAMES ||
                totalIntervalsLong > Integer.MAX_VALUE)
            throw new IOException("branched recording exceeds runtime capacity");

        File output = newRawFile(context);
        File pending = new File(output.getAbsolutePath() + ".pending");
        try {
            spliceRaw(prefix, prefixSummary, suffix, suffixSummary, pending,
                    Math.toIntExact(totalFramesLong), Math.toIntExact(totalIntervalsLong));
            A9TasArchive.SourceSummary result = A9TasArchive.inspectSource(pending);
            if (result.frameCount != totalFramesLong ||
                    result.intervalCount != totalIntervalsLong)
                throw new IOException("branched recording verification failed");
            // The two ESC presses delimit an editing pause in wall-clock time;
            // A9G4R2 stores only authoritative simulation ticks.  Prove that
            // the published boundary remains adjacent (T, T+1) with one fixed
            // delta, so replay never pauses at the splice point.
            verifyContinuousBoundary(pending, prefixSummary.frameCount,
                    prefixSummary.fixedDeltaUs);
            if (!pending.renameTo(output))
                throw new IOException("branched recording publish failed");
            String hash = A9TasLibrary.sha256(output);
            A9TasLibrary.Entry packed = A9TasLibrary.pack(context, output, hash,
                    metadata(base, " · 分支 T" + baseTargetTick,
                            "branch", baseTargetTick));
            // The canonical A9TAS1 archive owns an authenticated copy of the
            // stream.  Keeping the intermediate raw stream would leak one file
            // on every brush-lap iteration.
            if (!output.delete()) output.deleteOnExit();
            return packed;
        } catch (Exception error) {
            if (pending.exists()) pending.delete();
            if (output.exists()) output.delete();
            throw error;
        }
    }

    /**
     * Publishes a branch that was recorded continuously by the native runtime.
     * The stream already contains the replayed prefix and natural suffix on one
     * authoritative Tick timeline, so Java must validate and package it without
     * retimestamping either side of the boundary.
     */
    static A9TasLibrary.Entry adoptContinuous(Context context,
                                              A9TasLibrary.Entry base,
                                              long baseTargetTick,
                                              File recording,
                                              String recordingSha,
                                              boolean checkpoint)
            throws Exception {
        if (base == null || recording == null || recordingSha == null ||
                !recordingSha.equals(A9TasLibrary.sha256(recording)))
            throw new IOException("continuous branch source identity changed");
        long prefixFrames = Math.addExact(baseTargetTick, 1L);
        A9TasArchive.SourceSummary summary = A9TasArchive.inspectSource(recording);
        if (prefixFrames <= 0 || prefixFrames >= summary.frameCount ||
                summary.frameCount > MAX_FRAMES)
            throw new IOException("continuous branch has no recorded suffix");
        verifyContinuousBoundary(recording, prefixFrames, summary.fixedDeltaUs);
        rejectExactSkippedSourceFrame(context, base, baseTargetTick,
                recording, prefixFrames);
        A9TasLibrary.Metadata branchMetadata = metadata(base,
                " · 分支 T" + baseTargetTick, "branch", baseTargetTick);
        // Paused checkpoints live in drafts; a completed race is exported to
        // recordings. Preserve that ownership distinction rather than trying
        // to retire a completed recording through the draft-only API.
        if (checkpoint)
            return A9TasLibrary.promoteDraft(context, recording, recordingSha,
                    Math.toIntExact(summary.frameCount), branchMetadata);
        return A9TasLibrary.pack(context, recording, recordingSha, branchMetadata);
    }

    static A9TasLibrary.Entry trimCopy(Context context, A9TasLibrary.Entry source,
                                       long targetTick) throws Exception {
        if (source == null || targetTick < 0 || targetTick >= source.summary.frameCount)
            throw new IOException("trim target tick is outside the recording");
        File prefix = A9TasLibrary.materializeReplaySource(context, source, targetTick);
        File output = newRawFile(context);
        File pending = new File(output.getAbsolutePath() + ".pending");
        try {
            copy(prefix, pending);
            A9TasArchive.SourceSummary summary = A9TasArchive.inspectSource(pending);
            if (summary.frameCount != targetTick + 1)
                throw new IOException("trimmed recording length changed");
            if (!pending.renameTo(output)) throw new IOException("trim copy publish failed");
            String hash = A9TasLibrary.sha256(output);
            A9TasLibrary.Entry packed = A9TasLibrary.pack(context, output, hash,
                    metadata(source, " · 截取 T" + targetTick,
                            "trim", targetTick));
            if (!output.delete()) output.deleteOnExit();
            return packed;
        } catch (Exception error) {
            if (pending.exists()) pending.delete();
            if (output.exists()) output.delete();
            throw error;
        }
    }

    private static void spliceRaw(File prefix, A9TasArchive.SourceSummary prefixSummary,
                                  File suffix, A9TasArchive.SourceSummary suffixSummary,
                                  File output, int totalFrames, int totalIntervals)
            throws Exception {
        try (RandomAccessFile first = new RandomAccessFile(prefix, "r");
             RandomAccessFile second = new RandomAccessFile(suffix, "r");
             FileOutputStream destination = new FileOutputStream(output, false)) {
            byte[] header = new byte[A9TasArchive.A9G4R2_HEADER_SIZE];
            first.readFully(header);
            ByteBuffer edited = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
            edited.putInt(24, totalFrames);
            edited.putInt(28, totalIntervals);
            destination.write(header);

            copyExact(first, destination,
                    prefixSummary.frameCount * A9TasArchive.A9G4R2_FRAME_SIZE);
            second.seek(A9TasArchive.A9G4R2_HEADER_SIZE);
            byte[] frame = new byte[A9TasArchive.A9G4R2_FRAME_SIZE];
            for (long index = 0; index < suffixSummary.frameCount; ++index) {
                second.readFully(frame);
                long tick = prefixSummary.frameCount + index;
                ByteBuffer view = ByteBuffer.wrap(frame).order(ByteOrder.LITTLE_ENDIAN);
                view.putLong(0, tick);
                view.putLong(8, Math.multiplyExact(Math.multiplyExact(tick,
                        prefixSummary.fixedDeltaUs), 1_000L));
                destination.write(frame);
            }

            first.seek(A9TasArchive.A9G4R2_HEADER_SIZE +
                    prefixSummary.frameCount * A9TasArchive.A9G4R2_FRAME_SIZE);
            copyExact(first, destination,
                    prefixSummary.intervalCount * A9TasArchive.A9G4R2_INTERVAL_SIZE);
            second.seek(A9TasArchive.A9G4R2_HEADER_SIZE +
                    suffixSummary.frameCount * A9TasArchive.A9G4R2_FRAME_SIZE);
            byte[] interval = new byte[A9TasArchive.A9G4R2_INTERVAL_SIZE];
            for (long index = 0; index < suffixSummary.intervalCount; ++index) {
                second.readFully(interval);
                ByteBuffer view = ByteBuffer.wrap(interval).order(ByteOrder.LITTLE_ENDIAN);
                view.putLong(0, Math.addExact(view.getLong(0), prefixSummary.frameCount));
                destination.write(interval);
            }
            destination.flush();
            destination.getFD().sync();
        }
    }

    private static A9TasLibrary.Metadata metadata(A9TasLibrary.Entry source,
                                                  String suffix, String kind,
                                                  long sourceTick) throws Exception {
        JSONObject manifest = source.summary.manifest;
        JSONObject game = manifest.getJSONObject("game");
        JSONObject race = manifest.getJSONObject("race");
        JSONObject lineage = new JSONObject().put("kind", kind)
                .put("parent_recording_id", manifest.getString("recording_id"))
                .put("source_tick", sourceTick);
        return new A9TasLibrary.Metadata(
                manifest.getString("title") + suffix, UtcTimestamp.nowSeconds(),
                game.getString("package"), game.getString("version"),
                game.getString("native_sha256"), game.getString("build_id"),
                game.getString("build_profile_sha256"), race.getString("map"),
                race.getString("car"), race.getString("control_mode"),
                A9TasArchive.canonical(lineage));
    }

    private static File newRawFile(Context context) throws Exception {
        File directory = new File(context.getFilesDir(), "recordings").getCanonicalFile();
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("recording directory unavailable");
        for (int attempt = 0; attempt < 100; ++attempt) {
            File candidate = new File(directory, "recording-" +
                    System.currentTimeMillis() + "-" +
                    (android.os.Process.myPid() + attempt) + ".a9g4r2");
            if (!candidate.exists() && !new File(candidate.getAbsolutePath() + ".pending").exists())
                return candidate;
        }
        throw new IOException("unable to allocate branch recording path");
    }

    private static void copy(File input, File output) throws Exception {
        try (FileInputStream source = new FileInputStream(input);
             FileOutputStream destination = new FileOutputStream(output, false)) {
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = source.read(buffer)) >= 0)
                if (count != 0) destination.write(buffer, 0, count);
            destination.flush();
            destination.getFD().sync();
        }
    }

    private static void verifyContinuousBoundary(File recording,
                                                 long prefixFrames,
                                                 long fixedDeltaUs)
            throws Exception {
        if (prefixFrames <= 0) throw new IOException("branch prefix is empty");
        try (RandomAccessFile input = new RandomAccessFile(recording, "r")) {
            long priorOffset = A9TasArchive.A9G4R2_HEADER_SIZE +
                    (prefixFrames - 1L) * A9TasArchive.A9G4R2_FRAME_SIZE;
            byte[] pair = new byte[A9TasArchive.A9G4R2_FRAME_SIZE * 2];
            input.seek(priorOffset);
            input.readFully(pair);
            ByteBuffer view = ByteBuffer.wrap(pair).order(ByteOrder.LITTLE_ENDIAN);
            long priorTick = view.getLong(0);
            long priorTime = view.getLong(8);
            long nextTick = view.getLong(A9TasArchive.A9G4R2_FRAME_SIZE);
            long nextTime = view.getLong(A9TasArchive.A9G4R2_FRAME_SIZE + 8);
            long expectedStep = Math.multiplyExact(fixedDeltaUs, 1_000L);
            if (priorTick != prefixFrames - 1L || nextTick != prefixFrames ||
                    nextTime - priorTime != expectedStep)
                throw new IOException("branch boundary contains a tick gap or pause");
        }
    }

    /**
     * Detects the concrete replay-to-record regression that ordinary tick/time
     * validation cannot see: the first suffix frame is timestamped T+1 but its
     * complete physics state is bit-identical to source T+2.  This is a
     * diagnostic/rejection guard only; it never invents a frame or rewrites a
     * timestamp.  Legitimate user divergence remains accepted.
     */
    private static void rejectExactSkippedSourceFrame(Context context,
                                                      A9TasLibrary.Entry base,
                                                      long baseTargetTick,
                                                      File recording,
                                                      long prefixFrames)
            throws Exception {
        if (baseTargetTick > Long.MAX_VALUE - 2L ||
                baseTargetTick + 2L >= base.summary.frameCount)
            return;
        File source = A9TasLibrary.materializeReplaySource(
                context, base, baseTargetTick + 2L);
        byte[] actual = readPhysicsState(recording, prefixFrames);
        byte[] expectedNext = readPhysicsState(source, prefixFrames);
        byte[] skippedNext = readPhysicsState(source, prefixFrames + 1L);
        if (!java.security.MessageDigest.isEqual(actual, expectedNext) &&
                java.security.MessageDigest.isEqual(actual, skippedNext))
            throw new IOException(
                    "replay-to-record boundary skipped one authoritative physics tick");
    }

    private static byte[] readPhysicsState(File recording, long frameIndex)
            throws Exception {
        final int physicsOffset = 60;
        final int physicsSize = 64 + 12;
        long frameOffset = Math.addExact(A9TasArchive.A9G4R2_HEADER_SIZE,
                Math.multiplyExact(frameIndex,
                        (long) A9TasArchive.A9G4R2_FRAME_SIZE));
        long offset = Math.addExact(frameOffset, physicsOffset);
        try (RandomAccessFile input = new RandomAccessFile(recording, "r")) {
            if (offset < 0 || offset + physicsSize > input.length())
                throw new IOException("branch physics boundary is outside the recording");
            byte[] state = new byte[physicsSize];
            input.seek(offset);
            input.readFully(state);
            return state;
        }
    }

    private static void copyExact(RandomAccessFile input, FileOutputStream output,
                                  long count) throws Exception {
        byte[] buffer = new byte[64 * 1024];
        long remaining = count;
        while (remaining > 0) {
            int read = input.read(buffer, 0, (int) Math.min(buffer.length, remaining));
            if (read < 0) throw new IOException("branch source ended early");
            if (read != 0) {
                output.write(buffer, 0, read);
                remaining -= read;
            }
        }
    }
}
