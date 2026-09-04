package dev.a9tas.android;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

final class RootShell {
    private static Process persistentProcess;
    private static BufferedWriter persistentInput;
    private static LinkedBlockingQueue<String> persistentOutput;
    static final class Result {
        final int exitCode;
        final List<String> output;
        final boolean timedOut;

        Result(int exitCode, List<String> output, boolean timedOut) {
            this.exitCode = exitCode;
            this.output = output;
            this.timedOut = timedOut;
        }

        boolean ok() { return !timedOut && exitCode == 0; }
    }

    private RootShell() {}

    static synchronized Result runFixedScript(String script, long timeoutSeconds)
            throws IOException, InterruptedException {
        long started = android.os.SystemClock.elapsedRealtime();
        ensurePersistentSession();
        String nonce = UUID.randomUUID().toString().replace("-", "");
        String begin = "A9TAS_ROOT_BEGIN_" + nonce;
        String end = "A9TAS_ROOT_END_" + nonce + ":";
        persistentInput.write("echo " + begin + "\n");
        persistentInput.write("( " + script + " )\n");
        persistentInput.write("a9tas_rc=$?; echo " + end + "$a9tas_rc\n");
        persistentInput.flush();

        List<String> lines = new ArrayList<>();
        boolean began = false;
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(timeoutSeconds);
        while (true) {
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0) {
                abortPersistent();
                return finish(-1, lines, true, started);
            }
            String line = persistentOutput.poll(remaining, TimeUnit.NANOSECONDS);
            if (line == null) {
                abortPersistent();
                return finish(-1, lines, true, started);
            }
            if (!began) {
                if (line.equals(begin)) began = true;
                else if (!processAlive(persistentProcess)) {
                    closePersistent();
                    return finish(255, lines, false, started);
                }
                continue;
            }
            if (line.startsWith(end)) {
                String raw = line.substring(end.length());
                int code;
                try { code = Integer.parseInt(raw); }
                catch (NumberFormatException error) { code = 255; }
                return finish(code, lines, false, started);
            }
            lines.add(line);
        }
    }

    private static Result finish(int exitCode, List<String> lines, boolean timedOut,
                                 long startedElapsed) {
        long duration = Math.max(0L,
                android.os.SystemClock.elapsedRealtime() - startedElapsed);
        DiagnosticBundle.recordRootReceipt(exitCode, timedOut, lines.size(), duration);
        return new Result(exitCode, lines, timedOut);
    }

    private static void ensurePersistentSession() throws IOException {
        if (processAlive(persistentProcess)) return;
        closePersistent();
        persistentOutput = new LinkedBlockingQueue<>();
        persistentProcess = startRootProcess();
        persistentInput = new BufferedWriter(new OutputStreamWriter(
                persistentProcess.getOutputStream(), StandardCharsets.UTF_8));
        Process processForReader = persistentProcess;
        LinkedBlockingQueue<String> outputForReader = persistentOutput;
        Thread reader = new Thread(() -> {
            try (BufferedReader input = new BufferedReader(new InputStreamReader(
                    processForReader.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = input.readLine()) != null) outputForReader.offer(line);
            } catch (IOException ignored) {
                // Command side observes process death or timeout and returns fail-closed.
            }
        }, "a9tas-persistent-root-output");
        reader.setDaemon(true);
        reader.start();
    }

    /**
     * Vendor Android builds do not consistently expose su through the app
     * process PATH.  Prefer an executable absolute path when one is visible,
     * then retain the ordinary PATH lookup for Magisk/KernelSU variants that
     * publish su dynamically after the user grants access.
     */
    private static Process startRootProcess() throws IOException {
        String[] absolute = {
                "/system/bin/su", "/system/xbin/su", "/sbin/su",
                "/su/bin/su", "/debug_ramdisk/su"
        };
        IOException last = null;
        for (String path : absolute) {
            File candidate = new File(path);
            if (!candidate.isFile() || !candidate.canExecute()) continue;
            try {
                return new ProcessBuilder(path).redirectErrorStream(true).start();
            } catch (IOException error) {
                last = error;
            }
        }
        try {
            return new ProcessBuilder("su").redirectErrorStream(true).start();
        } catch (IOException error) {
            if (last != null) error.addSuppressed(last);
            throw new IOException("Root executable was not found; open the root manager and grant A9 TAS", error);
        }
    }

    static synchronized void closePersistent() {
        abortPersistent();
    }

    /**
     * A timed-out root command may still own the shell's foreground execution.
     * Writing "exit" to that shell before destroying it can itself block on
     * vendor su implementations.  Destroy first and never reuse any associated
     * stream/queue after a timeout.
     */
    private static void abortPersistent() {
        Process process = persistentProcess;
        persistentInput = null;
        persistentProcess = null;
        persistentOutput = null;
        if (processAlive(process)) process.destroy();
        // Close the raw process pipe rather than BufferedWriter.close(), which
        // would try to flush buffered text into a wedged su process.
        if (process != null) try { process.getOutputStream().close(); }
        catch (IOException ignored) {}
    }

    /** API-1 compatible process-liveness check; the newer convenience API requires API 26. */
    private static boolean processAlive(Process process) {
        if (process == null) return false;
        try {
            process.exitValue();
            return false;
        } catch (IllegalThreadStateException running) {
            return true;
        }
    }
}
