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
        Files.createDirectories(root.resolve("recordings"));
        Files.createDirectories(root.resolve("drafts"));
        byte[] archive = Files.readAllBytes(Path.of(args[0]));
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
