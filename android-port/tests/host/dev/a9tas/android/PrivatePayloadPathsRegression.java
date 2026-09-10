package dev.a9tas.android;

import java.nio.charset.StandardCharsets;

/** Executes the generated prepare shell with only directory existence mocked. */
public final class PrivatePayloadPathsRegression {
    private static int checks;
    private static void run(String bash, int uid, String reported, int reportedUid,
                            String visible, String logical, String staging) throws Exception {
        String resolver = PrivatePayloadPaths.resolveScript(42, "com.game", reported, reportedUid);
        resolver = resolver.replace("a9tas_data_dir(){ [ -d \"$1\" ]; };",
                "a9tas_data_dir(){ [ \"$1\" = \"$VISIBLE\" ]; };");
        String script = "set -eu; uid=" + uid + ";" + resolver
                + "printf 'RESULT|%s|%s\\n' \"$logical_app_data\" \"$stage_app_data\";\n";
        ProcessBuilder pb = new ProcessBuilder(bash, "-s");
        pb.environment().put("VISIBLE", visible);
        pb.environment().put("MSYS_NO_PATHCONV", "1");
        pb.redirectErrorStream(true);
        Process p = pb.start();
        try (var stdin = p.getOutputStream()) { stdin.write(script.getBytes(StandardCharsets.UTF_8)); }
        String output = new String(p.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
        int exit = p.waitFor();
        if (logical == null) {
            if (exit != 73 || !output.contains("G10_ERROR private_data_unavailable"))
                throw new AssertionError("Expected explicit unresolved path failure: " + output);
        } else if (exit != 0 || !output.contains("RESULT|" + logical + "|" + staging)) {
            throw new AssertionError("Wrong storage path: " + output);
        }
        checks++;
    }
    public static void main(String[] args) throws Exception {
        String bash = args[0];
        run(bash, 10123, null, -1, "/data/user/0/com.game", "/data/user/0/com.game", "/data/user/0/com.game");
        run(bash, 10123, null, -1, "/data/data/com.game", "/data/data/com.game", "/data/data/com.game");
        run(bash, 10123, "/mnt/expand/volume/user/0/com.game", 10123,
                "/mnt/expand/volume/user/0/com.game", "/mnt/expand/volume/user/0/com.game", "/mnt/expand/volume/user/0/com.game");
        run(bash, 10123, null, -1, "/proc/42/root/data/user/0/com.game",
                "/data/user/0/com.game", "/proc/42/root/data/user/0/com.game");
        run(bash, 10123, null, -1, "/data_mirror/data_ce/null/0/com.game",
                "/data_mirror/data_ce/null/0/com.game", "/data_mirror/data_ce/null/0/com.game");
        run(bash, 1010123, "/data/user/0/com.game", 10123, "/data/user/10/com.game",
                "/data/user/10/com.game", "/data/user/10/com.game");
        run(bash, 1010123, "/data/user/0/com.game", 10123, "/data/data/com.game", null, null);
        run(bash, 10123, null, -1, "", null, null);
        run(bash, 10123, "/mnt/expand/a'b/user/0/com.game", 10123,
                "/mnt/expand/a'b/user/0/com.game", "/mnt/expand/a'b/user/0/com.game", "/mnt/expand/a'b/user/0/com.game");
        System.out.println("PRIVATE_PAYLOAD_PATHS passed=1 checks=" + checks);
    }
}
