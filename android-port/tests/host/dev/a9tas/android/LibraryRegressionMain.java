package dev.a9tas.android;

import android.content.Context;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/** Executes production Java archive code on synthetic data; no game or root. */
public final class LibraryRegressionMain {
    private static int checks;
    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
        checks++;
    }
    private interface Attempt { void run() throws Exception; }
    private static void rejects(Attempt action, String message) throws Exception {
        try { action.run(); } catch (java.io.IOException expected) { checks++; return; }
        throw new AssertionError(message);
    }
    public static void main(String[] args) throws Exception {
        check(SessionOrchestrator.shouldSealExistingTicks(false, 800, true), "save closed prefix immediately");
        check(!SessionOrchestrator.shouldSealExistingTicks(true, 800, true), "finish takes priority over save");
        check(!SessionOrchestrator.shouldSealExistingTicks(false, 0, true), "no empty checkpoint");
        check(!SessionOrchestrator.shouldSealExistingTicks(false, 800, false), "record continues without save request");
        testRootOwnership();
        Path root = Path.of(args[1]);
        Context context = new Context(root.toFile());
        var session = context.getSharedPreferences("session", 0);
        session.edit().putBoolean("prepared_ready", true)
                .putBoolean("session_hooks_installed", true).putInt("prepared_pid", 5165)
                .putBoolean("resident_retry_auto_loop", true)
                .putString("selected_archive", "keep-me.a9tas").commit();
        var clear = SessionOrchestrator.class.getDeclaredMethod("clearTerminatedRuntimeState", Context.class);
        clear.setAccessible(true);
        clear.invoke(null, context);
        check(!session.getBoolean("prepared_ready", true), "dead process no longer prepared");
        check(!session.getBoolean("session_hooks_installed", true), "dead hooks no longer installed");
        check(!session.getBoolean("resident_retry_auto_loop", true), "dead retry not armed");
        check(session.getString("selected_archive", "").equals("keep-me.a9tas"), "recording selection survives cleanup");
        check(SessionOrchestrator.objectFailureDetails("G4_OBJECT_DIAG lifecycle=0 scanned=123\n" + "x".repeat(700))
                .contains("scanned=123"), "object diagnostic survives tail truncation");
        Files.createDirectories(root.resolve("recordings"));
        String sparseFinish = "complete=1 ticks=4217 begin=4217 interval=1794 "
                + "interval_calls=1794 final=4217 end=4217 error=0";
        SessionOrchestrator.validateIntegrationReceipt(sparseFinish, 4217, 6944);
        check(true, "actual 144Hz finish receipt accepts interpolation-only ticks");
        SessionOrchestrator.validateIntegrationReceipt("interval=0 interval_calls=0", 1, 8333);
        check(true, "zero-integration short completion accepted at 120Hz");
        SessionOrchestrator.validateIntegrationReceipt("interval=5 interval_calls=7", 5, 16667);
        check(true, "legacy multi-step integration remains accepted");
        rejects(() -> SessionOrchestrator.validateIntegrationReceipt(sparseFinish, 4217, 16667),
                "legacy format still requires an integration per tick");
        rejects(() -> SessionOrchestrator.validateIntegrationReceipt("interval=6 interval_calls=6", 5, 6944),
                "integrated frame count cannot exceed logical frames");
        rejects(() -> SessionOrchestrator.validateIntegrationReceipt("interval=2 interval_calls=1", 5, 6944),
                "interval calls cannot be fewer than integrated frames");
        rejects(() -> SessionOrchestrator.validateIntegrationReceipt("interval=0 interval_calls=1", 5, 6944),
                "zero integrated frames cannot have interval calls");
        rejects(() -> SessionOrchestrator.validateIntegrationReceipt("interval=2", 5, 6944),
                "missing integration counters rejected");
        session.edit().putInt("record_tick_hz", 144).commit();
        check(SessionOrchestrator.stableRecordDeltaUs(session) == 6944
                && session.getInt("record_tick_hz", 0) == 144,
                "144Hz is retained for the completion-aware candidate");
        session.edit().putInt("record_tick_hz", 120).commit();
        check(SessionOrchestrator.stableRecordDeltaUs(session) == 8333
                && session.getInt("record_tick_hz", 0) == 120,
                "120Hz is retained for the completion-aware candidate");
        session.edit().putInt("record_tick_hz", 999).commit();
        check(SessionOrchestrator.stableRecordDeltaUs(session) == 16667
                && session.getInt("record_tick_hz_before_compatibility_reset", 0) == 999
                && session.getInt("record_tick_hz", 0) == 60,
                "invalid record rate resets to 60Hz without changing archives");
        check(SessionOrchestrator.stableRecordDeltaUs(session) == 16667,
                "60Hz remains the default");
        String runtimeFault = SessionOrchestrator.runtimeFailureDetails(
                "G4_STATUS complete=0 ticks=0 begin=1 interval=0 final=1 end=0 "
                + "error=13 coordinator_result=-6 fault_context=0x200000001 fault_tick=0 "
                + "x".repeat(6000) + " interval_probe=20,0,0x10,0x20,0x30,0x40"
                + " tick_config=1,6944 control=0,0");
        check(runtimeFault.contains("coordinator_result=-6")
                && runtimeFault.contains("fault_context=0x200000001")
                && runtimeFault.contains("interval=0"), "runtime fault origin survives truncation");
        check(runtimeFault.length() < 4096 && runtimeFault.contains("control=0,0"),
                "runtime failure detail is bounded and retains tail");
        check(runtimeFault.startsWith("error=13")
                && runtimeFault.substring(0, runtimeFault.indexOf('\n'))
                    .contains("interval_probe=20,0,0x10,0x20,0x30,0x40")
                && runtimeFault.contains("tick_config=1,6944"),
                "interval qualification evidence survives receipt truncation");
        Files.createDirectories(root.resolve("drafts"));
        byte[] archive = Files.readAllBytes(Path.of(args[0]));
        Context importContext = new Context(root.resolve("import-tests").toFile());
        A9TasLibrary.Entry imported = RecordingImporter.importStream(importContext,
                new java.io.ByteArrayInputStream(archive));
        check(imported.file.isFile(), "shared stream import publishes archive");
        check(RecordingImporter.importStream(importContext,
                new java.io.ByteArrayInputStream(archive)).file.equals(imported.file),
                "identical archive reimport is idempotent");
        rejects(() -> RecordingImporter.importStream(importContext,
                new java.io.ByteArrayInputStream(new byte[160])), "invalid shared import rejected");
        check(SharedRecordingBrowser.allowed("/sdcard/Documents/friend.a9tas"), "Documents import allowed");
        check(SharedRecordingBrowser.allowed("/sdcard/Download/friend.A9TAS"), "Download extension case");
        check(!SharedRecordingBrowser.allowed("/sdcard/Documents/../secret.a9tas"), "no traversal");
        check(!SharedRecordingBrowser.allowed("/data/local/tmp/file.a9tas"), "bounded shared roots");
        check(!SharedRecordingBrowser.allowed("/sdcard/Documents/a\nb.a9tas"), "no multiline path");
        int offset = A9TasArchive.HEADER_SIZE +
                ByteBuffer.wrap(archive).order(ByteOrder.LITTLE_ENDIAN).getInt(20);
        byte[] raw = Arrays.copyOfRange(archive, offset, archive.length);
        Path source = root.resolve("recordings/recording-1-3.a9g4r2");
        Files.write(source, raw);
        String hash = A9TasLibrary.sha256(source.toFile());
        A9TasArchive.Summary template = A9TasArchive.inspect(Path.of(args[0]).toFile());
        var game = template.manifest.getJSONObject("game");
        var race = template.manifest.getJSONObject("race");
        var metadata = new A9TasLibrary.Metadata("test1", "2026-09-04T00:00:00Z",
                game.getString("package"), game.getString("version"),
                game.getString("native_sha256"), game.getString("build_id"),
                game.getString("build_profile_sha256"), race.getString("map"),
                race.getString("car"), race.getString("control_mode"), "");
        var entry = A9TasLibrary.pack(context, source.toFile(), hash, metadata);
        {
            Context sparseContext = new Context(root.resolve("sparse-144").toFile());
            byte[] sparse = Arrays.copyOf(raw, 64 + 3 * 144 + 16);
            ByteBuffer sb = ByteBuffer.wrap(sparse).order(ByteOrder.LITTLE_ENDIAN);
            sb.putInt(8, 4); sb.putInt(28, 1); sb.putInt(32, 6944); sb.putInt(36, 0x3f);
            for (int i = 0; i < 3; ++i) {
                sb.putLong(64 + i * 144 + 8, i * 6944_000L);
                sb.putInt(64 + i * 144 + 32, 0x48);
            }
            sb.putLong(64 + 3 * 144, 1); sb.putInt(64 + 3 * 144 + 8, 0);
            Path sparseRaw = root.resolve("sparse-144/recordings/recording-1-3.a9g4r2");
            Files.createDirectories(sparseRaw.getParent()); Files.write(sparseRaw, sparse);
            var sparseEntry = A9TasLibrary.pack(sparseContext, sparseRaw.toFile(),
                    A9TasLibrary.sha256(sparseRaw.toFile()), metadata);
            check(sparseEntry.summary.frameCount == 3, "sparse archive roundtrip");
            var zeroPrefix = A9TasLibrary.materializeReplaySource(sparseContext, sparseEntry, 0);
            var zeroSummary = A9TasArchive.inspectSource(zeroPrefix);
            check(zeroSummary.version == 4 && zeroSummary.frameCount == 1 && zeroSummary.intervalCount == 0,
                    "cut at leading interpolation-only tick retains empty interval stream");
            var branch = A9TasBranchEditor.splice(sparseContext, sparseEntry, 0,
                    sparseRaw.toFile(), A9TasLibrary.sha256(sparseRaw.toFile()));
            check(branch.summary.frameCount == 4 && branch.summary.fixedDeltaUs == 6944,
                    "splice after zero-integration prefix preserves tick timeline");
            byte[] empty = Arrays.copyOf(sparse, 64 + 3 * 144);
            ByteBuffer.wrap(empty).order(ByteOrder.LITTLE_ENDIAN).putInt(28, 0);
            Path emptyRaw = root.resolve("sparse-144/empty.a9g4r2");
            Files.write(emptyRaw, empty);
            check(A9TasArchive.inspectSource(emptyRaw.toFile()).intervalCount == 0,
                    "entirely interpolation-only segment is representable");
            byte[] old = sparse.clone();
            ByteBuffer ob = ByteBuffer.wrap(old).order(ByteOrder.LITTLE_ENDIAN);
            ob.putInt(8, 3); ob.putInt(36, 0x1f);
            Path bad = root.resolve("sparse-144/legacy-missing.a9g4r2"); Files.write(bad, old);
            rejects(() -> A9TasArchive.inspectSource(bad.toFile()), "legacy missing intervals remain rejected");
            byte[] nan = sparse.clone();
            ByteBuffer.wrap(nan).order(ByteOrder.LITTLE_ENDIAN).putInt(64 + 40, 0x7fc00000);
            Path nanRaw = root.resolve("sparse-144/nan.a9g4r2"); Files.write(nanRaw, nan);
            rejects(() -> A9TasArchive.inspectSource(nanRaw.toFile()), "sparse barrel NaN remains rejected");
        }
        {
            final int count = 24000, step = 6944;
            Context longContext = new Context(root.resolve("long-144").toFile());
            byte[] full = new byte[64 + count * (144 + 16)];
            System.arraycopy(raw, 0, full, 0, 64);
            ByteBuffer lb = ByteBuffer.wrap(full).order(ByteOrder.LITTLE_ENDIAN);
            lb.putInt(24, count); lb.putInt(28, count); lb.putInt(32, step);
            for (int i = 0; i < count; i++) {
                int frame = 64 + i * 144;
                System.arraycopy(raw, 64, full, frame, 144);
                lb.putLong(frame, i); lb.putLong(frame + 8, i * (long) step * 1000);
                int interval = 64 + count * 144 + i * 16;
                System.arraycopy(raw, 64 + 3 * 144, full, interval, 16);
                lb.putLong(interval, i); lb.putInt(interval + 8, 0);
            }
            Path longRaw = root.resolve("long-144/recordings/recording-1-24000.a9g4r2");
            Files.createDirectories(longRaw.getParent()); Files.write(longRaw, full);
            String longSha = A9TasLibrary.sha256(longRaw.toFile());
            var longEntry = A9TasLibrary.pack(longContext, longRaw.toFile(), longSha, metadata);
            check(longEntry.summary.frameCount == count, "24000-tick archive roundtrip");
            check((count - 1L) * step > 150_000_000L, "144Hz exceeds 150 seconds");
            var longPrefix = A9TasLibrary.materializeReplaySource(longContext, longEntry, 22000);
            check(A9TasArchive.inspectSource(longPrefix).frameCount == 22001,
                    "prefix past old 7200/16384 limits");
            Path longDraft = root.resolve("long-144/drafts/attempt-1-24000.a9g4r2");
            Files.createDirectories(longDraft.getParent()); Files.write(longDraft, full);
            var longBranch = A9TasBranchEditor.adoptContinuous(longContext, longEntry,
                    22000, longDraft.toFile(), longSha);
            check(longBranch.summary.frameCount == count &&
                    longBranch.summary.recordingSha256.equals(longSha),
                    "long branch preserves all source ticks");
        }
        for (int step : new int[]{8333, 6944}) {
            Context highContext = new Context(root.resolve("hz-" + step).toFile());
            byte[] high = raw.clone();
            ByteBuffer hb = ByteBuffer.wrap(high).order(ByteOrder.LITTLE_ENDIAN);
            hb.putInt(32, step);
            for (int i = 0; i < 3; i++)
                hb.putLong(64 + i * 144 + 8, i * (long)step * 1000L);
            Path highRaw = root.resolve("hz-" + step + "/recordings/recording-1-3.a9g4r2");
            Files.createDirectories(highRaw.getParent());
            Files.write(highRaw, high);
            String highSha = A9TasLibrary.sha256(highRaw.toFile());
            var highEntry = A9TasLibrary.pack(highContext, highRaw.toFile(), highSha, metadata);
            check(highEntry.summary.fixedDeltaUs == step, "high-rate archive retains timestep");
            var trimmed = A9TasLibrary.materializeReplaySource(highContext, highEntry, 1);
            var trimmedSummary = A9TasArchive.inspectSource(trimmed);
            check(trimmedSummary.fixedDeltaUs == step && trimmedSummary.frameCount == 2,
                    "high-rate prefix preserves timestep and length");
            Path highDraft = root.resolve("hz-" + step + "/drafts/attempt-1-3.a9g4r2");
            Files.createDirectories(highDraft.getParent());
            Files.write(highDraft, high);
            var adoptedHigh = A9TasBranchEditor.adoptContinuous(highContext, highEntry,
                    0, highDraft.toFile(), highSha);
            check(adoptedHigh.summary.fixedDeltaUs == step &&
                    adoptedHigh.summary.recordingSha256.equals(highSha),
                    "high-rate continuation preserves timeline bytes");
        }
        check(entry.archiveSha256.equals(A9TasLibrary.sha256(entry.file)), "streamed archive digest");
        check(entry.summary.recordingSha256.equals(hash), "source digest unchanged");
        check(A9TasLibrary.list(context).valid.get(0) == entry, "warm listing cache reused");
        var renamed = A9TasLibrary.rename(context, entry, "renamed");
        check(renamed.summary.recordingSha256.equals(hash), "rename preserves recording bytes");
        check(renamed.archiveSha256.equals(A9TasLibrary.sha256(renamed.file)), "rename digest");
        check(A9TasLibrary.list(context).valid.get(0).title().equals("renamed"), "cache rename invalidation");
        rejects(() -> A9TasLibrary.verified(context, entry.file.getPath(), entry.archiveSha256),
                "stale selection must be rejected");
        check(A9TasLibrary.verified(context, renamed.file.getPath(), renamed.archiveSha256)
                .title().equals("renamed"), "direct selected lookup");
        rejects(() -> A9TasLibrary.pack(context, source.toFile(), "0".repeat(64), metadata),
                "wrong source digest cannot publish");
        check(A9TasLibrary.list(context).valid.size() == 1, "failed pack leaves no archive");
        Path draft = root.resolve("drafts/attempt-1-3.a9g4r2");
        Files.write(draft, raw);
        var promoted = A9TasLibrary.promoteDraft(context, draft.toFile(), hash, 3, metadata);
        check(!Files.exists(draft), "successful promotion retires draft");
        check(promoted.archiveSha256.equals(A9TasLibrary.sha256(promoted.file)), "promotion digest");
        Path badDraft = root.resolve("drafts/attempt-2-3.a9g4r2");
        Files.write(badDraft, raw);
        rejects(() -> A9TasLibrary.promoteDraft(context, badDraft.toFile(), "0".repeat(64), 3, metadata),
                "failed promotion rejects wrong hash");
        check(Files.exists(badDraft), "failed promotion preserves draft");
        Path branchDraft = root.resolve("drafts/attempt-3-3.a9g4r2");
        Files.write(branchDraft, raw);
        var branch = A9TasBranchEditor.adoptContinuous(context, renamed, 0,
                branchDraft.toFile(), hash);
        check(branch.summary.recordingSha256.equals(hash), "adopt does not retimestamp or splice bytes");
        check(branch.summary.frameCount == 3, "adopt preserves full native timeline");
        long modified = renamed.file.lastModified();
        byte[] corrupt = Files.readAllBytes(renamed.file.toPath());
        corrupt[corrupt.length - 1] ^= 1;
        Files.write(renamed.file.toPath(), corrupt);
        renamed.file.setLastModified(modified);
        rejects(() -> A9TasLibrary.verified(context, renamed.file.getPath(), renamed.archiveSha256),
                "playback must not trust size/mtime listing cache");
        A9TasLibrary.delete(context, promoted);
        check(!promoted.file.exists(), "delete removes archive");
        check(A9TasLibrary.list(context).valid.stream().noneMatch(e -> e.file.equals(promoted.file)),
                "deleted entry absent from listing");
        System.out.println("LIBRARY_JAVA_REGRESSION passed=1 checks=" + checks);
    }

    private static void testRootOwnership() throws Exception {
        class FakeProcess extends java.lang.Process {
            boolean destroyed;
            final java.io.ByteArrayOutputStream output = new java.io.ByteArrayOutputStream();
            public java.io.OutputStream getOutputStream() { return output; }
            public java.io.InputStream getInputStream() { return java.io.InputStream.nullInputStream(); }
            public java.io.InputStream getErrorStream() { return java.io.InputStream.nullInputStream(); }
            public int waitFor() { return 0; }
            public int exitValue() { if (!destroyed) throw new IllegalThreadStateException(); return 0; }
            public void destroy() { destroyed = true; }
        }
        FakeProcess process = new FakeProcess();
        var field = RootShell.class.getDeclaredField("persistentProcess");
        field.setAccessible(true);
        field.set(null, process);
        var ownerGone = new java.util.concurrent.atomic.AtomicBoolean(true);
        Thread cleanup;
        synchronized (RootShell.class) {
            cleanup = new Thread(() -> RootShell.closePersistentIf(ownerGone::get));
            cleanup.start();
            ownerGone.set(false); // new service before the old command releases the lock
        }
        cleanup.join(2000);
        check(!cleanup.isAlive() && !process.destroyed, "replacement service keeps its shell");
        RootShell.closePersistentIf(() -> true);
        check(process.destroyed, "unowned shell closes after idle");
    }
}
