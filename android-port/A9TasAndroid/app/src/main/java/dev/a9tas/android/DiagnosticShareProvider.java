package dev.a9tas.android;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/** Read-only, grant-scoped provider for one generated diagnostic ZIP. */
public final class DiagnosticShareProvider extends ContentProvider {
    private static final String MIME = "application/zip";

    static Uri uriFor(android.content.Context context, File file) throws IOException {
        File checked = checkedFile(context, file == null ? "" : file.getName());
        if (!checked.equals(file.getCanonicalFile()))
            throw new IOException("diagnostic share path mismatch");
        return new Uri.Builder().scheme("content")
                .authority(context.getPackageName() + ".diagnostics")
                .appendPath(checked.getName()).build();
    }

    @Override public boolean onCreate() { return true; }

    @Override public String getType(Uri uri) { return MIME; }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode)
            throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("read-only provider");
        try {
            File file = resolve(uri);
            return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
        } catch (IOException error) {
            throw new FileNotFoundException(error.getMessage());
        }
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection,
                                  String[] selectionArgs, String sortOrder) {
        try {
            File file = resolve(uri);
            String[] columns = projection == null ?
                    new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE} : projection;
            MatrixCursor cursor = new MatrixCursor(columns, 1);
            MatrixCursor.RowBuilder row = cursor.newRow();
            for (String column : columns) {
                if (OpenableColumns.DISPLAY_NAME.equals(column)) row.add(file.getName());
                else if (OpenableColumns.SIZE.equals(column)) row.add(file.length());
                else row.add(null);
            }
            return cursor;
        } catch (IOException error) {
            return new MatrixCursor(projection == null ? new String[0] : projection, 0);
        }
    }

    @Override public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("read-only provider");
    }

    @Override public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only provider");
    }

    @Override public int update(Uri uri, ContentValues values, String selection,
                                String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only provider");
    }

    private File resolve(Uri uri) throws IOException {
        if (getContext() == null || uri == null ||
                !getContext().getPackageName().concat(".diagnostics").equals(uri.getAuthority()) ||
                uri.getPathSegments().size() != 1)
            throw new IOException("invalid diagnostic content URI");
        return checkedFile(getContext(), uri.getLastPathSegment());
    }

    private static File checkedFile(android.content.Context context, String name)
            throws IOException {
        if (name == null || !name.matches("A9TAS-diagnostics-[0-9TZ.-]+[.]zip"))
            throw new IOException("invalid diagnostic filename");
        File root = new File(context.getCacheDir(), "diagnostics-export").getCanonicalFile();
        File file = new File(root, name).getCanonicalFile();
        if (!root.equals(file.getParentFile()) || !file.isFile())
            throw new IOException("diagnostic file is unavailable");
        return file;
    }
}
