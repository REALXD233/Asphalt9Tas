package dev.a9tas.android;

/** Pure cold-process recovery decision shared by startup and its offline self-test. */
final class RecoveryPolicy {
    private RecoveryPolicy() {}

    static boolean requiresRecovery(boolean operationActive, boolean hooksInstalled,
                                    boolean serviceLive) {
        // A stale Java operation flag alone cannot have modified the game.  It
        // is safe to clear and retry.  Explicit recovery is reserved for a
        // cold process that may still own installed game hooks.
        return !serviceLive && hooksInstalled;
    }

    static void selfTest() {
        if (requiresRecovery(false, false, false) ||
                requiresRecovery(true, true, true) ||
                requiresRecovery(true, false, true) ||
                requiresRecovery(true, false, false) ||
                !requiresRecovery(false, true, false) ||
                !requiresRecovery(true, true, false))
            throw new IllegalStateException("cold-process recovery policy self-test failed");
    }
}
