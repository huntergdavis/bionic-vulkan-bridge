package io.github.huntergdavis.bvb.visiblehost;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.FileDescriptor;
import java.io.FileNotFoundException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.List;

public final class SharedRegionProvider extends ContentProvider {
    private static final String EXTERNAL_BROKER_SOCKET =
            "bvb-e036-";
    private static final int EXTERNAL_SOCKET_TOKEN_CHARS = 32;
    private static final int EXTERNAL_RESPONSE_BYTES = 20;

    static {
        System.loadLibrary("bvb-visible-host");
    }

    static native int nativeOpenRegion(String token);

    static final class ExternalMemoryResult {
        int status;
        ParcelFileDescriptor descriptor;
        long allocationSize;
        int memoryTypeIndex;
        int bufferBytes;
    }

    private static int getI32(byte[] input, int offset) {
        return (input[offset] & 0xff)
                | ((input[offset + 1] & 0xff) << 8)
                | ((input[offset + 2] & 0xff) << 16)
                | ((input[offset + 3] & 0xff) << 24);
    }

    private static long getU64(byte[] input, int offset) {
        return (getI32(input, offset) & 0xffffffffL)
                | ((getI32(input, offset + 4) & 0xffffffffL) << 32);
    }

    private static void readExact(InputStream input, byte[] bytes)
            throws Exception {
        int offset = 0;
        while (offset < bytes.length) {
            int count = input.read(bytes, offset, bytes.length - offset);
            if (count < 0) {
                throw new IllegalStateException(
                        "external broker response ended early");
            }
            offset += count;
        }
    }

    static ExternalMemoryResult openExternalMemory(String token)
            throws Exception {
        if (token == null || token.length() != 64) {
            throw new IllegalArgumentException("invalid lifecycle token");
        }
        LocalSocket socket = null;
        try {
            Exception lastConnectFailure = null;
            for (int attempt = 0; attempt < 100; ++attempt) {
                socket = new LocalSocket();
                try {
                    socket.connect(new LocalSocketAddress(
                            EXTERNAL_BROKER_SOCKET
                                    + token.substring(
                                            0, EXTERNAL_SOCKET_TOKEN_CHARS),
                            LocalSocketAddress.Namespace.ABSTRACT));
                    lastConnectFailure = null;
                    break;
                } catch (Exception failure) {
                    lastConnectFailure = failure;
                    try {
                        socket.close();
                    } catch (Exception ignored) {}
                    socket = null;
                    Thread.sleep(10);
                }
            }
            if (socket == null) {
                throw new SecurityException(
                        "external broker connect failed",
                        lastConnectFailure);
            }
            socket.setSoTimeout(10000);
            OutputStream output = socket.getOutputStream();
            output.write(token.getBytes("US-ASCII"));
            output.flush();
            byte[] response = new byte[EXTERNAL_RESPONSE_BYTES];
            readExact(socket.getInputStream(), response);
            ExternalMemoryResult result = new ExternalMemoryResult();
            result.status = getI32(response, 0);
            result.allocationSize = getU64(response, 4);
            result.memoryTypeIndex = getI32(response, 12);
            result.bufferBytes = getI32(response, 16);
            if (result.status == 0) {
                FileDescriptor[] descriptors =
                        socket.getAncillaryFileDescriptors();
                if (descriptors == null || descriptors.length != 1) {
                    throw new IllegalStateException(
                            "external broker returned no descriptor");
                }
                result.descriptor = ParcelFileDescriptor.dup(descriptors[0]);
            }
            return result;
        } finally {
            if (socket != null) {
                socket.close();
            }
        }
    }

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
