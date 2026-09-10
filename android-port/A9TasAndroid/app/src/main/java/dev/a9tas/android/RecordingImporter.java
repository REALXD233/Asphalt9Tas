package dev.a9tas.android;

import android.content.Context;
import android.net.Uri;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.UUID;

/** Strict one-shot SAF import into the canonical app-private A9TAS1 library. */
final class RecordingImporter {
    private static final long MAX_ARCHIVE = 512L * 1024L * 1024L + 64L * 1024L +
            A9TasArchive.HEADER_SIZE;

    private RecordingImporter() {}

    static A9TasLibrary.Entry importArchive(Context context, Uri source) throws Exception {
        if (source == null || !"content".equals(source.getScheme()))
            throw new IOException("import source is not a content URI");
        return importStream(context, context.getContentResolver().openInputStream(source));
    }

    static A9TasLibrary.Entry importStream(Context context, InputStream source) throws Exception {
        try (InputStream input = source) {
            if (input == null) throw new IOException("document provider returned no input");
            return copyAndPublish(context, input);
        }
    }

    private static A9TasLibrary.Entry copyAndPublish(Context context, InputStream input) throws Exception {
        File directory = new File(context.getFilesDir(), "library");
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("unable to create A9TAS library");
        File pending = new File(directory, ".import-" + UUID.randomUUID() + ".pending");
        long bytes = 0;
        try {
            try (FileOutputStream output = new FileOutputStream(pending, false)) {
                byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = input.read(buffer)) != -1) {
                    if (count == 0) continue;
                    bytes += count;
                    if (bytes > MAX_ARCHIVE) throw new IOException("A9TAS1 import exceeds size limit");
                    output.write(buffer, 0, count);
                }
                output.flush();
                output.getFD().sync();
            }
            A9TasArchive.Summary summary = A9TasArchive.inspect(pending);
            String id = summary.manifest.getString("recording_id");
            File destination = new File(directory, id + ".a9tas").getCanonicalFile();
            if (!directory.getCanonicalFile().equals(destination.getParentFile()))
                throw new IOException("import destination escaped the private library");
            String sha = A9TasLibrary.sha256(pending);
            if (destination.exists()) {
                A9TasArchive.Summary existing = A9TasArchive.inspect(destination);
                String existingSha = A9TasLibrary.sha256(destination);
                if (!sha.equals(existingSha) ||
                        !existing.manifest.getString("recording_id").equals(id))
                    throw new IOException("recording ID conflicts with an existing archive");
                pending.delete();
                return new A9TasLibrary.Entry(destination, existing, existingSha);
            }
            if (!pending.renameTo(destination)) throw new IOException("atomic import publish failed");
            A9TasArchive.Summary published = A9TasArchive.inspect(destination);
            if (!sha.equals(A9TasLibrary.sha256(destination))) {
                destination.delete();
                throw new IOException("published import identity mismatch");
            }
            return new A9TasLibrary.Entry(destination, published, sha);
        } catch (Exception error) {
            if (pending.exists()) pending.delete();
            throw error;
        }
    }
}
