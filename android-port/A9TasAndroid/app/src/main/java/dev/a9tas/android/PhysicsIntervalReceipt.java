package dev.a9tas.android;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** Exact Android decoder for the proven A9PIO2 StepOptions-owner receipt. */
final class PhysicsIntervalReceipt {
    private static final int HEADER_SIZE = 136;
    private static final int SAMPLE_SIZE = 72;
    // BuildProfile v1 with eight hook RVAs places step_options_vtable_rva at 0x80.
    private static final int STEP_OPTIONS_VTABLE_RVA_OFFSET = 128;
    private static final int CLEAN = 1 << 0;
    private static final int TARGET_VERIFIED = 1 << 1;
    private static final int INLINE_ALL = 1 << 3;
    private static final int IDENTITY_STABLE = 1 << 4;
    private static final int INTERVAL_STABLE = 1 << 5;
    private static final int SAMPLE_READ_OK = 1 << 0;
    private static final int SAMPLE_CONTEXT_OK = 1 << 1;
    private static final int SAMPLE_BACKEND_OK = 1 << 2;
    private static final int SAMPLE_INLINE_DEFAULT = 1 << 3;
    private static final int SAMPLE_INTERVAL_FINITE = 1 << 4;
    private static final int SAMPLE_OPTIONS_HEAD_OK = 1 << 5;
    private static final long REFERENCE_HOOK0 = 0x3695474L;
    private static final long REFERENCE_GETTER = 0x3695740L;

    static final class Result {
        final int pid;
        final long libraryBase;
        final long owner;
        final int sampleCount;

        Result(int pid, long libraryBase, long owner, int sampleCount) {
            this.pid = pid;
            this.libraryBase = libraryBase;
            this.owner = owner;
            this.sampleCount = sampleCount;
        }
    }

    private PhysicsIntervalReceipt() {}

    static Result parse(byte[] blob, byte[] buildProfile, int expectedPid,
                        long expectedBase) throws IOException {
        if (blob == null || blob.length < HEADER_SIZE) throw invalid("truncated header");
        if (buildProfile == null || buildProfile.length < 384 ||
                !startsWith(buildProfile, "A9BPR1\0\0") ||
                u32(buildProfile, 8) != 1 || u32(buildProfile, 12) != 384)
            throw invalid("invalid BuildProfile");
        ByteBuffer input = ByteBuffer.wrap(blob).order(ByteOrder.LITTLE_ENDIAN);
        byte[] magic = new byte[8];
        input.get(magic);
        if (!Arrays.equals(magic, "A9PIO2\0\0".getBytes(StandardCharsets.US_ASCII)) ||
                input.getInt(8) != 2 || input.getInt(12) != HEADER_SIZE ||
                input.getInt(16) != SAMPLE_SIZE || input.getInt(132) != 0)
            throw invalid("A9PIO2 ABI mismatch");

        int flags = input.getInt(20);
        long rawPid = input.getLong(24);
        long base = input.getLong(32);
        long sampleCountLong = input.getLong(88);
        long readErrors = input.getLong(96);
        long identityFailures = input.getLong(104);
        long alternateOptions = input.getLong(112);
        long intervalChanges = input.getLong(120);
        float interval = Float.intBitsToFloat(input.getInt(128));
        if (rawPid != (expectedPid & 0xffffffffL) || base != expectedBase ||
                sampleCountLong < 2 || sampleCountLong > 100000)
            throw invalid("identity or sample count mismatch");
        int sampleCount = (int) sampleCountLong;
        long expectedLength = HEADER_SIZE + Math.multiplyExact((long) sampleCount, SAMPLE_SIZE);
        if (blob.length != expectedLength) throw invalid("receipt length mismatch");
        int requiredHeader = CLEAN | TARGET_VERIFIED | IDENTITY_STABLE | INTERVAL_STABLE;
        if ((flags & requiredHeader) != requiredHeader || (flags & INLINE_ALL) != 0 ||
                readErrors != 0 || identityFailures != 0 || intervalChanges != 0 ||
                alternateOptions != sampleCountLong || !Float.isFinite(interval) ||
                interval <= 0.0f || interval > 1.0f)
            throw invalid("header invariants failed");

        long hook0 = u64(buildProfile, 32);
        long expectedVptrRva = u64(buildProfile, STEP_OPTIONS_VTABLE_RVA_OFFSET);
        if (hook0 < REFERENCE_HOOK0 || expectedVptrRva == 0)
            throw invalid("BuildProfile relocation invalid");
        long expectedVptr = checkedAdd(base, expectedVptrRva);
        long expectedGetter = checkedAdd(base,
                checkedAdd(REFERENCE_GETTER, hook0 - REFERENCE_HOOK0));
        int requiredSample = SAMPLE_READ_OK | SAMPLE_CONTEXT_OK | SAMPLE_BACKEND_OK |
                SAMPLE_INTERVAL_FINITE | SAMPLE_OPTIONS_HEAD_OK;
        long owner = 0;
        long word8 = 0;
        long previousTimestamp = 0;
        for (int index = 0; index < sampleCount; ++index) {
            int offset = HEADER_SIZE + index * SAMPLE_SIZE;
            long timestamp = input.getLong(offset);
            long currentOwner = input.getLong(offset + 8);
            long vptr = input.getLong(offset + 16);
            long getter = input.getLong(offset + 24);
            long currentWord8 = input.getLong(offset + 32);
            int sampleFlags = input.getInt(offset + 64);
            if ((index != 0 && Long.compareUnsigned(timestamp, previousTimestamp) <= 0) ||
                    currentOwner == 0 || vptr != expectedVptr || getter != expectedGetter ||
                    (sampleFlags & requiredSample) != requiredSample ||
                    (sampleFlags & SAMPLE_INLINE_DEFAULT) != 0)
                throw invalid("sample invariants failed at " + index);
            if (index == 0) {
                owner = currentOwner;
                word8 = currentWord8;
            } else if (owner != currentOwner || word8 != currentWord8) {
                throw invalid("owner identity was not stable");
            }
            previousTimestamp = timestamp;
        }
        return new Result(expectedPid, expectedBase, owner, sampleCount);
    }

