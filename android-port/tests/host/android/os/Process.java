package android.os;

/** Host-only test double for unique branch staging names; not packaged in APK. */
public final class Process {
    private Process() {}
    public static int myPid() { return (int) java.lang.ProcessHandle.current().pid(); }
}
