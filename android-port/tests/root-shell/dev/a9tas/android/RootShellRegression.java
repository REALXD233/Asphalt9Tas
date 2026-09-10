package dev.a9tas.android;

import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.LinkedBlockingQueue;

public final class RootShellRegression {
    static void set(String name, Object value) throws Exception {
        Field f = RootShell.class.getDeclaredField(name);
        f.setAccessible(true); f.set(null, value);
    }
    static void child(String mode) throws Exception {
        BufferedReader reader = new BufferedReader(new InputStreamReader(System.in));
        String begin = reader.readLine();
        if (mode.equals("before")) return;
        System.out.println(begin.substring(5)); System.out.flush();
        if (mode.equals("after")) return;
        reader.readLine();
        String end = reader.readLine();
        if (mode.equals("timeout")) { Thread.sleep(10000); return; }
        int start = end.indexOf("echo ") + 5;
        System.out.println(end.substring(start).replace("$a9tas_rc", "0"));
        System.out.flush();
    }
    static void check(String mode) throws Exception {
        Process p = new ProcessBuilder(new File(System.getProperty("java.home"),
                "bin/java.exe").getPath(), "-cp", System.getProperty("java.class.path"),
                RootShellRegression.class.getName(), "--child", mode).start();
        LinkedBlockingQueue<String> queue = new LinkedBlockingQueue<>();
        set("persistentProcess", p);
        set("persistentInput", new BufferedWriter(new OutputStreamWriter(
                p.getOutputStream(), StandardCharsets.UTF_8)));
        set("persistentOutput", queue);
        Method pump = RootShell.class.getDeclaredMethod("pumpOutput", Process.class,
                LinkedBlockingQueue.class);
        pump.setAccessible(true);
        Thread reader = new Thread(() -> {
            try { pump.invoke(null, p, queue); }
            catch (Exception e) { throw new RuntimeException(e); }
        });
        reader.setDaemon(true); reader.start();
        long start = System.nanoTime();
        try {
            RootShell.Result result = RootShell.runFixedScript("ignored", mode.equals("timeout") ? 1 : 10);
            long ms = (System.nanoTime() - start) / 1000000L;
            if (mode.equals("success")) {
                if (!result.ok()) throw new AssertionError("success marker lost to EOF");
            } else if (mode.equals("timeout")) {
                if (!result.timedOut) throw new AssertionError("timeout missing");
            } else if (result.exitCode != 255 || result.timedOut || ms > 4000) {
                throw new AssertionError("EOF did not return promptly: " + mode + " " + ms);
            }
            System.out.println("ROOT_SHELL_CASE passed=1 mode=" + mode + " ms=" + ms);
        } finally { RootShell.closePersistent(); p.destroy(); }
    }
    public static void main(String[] args) throws Exception {
        if (args.length > 0) { child(args[1]); return; }
        for (String mode : new String[]{"before", "after", "success", "timeout", "success"}) check(mode);
    }
}
