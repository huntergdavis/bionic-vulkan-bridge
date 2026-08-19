package io.github.huntergdavis.bvb.visiblehost;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.FileNotFoundException;
import java.util.List;

public final class SharedRegionProvider extends ContentProvider {
    static native int nativeOpenRegion(String token);
    static native long[] nativeOpenExternalMemory(String token);

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode)
            throws FileNotFoundException {
        List<String> segments = uri.getPathSegments();
        if (!"r".equals(mode) || segments.size() != 2
                || !"region".equals(segments.get(0))) {
            throw new FileNotFoundException("unsupported shared-region URI");
        }
        int descriptor = nativeOpenRegion(segments.get(1));
        if (descriptor < 0) {
            throw new FileNotFoundException(
                    "shared-region authorization failed: " + descriptor);
        }
        return ParcelFileDescriptor.adoptFd(descriptor);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public String getType(Uri uri) {
        return "application/vnd.bvb.shared-region";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("read-only provider");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only provider");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection,
                      String[] selectionArgs) {
        throw new UnsupportedOperationException("read-only provider");
    }
}
