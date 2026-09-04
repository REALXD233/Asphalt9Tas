package dev.a9tas.android;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.Locale;

/** Exact one-shot export through a user-selected Storage Access Framework URI. */
final class RecordingExporter {
    static final class Receipt {
        final long bytes;
        final String sha256;

        Receipt(long bytes, String sha256) {
            this.bytes = bytes;
            this.sha256 = sha256;
        }
    }

    private RecordingExporter() {}

    static Receipt export(Context context, String sourcePath, String expectedSha,
                          Uri destination) throws Exception {
        if (destination == null || !"content".equals(destination.getScheme()))
            throw new IOException("export destination is not a content URI");
        if (expectedSha == null || !expectedSha.matches("[0-9a-f]{64}"))
            throw new IOException("recording identity is missing");
        File source = new File(sourcePath == null ? "" : sourcePath).getCanonicalFile();
        File rawRoot = new File(context.getFilesDir(), "recordings").getCanonicalFile();
        File archiveRoot = new File(context.getFilesDir(), "library").getCanonicalFile();
        boolean raw = rawRoot.equals(source.getParentFile()) &&
                source.getName().matches("recording-[0-9]+-[0-9]+[.]a9g4r2");
        boolean archive = archiveRoot.equals(source.getParentFile()) &&
                source.getName().matches("[0-9a-f-]{36}[.]a9tas");
        if ((!raw && !archive) || !source.isFile())
            throw new IOException("recording path is outside the private libraries");
        if (archive) A9TasArchive.inspect(source); else A9TasArchive.inspectSource(source);
        String sourceSha = sha256(new FileInputStream(source));
        if (!expectedSha.equals(sourceSha)) throw new IOException("recording changed before export");

        long bytes = 0;
        try (ParcelFileDescriptor descriptor = context.getContentResolver()
                     .openFileDescriptor(destination, "rwt");
             InputStream input = new FileInputStream(source)) {
            if (descriptor == null) throw new IOException("document provider returned no file");
            try (FileOutputStream output = new FileOutputStream(descriptor.getFileDescriptor())) {
                byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (count != 0) {
                        output.write(buffer, 0, count);
                        bytes += count;
                    }
                }
                output.flush();
                output.getFD().sync();
            }
        }
        if (bytes != source.length()) throw new IOException("export byte count mismatch");

        String exportedSha;
        try (InputStream verification = context.getContentResolver().openInputStream(destination)) {
            if (verification == null) throw new IOException("export cannot be reopened for verification");
            exportedSha = sha256(verification);
        }
        if (!sourceSha.equals(exportedSha))
            throw new IOException("exported document hash mismatch; delete the partial document");
        return new Receipt(bytes, exportedSha);
    }

    private static String sha256(InputStream input) throws Exception {
        try (InputStream stream = input) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = stream.read(buffer)) != -1)
                if (count != 0) digest.update(buffer, 0, count);
            StringBuilder output = new StringBuilder(64);
            for (byte value : digest.digest())
                output.append(String.format(Locale.ROOT, "%02x", value & 0xff));
            return output.toString();
        }
    }
}