    static byte[] decodeHex(String value) throws IOException {
        if (value == null) throw invalid("missing observation hex");
        String hex = value.replaceAll("\\s+", "");
        if ((hex.length() & 1) != 0 || !hex.matches("[0-9a-fA-F]+"))
            throw invalid("invalid observation hex");
        byte[] bytes = new byte[hex.length() / 2];
        for (int index = 0; index < bytes.length; ++index)
            bytes[index] = (byte) Integer.parseInt(hex.substring(index * 2, index * 2 + 2), 16);
        return bytes;
    }

    static void selfTest() throws IOException {
        long base = 0x100000000L;
        long owner = 0x200000000L;
        long hook0 = REFERENCE_HOOK0 + 0x23790L;
        long vptrRva = 0x7f26300L;
        byte[] profile = new byte[384];
        ByteBuffer p = ByteBuffer.wrap(profile).order(ByteOrder.LITTLE_ENDIAN);
        p.put("A9BPR1\0\0".getBytes(StandardCharsets.US_ASCII));
        p.putInt(8, 1); p.putInt(12, 384);
        p.putLong(32, hook0); p.putLong(STEP_OPTIONS_VTABLE_RVA_OFFSET, vptrRva);
        byte[] trace = new byte[HEADER_SIZE + 2 * SAMPLE_SIZE];
        ByteBuffer t = ByteBuffer.wrap(trace).order(ByteOrder.LITTLE_ENDIAN);
        t.put("A9PIO2\0\0".getBytes(StandardCharsets.US_ASCII));
        t.putInt(8, 2); t.putInt(12, HEADER_SIZE); t.putInt(16, SAMPLE_SIZE);
        t.putInt(20, CLEAN | TARGET_VERIFIED | IDENTITY_STABLE | INTERVAL_STABLE);
        t.putLong(24, 42); t.putLong(32, base); t.putLong(88, 2);
        t.putLong(112, 2); t.putInt(128, Float.floatToRawIntBits(1.0f / 60.0f));
        for (int index = 0; index < 2; ++index) {
            int offset = HEADER_SIZE + index * SAMPLE_SIZE;
            t.putLong(offset, 100 + index);
            t.putLong(offset + 8, owner);
            t.putLong(offset + 16, base + vptrRva);
            t.putLong(offset + 24, base + REFERENCE_GETTER + hook0 - REFERENCE_HOOK0);
            t.putLong(offset + 32, 0x1122334455667788L);
            t.putInt(offset + 64, SAMPLE_READ_OK | SAMPLE_CONTEXT_OK |
                    SAMPLE_BACKEND_OK | SAMPLE_INTERVAL_FINITE | SAMPLE_OPTIONS_HEAD_OK);
        }
        Result result = parse(trace, profile, 42, base);
        if (result.owner != owner || result.sampleCount != 2) throw invalid("self-test result");
        trace[HEADER_SIZE + 24] ^= 1;
        try {
            parse(trace, profile, 42, base);
            throw invalid("negative self-test accepted corruption");
        } catch (IOException expected) {
            if ("negative self-test accepted corruption".equals(expected.getMessage())) throw expected;
        }
    }

    private static long checkedAdd(long left, long right) throws IOException {
        if (left < 0 || right < 0 || Long.MAX_VALUE - left < right)
            throw invalid("address overflow");
        return left + right;
    }

    private static boolean startsWith(byte[] bytes, String value) {
        byte[] prefix = value.getBytes(StandardCharsets.US_ASCII);
        if (bytes.length < prefix.length) return false;
        for (int index = 0; index < prefix.length; ++index)
            if (bytes[index] != prefix[index]) return false;
        return true;
    }

    private static long u32(byte[] bytes, int offset) {
        return ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).getInt(offset) & 0xffffffffL;
    }

    private static long u64(byte[] bytes, int offset) {
        return ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).getLong(offset);
    }

    private static IOException invalid(String detail) {
        return new IOException(detail);
    }
}
