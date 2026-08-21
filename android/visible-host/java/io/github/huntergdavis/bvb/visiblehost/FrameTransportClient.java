package io.github.huntergdavis.bvb.visiblehost;

import android.content.Intent;
import android.net.Credentials;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.hardware.HardwareBuffer;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.RemoteException;

import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStreamWriter;
import java.lang.reflect.Method;

/** Allocation-time relay; this process exits before the native frame loop. */
public final class FrameTransportClient extends Binder {
    private static final int SETUP_MAGIC = 0x31544642;
    private static final int SETUP_VERSION = 1;
    private static final int SETUP_BYTES = 128;
    private static final int SETUP_FLAG_DMA_BUF = 1;
    private static final int SETUP_FLAG_AHARDWAREBUFFER = 2;
    private static final int MAX_IMAGES = 4;
    private static final long RESULT_TIMEOUT_NS = 10000000000L;
    private static final String HOST_PACKAGE =
            "io.github.huntergdavis.bvb.visiblehost";

    private final ParcelFileDescriptor[] descriptors;
    private final HardwareBuffer[] hardwareBuffers;
    private final long[] allocationSizes = new long[MAX_IMAGES];
    private final int[] memoryTypes = new int[MAX_IMAGES];
    private int imageCount;
    private int width;
    private int height;
    private int format;
    private int imageUsage;
    private int setupFlags;
    private long generation;
    private boolean resultDelivered;
    private int nativeStatus;
    private String nativeDetail;

    private FrameTransportClient(byte[] setup, FileDescriptor[] received,
                                 HardwareBuffer[] receivedBuffers)
            throws Exception {
        if (getI32(setup, 0) != SETUP_MAGIC || getU16(setup, 4) != SETUP_VERSION
                || getU16(setup, 6) != SETUP_BYTES) {
            throw new IllegalArgumentException("invalid frame setup header");
        }
        imageCount = getI32(setup, 8);
        width = getI32(setup, 12);
        height = getI32(setup, 16);
        format = getI32(setup, 20);
        imageUsage = getI32(setup, 24);
        setupFlags = getI32(setup, 28);
        generation = getI64(setup, 32);
        if (imageCount < 2 || imageCount > MAX_IMAGES || width <= 0
                || height <= 0 || format == 0 || imageUsage == 0
                || generation == 0L || received == null
                || receivedBuffers == null ||
                received.length !=
                    ((setupFlags & SETUP_FLAG_AHARDWAREBUFFER) != 0
                        ? 1 : imageCount + 1) ||
                receivedBuffers.length !=
                    ((setupFlags & SETUP_FLAG_AHARDWAREBUFFER) != 0
                        ? imageCount : 0)) {
            throw new IllegalArgumentException("invalid frame setup values");
        }
        for (int index = 0; index < MAX_IMAGES; ++index) {
            allocationSizes[index] = getI64(setup, 40 + index * 8);
            memoryTypes[index] = getI32(setup, 72 + index * 4);
            if ((index < imageCount) != (allocationSizes[index] > 0L)) {
                throw new IllegalArgumentException("invalid allocation table");
            }
        }
        if ((setupFlags &
             ~(SETUP_FLAG_DMA_BUF | SETUP_FLAG_AHARDWAREBUFFER)) != 0 ||
                (setupFlags & SETUP_FLAG_DMA_BUF) != 0 &&
                (setupFlags & SETUP_FLAG_AHARDWAREBUFFER) != 0) {
            throw new IllegalArgumentException("unsupported setup flags");
        }
        for (int offset = 88; offset < SETUP_BYTES; offset += 4) {
            if (getI32(setup, offset) != 0) {
                throw new IllegalArgumentException("nonzero setup reserved word");
            }
        }
        descriptors = new ParcelFileDescriptor[received.length];
        hardwareBuffers = receivedBuffers;
        try {
            for (int index = 0; index < received.length; ++index) {
                descriptors[index] = ParcelFileDescriptor.dup(received[index]);
            }
        } catch (Exception failure) {
            closeDescriptors();
            throw failure;
        }
    }

    private static int getU16(byte[] input, int offset) {
        return (input[offset] & 0xff) | ((input[offset + 1] & 0xff) << 8);
    }

    private static int getI32(byte[] input, int offset) {
        return (input[offset] & 0xff)
                | ((input[offset + 1] & 0xff) << 8)
                | ((input[offset + 2] & 0xff) << 16)
                | ((input[offset + 3] & 0xff) << 24);
    }

    private static long getI64(byte[] input, int offset) {
        return (getI32(input, offset) & 0xffffffffL)
                | ((getI32(input, offset + 4) & 0xffffffffL) << 32);
    }

    private static void readExact(InputStream input, byte[] output)
            throws Exception {
        int offset = 0;
        while (offset < output.length) {
            int count = input.read(output, offset, output.length - offset);
            if (count < 0) throw new IllegalStateException("setup ended early");
            offset += count;
        }
    }

    private static IBinder binderFrom(Bundle request) throws Exception {
        Method getBinder = Bundle.class.getMethod("getBinder", String.class);
        return (IBinder)getBinder.invoke(
                request, SharedRegionClient.EXTRA_CALLBACK);
    }

