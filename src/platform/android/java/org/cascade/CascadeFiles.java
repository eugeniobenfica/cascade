/* The files cascade's activity hands to other apps (a picture copied to the clipboard): content://
 * URIs of the authority <package>.cascade.files, served read-only from the cache's "shared" folder
 * and nowhere else. Not exported: an app reads one only while the system grants it that URI (the
 * clipboard does, to the app that pastes). The app's manifest declares it:
 *
 *   <provider android:name="org.cascade.CascadeFiles" android:authorities="<package>.cascade.files"
 *             android:exported="false" android:grantUriPermissions="true" />
 */
package org.cascade;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

public class CascadeFiles extends ContentProvider {
    static String authority(Context c) { return c.getPackageName() + ".cascade.files"; }

    static File folder(Context c) { return new File(c.getCacheDir(), "shared"); }

    /* the URI another app reads f by (f in the shared folder) */
    static Uri uriFor(Context c, File f) {
        return new Uri.Builder().scheme("content").authority(authority(c)).appendPath(f.getName()).build();
    }

    /* the file a URI names: one directly in the shared folder, never a path out of it */
    File fileOf(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (name == null || uri.getPathSegments().size() != 1 || name.isEmpty() || name.startsWith(".") || name.indexOf('/') >= 0)
            throw new FileNotFoundException(uri.toString());
        try {
            File dir = folder(getContext()).getCanonicalFile();
            File f = new File(dir, name).getCanonicalFile();
            if (!dir.equals(f.getParentFile()) || !f.isFile()) throw new FileNotFoundException(uri.toString());
            return f;
        } catch (IOException e) {
            throw new FileNotFoundException(uri.toString());
        }
    }

    @Override public boolean onCreate() { return true; }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (mode == null || !mode.equals("r")) throw new FileNotFoundException("read only");
        return ParcelFileDescriptor.open(fileOf(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override public String getType(Uri uri) {
        String name = uri.getLastPathSegment();
        int dot = name == null ? -1 : name.lastIndexOf('.');
        String t = dot < 0 ? null : MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase());
        return t != null ? t : "application/octet-stream";
    }

    @Override public Cursor query(Uri uri, String[] columns, String selection, String[] args, String order) {
        File f;
        try {
            f = fileOf(uri);
        } catch (FileNotFoundException e) {
            return null;
        }
        if (columns == null) columns = new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor c = new MatrixCursor(columns, 1);
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; ++i) {
            if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(columns[i])) row[i] = f.length();
        }
        c.addRow(row);
        return c;
    }

    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException("read only"); }
    @Override public int delete(Uri uri, String selection, String[] args) { throw new UnsupportedOperationException("read only"); }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { throw new UnsupportedOperationException("read only"); }
}
