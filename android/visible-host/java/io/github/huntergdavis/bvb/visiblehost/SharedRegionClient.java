package io.github.huntergdavis.bvb.visiblehost;

import android.app.ActivityManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.RemoteException;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileDescriptor;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.lang.reflect.Method;

public final class SharedRegionClient extends Binder {
    public static final String ACTION_REQUEST =
            "io.github.huntergdavis.bvb.visiblehost.REQUEST_SHARED_REGION";
    public static final String ACTION_EXTERNAL_MEMORY =
            "io.github.huntergdavis.bvb.visiblehost.REQUEST_EXTERNAL_MEMORY";
    public static final String ACTION_EXTERNAL_SYNC =
            "io.github.huntergdavis.bvb.visiblehost.REQUEST_EXTERNAL_SYNC";
    public static final String EXTRA_REQUEST = "bvb_request";
    public static final String EXTRA_CALLBACK = "bvb_callback";
    public static final String EXTRA_TOKEN = "bvb_token";
    public static final String CALLBACK_DESCRIPTOR =
            "io.github.huntergdavis.bvb.visiblehost.ISharedRegionCallback";
    public static final int TRANSACTION_DELIVER = FIRST_CALL_TRANSACTION;

    private static final String TERMUX_PACKAGE = "com.termux";
    private static final String HOST_PACKAGE =
            "io.github.huntergdavis.bvb.visiblehost";
    private static final int INTENT_SENDER_BROADCAST = 1;
    private static final int USER_ID_RANGE = 100000;
    private static final long CALLBACK_TIMEOUT_NS = 10000000000L;
    private static final int RELAY_COMPLETION_TIMEOUT_MS = 300000;
    private static final int PROTOCOL_MAGIC = 0x31425642;
    private static final int PROTOCOL_VERSION = 1;
    private static final int PROTOCOL_REQUEST = 1;
    private static final int PROTOCOL_RESPONSE = 2;
    private static final int OPCODE_HELLO = 1;
    private static final int OPCODE_EXTERNAL_MEMORY_IMPORT_TEST = 50;
    private static final int OPCODE_EXTERNAL_SYNC_IMPORT_TEST = 51;
    private static final int PROTOCOL_HEADER_SIZE = 24;
    private static final int HELLO_REQUEST_SIZE = 8;
    private static final int EXTERNAL_MEMORY_IMPORT_REQUEST_SIZE = 16;
    private static final int EXTERNAL_SYNC_IMPORT_REQUEST_SIZE = 24;
    private static final int RELAY_REQUEST_ID = 0xE021;

    private boolean delivered;
    private int deliveryStatus;
    private String deliveryDetail;
    private ParcelFileDescriptor region;
    private ParcelFileDescriptor syncDescriptor;
    private boolean requestExternal;
    private boolean requestExternalSync;
    private long allocationSize;
    private int memoryTypeIndex;
    private int bufferBytes;
    private int expectedFillWord;