    private void closeDescriptors() {
        for (ParcelFileDescriptor descriptor : descriptors) {
            if (descriptor == null) continue;
            try {
                descriptor.close();
            } catch (Exception ignored) {}
        }
        for (HardwareBuffer buffer : hardwareBuffers) {
            if (buffer != null) buffer.close();
        }
    }

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        if (code == INTERFACE_TRANSACTION) {
            reply.writeString(SharedRegionClient.CALLBACK_DESCRIPTOR);
            return true;
        }
        data.enforceInterface(SharedRegionClient.CALLBACK_DESCRIPTOR);
        if (code == SharedRegionClient.TRANSACTION_REQUEST_GAME_FRAME_RING) {
            reply.writeNoException();
            reply.writeInt(0);
            reply.writeInt(imageCount);
            reply.writeInt(width);
            reply.writeInt(height);
            reply.writeInt(format);
            reply.writeInt(imageUsage);
            reply.writeInt(setupFlags);
            reply.writeLong(generation);
            for (int index = 0; index < imageCount; ++index) {
                reply.writeLong(allocationSizes[index]);
                reply.writeInt(memoryTypes[index]);
            }
            for (HardwareBuffer buffer : hardwareBuffers) {
                buffer.writeToParcel(reply, 0);
            }
            for (ParcelFileDescriptor descriptor : descriptors) {
                descriptor.writeToParcel(reply, 0);
            }
            return true;
        }
        if (code == SharedRegionClient.TRANSACTION_GAME_FRAME_RING_RESULT) {
            int deliveredStatus = data.readInt();
            String deliveredDetail = data.readString();
            synchronized (this) {
                nativeStatus = deliveredStatus;
                nativeDetail = deliveredDetail;
                resultDelivered = true;
                notifyAll();
            }
            reply.writeNoException();
            return true;
        }
        return super.onTransact(code, data, reply, flags);
    }

    private synchronized boolean waitForResult() throws InterruptedException {
        long deadline = System.nanoTime() + RESULT_TIMEOUT_NS;
        while (!resultDelivered) {
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0L) return false;
            wait(remaining / 1000000L, (int)(remaining % 1000000L));
        }
        return true;
    }

    private static void requestInstall(String token, IBinder callback)
            throws Exception {
        Bundle request = new Bundle();
        Method putBinder = Bundle.class.getMethod(
                "putBinder", String.class, IBinder.class);
        putBinder.invoke(request, SharedRegionClient.EXTRA_CALLBACK, callback);
        request.putString(SharedRegionClient.EXTRA_TOKEN, token);
        Intent intent = new Intent(
                SharedRegionClient.ACTION_INSTALL_GAME_FRAME_RING);
        intent.setPackage(HOST_PACKAGE);
        intent.putExtra(SharedRegionClient.EXTRA_REQUEST, request);
        SharedRegionClient.sendBroadcast(intent);
    }

    private static void writeResult(String path, String json) throws Exception {
        OutputStreamWriter writer = new OutputStreamWriter(
                new FileOutputStream(path), "UTF-8");
        writer.write(json);
        writer.write('\n');
        writer.close();
    }

    public static void main(String[] arguments) {
        if (arguments.length != 3) {
            System.err.println(
                    "usage: FrameTransportClient TOKEN RESULT_JSON SETUP_SOCKET");
            System.exit(2);
        }
        String stage = "receive_same_uid_setup";
        FrameTransportClient client = null;
        try {
            LocalServerSocket listener = new LocalServerSocket(arguments[2]);
            LocalSocket socket = listener.accept();
            listener.close();
            Credentials peer = socket.getPeerCredentials();
            if (peer == null || peer.getUid() != Process.myUid()) {
                throw new SecurityException("frame setup sender UID mismatch");
            }
            byte[] setup = new byte[SETUP_BYTES];
            readExact(socket.getInputStream(), setup);
            FileDescriptor[] received = socket.getAncillaryFileDescriptors();
            int setupFlags = getI32(setup, 28);
            int imageCount = getI32(setup, 8);
            HardwareBuffer[] hardwareBuffers = new HardwareBuffer[0];
            if ((setupFlags & SETUP_FLAG_AHARDWAREBUFFER) != 0) {
                ParcelFileDescriptor duplicate =
                        ParcelFileDescriptor.dup(socket.getFileDescriptor());
                int nativeSocket = duplicate.detachFd();
                hardwareBuffers =
                        SharedRegionProvider.nativeReceiveHardwareBuffers(
                                nativeSocket, imageCount);
                if (hardwareBuffers == null ||
                        hardwareBuffers.length != imageCount) {
                    throw new IllegalStateException(
                            "native AHardwareBuffer receive failed");
                }
            }
            client = new FrameTransportClient(setup, received,
                                              hardwareBuffers);
            socket.close();
            stage = "binder_install";
            requestInstall(arguments[0], client);
            if (!client.waitForResult()) {
                throw new IllegalStateException("Activity install callback timed out");
            }
            if (client.nativeStatus != 0) {
                throw new IllegalStateException(
                        "native install failed: " + client.nativeStatus + " "
                                + client.nativeDetail);
            }
            writeResult(arguments[1],
                    "{\"result\":\"pass\",\"setup_transport\":"
                            + "\"same_uid_scm_rights_then_binder_reply\","
                            + "\"image_count\":" + client.imageCount + ","
                            + "\"generation\":" + client.generation + ","
                            + "\"per_frame_java_calls\":0,"
                            + "\"per_frame_binder_calls\":0}");
        } catch (Throwable failure) {
            try {
                writeResult(arguments[1],
                        "{\"result\":\"fail\",\"stage\":\"" + stage
                                + "\",\"detail\":\""
                                + failure.toString().replace("\\", "\\\\")
                                    .replace("\"", "\\\"") + "\"}");
            } catch (Throwable ignored) {}
            System.exit(1);
        } finally {
            if (client != null) client.closeDescriptors();
        }
    }
}