    @Override
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        if (code == INTERFACE_TRANSACTION) {
            reply.writeString(CALLBACK_DESCRIPTOR);
            return true;
        }
        if (code != TRANSACTION_DELIVER) {
            return super.onTransact(code, data, reply, flags);
        }
        data.enforceInterface(CALLBACK_DESCRIPTOR);
        int status = data.readInt();
        String detail = status == 0 ? null : data.readString();
        long deliveredAllocationSize = 0L;
        int deliveredMemoryTypeIndex = 0;
        int deliveredBufferBytes = 0;
        int deliveredExpectedFillWord = 0;
        if (status == 0 && requestExternal) {
            deliveredAllocationSize = data.readLong();
            deliveredMemoryTypeIndex = data.readInt();
            deliveredBufferBytes = data.readInt();
            if (requestExternalSync) {
                deliveredExpectedFillWord = data.readInt();
            }
        }
        ParcelFileDescriptor descriptor = status == 0
                ? ParcelFileDescriptor.CREATOR.createFromParcel(data)
                : null;
        ParcelFileDescriptor deliveredSyncDescriptor =
                status == 0 && requestExternalSync
                ? ParcelFileDescriptor.CREATOR.createFromParcel(data)
                : null;
        synchronized (this) {
            if (delivered) {
                if (descriptor != null) {
                    try {
                        descriptor.close();
                    } catch (Exception ignored) {}
                }
                if (deliveredSyncDescriptor != null) {
                    try {
                        deliveredSyncDescriptor.close();
                    } catch (Exception ignored) {}
                }
                throw new IllegalStateException("duplicate shared-region delivery");
            }
            deliveryStatus = status;
            deliveryDetail = detail;
            region = descriptor;
            syncDescriptor = deliveredSyncDescriptor;
            allocationSize = deliveredAllocationSize;
            memoryTypeIndex = deliveredMemoryTypeIndex;
            bufferBytes = deliveredBufferBytes;
            expectedFillWord = deliveredExpectedFillWord;
            delivered = true;
            notifyAll();
        }
        reply.writeNoException();
        return true;
    }

    private synchronized boolean waitForDelivery() throws InterruptedException {
        long deadline = System.nanoTime() + CALLBACK_TIMEOUT_NS;
        while (!delivered) {
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0L) {
                return false;
            }
            long milliseconds = remaining / 1000000L;
            int nanoseconds = (int)(remaining % 1000000L);
            wait(milliseconds, nanoseconds);
        }
        return true;
    }

    private static Object activityManager() throws Exception {
        Method getService = ActivityManager.class.getDeclaredMethod("getService");
        getService.setAccessible(true);
        return getService.invoke(null);
    }

    private static Method methodNamed(Object target, String name,
                                      Class<?> requiredArrayType) {
        for (Method method : target.getClass().getMethods()) {
            if (!name.equals(method.getName())) {
                continue;
            }
            for (Class<?> type : method.getParameterTypes()) {
                if (type == requiredArrayType) {
                    return method;
                }
            }
        }
        return null;
    }

    private static void sendBroadcast(Intent intent) throws Exception {
        Object manager = activityManager();
        Method getIntentSender = methodNamed(
                manager, "getIntentSender", Intent[].class);
        if (getIntentSender == null) {
            throw new NoSuchMethodException("IActivityManager.getIntentSender");
        }
        Class<?>[] types = getIntentSender.getParameterTypes();
        Object[] values = new Object[types.length];
        boolean packageWritten = false;
        for (int index = 0; index < types.length; ++index) {
            Class<?> type = types[index];
            if (type == int.class) {
                if (index == 0) {
                    values[index] = INTENT_SENDER_BROADCAST;
                } else if (index > 0 && types[index - 1] == String[].class) {
                    values[index] = PendingIntent.FLAG_CANCEL_CURRENT
                            | PendingIntent.FLAG_ONE_SHOT;
                } else if (index == types.length - 1) {
                    values[index] = Process.myUid() / USER_ID_RANGE;
                } else {
                    values[index] = 0;
                }
            } else if (type == String.class && !packageWritten) {
                values[index] = TERMUX_PACKAGE;
                packageWritten = true;
            } else if (type == Intent[].class) {
                values[index] = new Intent[] {intent};
            } else if (type == boolean.class) {
                values[index] = false;
            } else if (type == long.class) {
                values[index] = 0L;
            } else {
                values[index] = null;
            }
        }
        Object sender = getIntentSender.invoke(manager, values);
        if (sender == null) {
            throw new IllegalStateException("AMS returned null intent sender");
        }
        Method send = methodNamed(sender, "send", Intent.class);
        if (send == null) {
            throw new NoSuchMethodException("IIntentSender.send");
        }
        types = send.getParameterTypes();
        values = new Object[types.length];
        for (int index = 0; index < types.length; ++index) {
            Class<?> type = types[index];
            if (type == int.class) {
                values[index] = 0;
            } else if (type == Intent.class) {
                values[index] = intent;
            } else if (type == boolean.class) {
                values[index] = false;
            } else {
                values[index] = null;
            }
        }
        send.invoke(sender, values);
    }

    private static void requestRegion(String token, IBinder callback,
                                      boolean external,
                                      boolean externalSync)
            throws Exception {
        Bundle request = new Bundle();
        Method putBinder = Bundle.class.getMethod(
                "putBinder", String.class, IBinder.class);
        putBinder.invoke(request, EXTRA_CALLBACK, callback);
        request.putString(EXTRA_TOKEN, token);
        Intent intent = new Intent(externalSync ? ACTION_EXTERNAL_SYNC
                : external ? ACTION_EXTERNAL_MEMORY : ACTION_REQUEST);
        intent.setPackage(HOST_PACKAGE);
        intent.putExtra(EXTRA_REQUEST, request);
        sendBroadcast(intent);
    }

    private static void putU16(byte[] output, int offset, int value) {
        output[offset] = (byte)value;
        output[offset + 1] = (byte)(value >>> 8);
    }

    private static void putU32(byte[] output, int offset, int value) {
        output[offset] = (byte)value;
        output[offset + 1] = (byte)(value >>> 8);
        output[offset + 2] = (byte)(value >>> 16);
        output[offset + 3] = (byte)(value >>> 24);
    }

    private static void putU64(byte[] output, int offset, long value) {
        putU32(output, offset, (int)value);
        putU32(output, offset + 4, (int)(value >>> 32));
    }

    private static int getU16(byte[] input, int offset) {
        return (input[offset] & 0xff) | ((input[offset + 1] & 0xff) << 8);
    }

    private static int getU32(byte[] input, int offset) {
        return (input[offset] & 0xff)
                | ((input[offset + 1] & 0xff) << 8)
                | ((input[offset + 2] & 0xff) << 16)
                | ((input[offset + 3] & 0xff) << 24);
    }

    private byte[] relayRequest() {
        int payloadSize = requestExternalSync
                ? EXTERNAL_SYNC_IMPORT_REQUEST_SIZE
                : requestExternal
                    ? EXTERNAL_MEMORY_IMPORT_REQUEST_SIZE : HELLO_REQUEST_SIZE;
        int opcode = requestExternalSync
                ? OPCODE_EXTERNAL_SYNC_IMPORT_TEST
                : requestExternal
                    ? OPCODE_EXTERNAL_MEMORY_IMPORT_TEST : OPCODE_HELLO;
        byte[] request = new byte[PROTOCOL_HEADER_SIZE + payloadSize];
        putU32(request, 0, PROTOCOL_MAGIC);
        putU16(request, 4, PROTOCOL_VERSION);
        putU16(request, 6, PROTOCOL_REQUEST);
        putU16(request, 8, opcode);
        putU32(request, 12, RELAY_REQUEST_ID);
        putU32(request, 16, payloadSize);
        if (requestExternal) {
            putU64(request, PROTOCOL_HEADER_SIZE, allocationSize);
            putU32(request, PROTOCOL_HEADER_SIZE + 8, memoryTypeIndex);
            putU32(request, PROTOCOL_HEADER_SIZE + 12, bufferBytes);
            if (requestExternalSync) {
                putU32(request, PROTOCOL_HEADER_SIZE + 16, expectedFillWord);
                putU32(request, PROTOCOL_HEADER_SIZE + 20, 0);
            }
        } else {
            putU16(request, PROTOCOL_HEADER_SIZE, PROTOCOL_VERSION);
            putU16(request, PROTOCOL_HEADER_SIZE + 2, PROTOCOL_VERSION);
        }
        return request;
    }

    private static void readExact(InputStream input, byte[] bytes)
            throws Exception {
        int offset = 0;
        while (offset < bytes.length) {
            int count = input.read(bytes, offset, bytes.length - offset);
            if (count < 0) {
                throw new IllegalStateException("relay response ended early");
            }
            offset += count;
        }
    }

    private long relayRegion(String socketName,
                             ParcelFileDescriptor descriptor)
            throws Exception {
        LocalSocket socket = new LocalSocket();
        long started = System.nanoTime();
        try {
            socket.connect(new LocalSocketAddress(
                    socketName, LocalSocketAddress.Namespace.ABSTRACT));
            socket.setSoTimeout(RELAY_COMPLETION_TIMEOUT_MS);
            socket.setFileDescriptorsForSend(requestExternalSync
                    ? new FileDescriptor[] {
                        descriptor.getFileDescriptor(),
                        syncDescriptor.getFileDescriptor()
                    }
                    : new FileDescriptor[] {descriptor.getFileDescriptor()});
            OutputStream output = socket.getOutputStream();
            output.write(relayRequest());
            output.flush();

            byte[] response = new byte[PROTOCOL_HEADER_SIZE];
            readExact(socket.getInputStream(), response);
            if (getU32(response, 0) != PROTOCOL_MAGIC
                    || getU16(response, 4) != PROTOCOL_VERSION
                    || getU16(response, 6) != PROTOCOL_RESPONSE
                    || getU16(response, 8) != (requestExternalSync
                            ? OPCODE_EXTERNAL_SYNC_IMPORT_TEST
                            : requestExternal
                                ? OPCODE_EXTERNAL_MEMORY_IMPORT_TEST
                                : OPCODE_HELLO)
                    || getU32(response, 12) != RELAY_REQUEST_ID
                    || getU32(response, 16) != 0
                    || getU32(response, 20) != 0) {
                throw new IllegalStateException("invalid relay response");
            }
            return System.nanoTime() - started;
        } finally {
            socket.close();
        }
    }

    private static void writeResult(String path, String json) throws Exception {
        FileOutputStream stream = new FileOutputStream(path);
        OutputStreamWriter writer = new OutputStreamWriter(stream, "UTF-8");
        writer.write(json);
        writer.write('\n');
        writer.close();
    }

    private static String jsonString(String value) {
        StringBuilder result = new StringBuilder();
        result.append('"');
        for (int index = 0; index < value.length(); ++index) {
            char character = value.charAt(index);
            if (character == '"' || character == '\\') {
                result.append('\\');
            }
            if (character == '\n') {
                result.append("\\n");
            } else if (character == '\r') {
                result.append("\\r");
            } else if (character == '\t') {
                result.append("\\t");
            } else if (character >= 0x20) {
                result.append(character);
            }
        }
        result.append('"');
        return result.toString();
    }

    private static String exceptionChain(Throwable failure) {
        StringBuilder result = new StringBuilder();
        Throwable current = failure;
        while (current != null) {
            if (result.length() != 0) {
                result.append(" <- ");
            }
            result.append(current.getClass().getName());
            if (current.getMessage() != null) {
                result.append(": ");
                result.append(current.getMessage());
            }
            current = current.getCause();
        }
        return result.toString();
    }

    public static void main(String[] arguments) {
        boolean externalSync = arguments.length == 4 &&
                "sync".equals(arguments[3]);
        boolean external = externalSync || arguments.length == 4 &&
                "external".equals(arguments[3]);
        if ((arguments.length != 2 && arguments.length != 3 && !external) ||
                (arguments.length == 4 && !external)) {
            System.err.println(
                    "usage: SharedRegionClient TOKEN RESULT_JSON "
                            + "[RELAY_SOCKET [external|sync]]");
            System.exit(2);
        }
        String stage = "send_broadcast";
        try {
            SharedRegionClient client = new SharedRegionClient();
            client.requestExternal = external;
            client.requestExternalSync = externalSync;
            requestRegion(arguments[0], client, external, externalSync);
            stage = "wait_callback";
            if (!client.waitForDelivery()) {
                throw new IllegalStateException("Binder callback timed out");
            }
            if (client.deliveryStatus != 0) {
                writeResult(arguments[1],
                        "{\"result\":\"fail\",\"stage\":\"request_region\","
                                + "\"native_status\":"
                                + client.deliveryStatus
                                + ",\"native_detail\":"
                                + jsonString(client.deliveryDetail == null
                                        ? "" : client.deliveryDetail) + "}");
                System.exit(1);
            }
            if (client.region == null) {
                throw new IllegalStateException("callback returned null region");
            }
            if (arguments.length >= 3) {
                stage = "relay_scm_rights";
                long relayRoundTripNs = client.relayRegion(
                        arguments[2], client.region);
                client.region.close();
                if (client.syncDescriptor != null) {
                    client.syncDescriptor.close();
                }
                writeResult(arguments[1],
                        "{\"result\":\"pass\",\"binder_region_received\":true,"
                                + "\"descriptor_kind\":"
                                + (externalSync
                                    ? "\"opaque_memory_plus_sync_fd\""
                                    : external ? "\"opaque_fd\""
                                    : "\"memfd\"")
                                + ",\"relay\":\"same_uid_scm_rights\","
                                + (external
                                    ? "\"allocation_size\":"
                                            + client.allocationSize
                                            + ",\"memory_type_index\":"
                                            + client.memoryTypeIndex
                                            + ",\"buffer_bytes\":"
                                            + client.bufferBytes + ","
                                            + (externalSync
                                                ? "\"expected_fill_word\":"
                                                    + (client.expectedFillWord
                                                        & 0xffffffffL) + ","
                                                : "")
                                    : "")
                                + "\"relay_round_trip_ns\":"
                                + relayRoundTripNs + "}");
                return;
            }
            stage = "read_region";
            FileInputStream input =
                    new ParcelFileDescriptor.AutoCloseInputStream(client.region);
            byte[] bytes = new byte[8192];
            int length = 0;
            while (length < bytes.length) {
                int count = input.read(bytes, length, bytes.length - length);
                if (count < 0) {
                    break;
                }
                length += count;
            }
            input.close();
            stage = "validate_region";
            byte[] marker =
                    "BVB_E020_SHARED_REGION binder_parcel_fd=PASS\n"
                            .getBytes("UTF-8");
            if (length != 4096 || marker.length > bytes.length) {
                throw new IllegalStateException("unexpected region length");
            }
            for (int index = 0; index < marker.length; ++index) {
                if (bytes[index] != marker[index]) {
                    throw new IllegalStateException("marker mismatch");
                }
            }
            writeResult(arguments[1],
                    "{\"result\":\"pass\",\"region_bytes\":4096,"
                            + "\"marker\":\"BVB_E020_SHARED_REGION\"}");
        } catch (Throwable failure) {
            failure.printStackTrace(System.err);
            try {
                writeResult(arguments[1],
                        "{\"result\":\"fail\",\"stage\":"
                                + jsonString(stage) + ",\"exception\":"
                                + jsonString(exceptionChain(failure)) + "}");
            } catch (Throwable ignored) {}
            System.exit(1);
        }
    }
}
