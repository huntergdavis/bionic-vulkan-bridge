package io.github.huntergdavis.bvb.visiblehost;

import android.app.ActivityManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.RemoteException;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.lang.reflect.Method;

public final class SharedRegionClient extends Binder {
    public static final String ACTION_REQUEST =
            "io.github.huntergdavis.bvb.visiblehost.REQUEST_SHARED_REGION";
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

    private boolean delivered;
    private int deliveryStatus;
    private ParcelFileDescriptor region;

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
        ParcelFileDescriptor descriptor = status == 0
                ? ParcelFileDescriptor.CREATOR.createFromParcel(data)
                : null;
        synchronized (this) {
            if (delivered) {
                if (descriptor != null) {
                    try {
                        descriptor.close();
                    } catch (Exception ignored) {}
                }
                throw new IllegalStateException("duplicate shared-region delivery");
            }
            deliveryStatus = status;
            region = descriptor;
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

    private static void requestRegion(String token, IBinder callback)
            throws Exception {
        Bundle request = new Bundle();
        Method putBinder = Bundle.class.getMethod(
                "putBinder", String.class, IBinder.class);
        putBinder.invoke(request, EXTRA_CALLBACK, callback);
        request.putString(EXTRA_TOKEN, token);
        Intent intent = new Intent(ACTION_REQUEST);
        intent.setPackage(HOST_PACKAGE);
        intent.putExtra(EXTRA_REQUEST, request);
        sendBroadcast(intent);
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
        if (arguments.length != 2) {
            System.err.println("usage: SharedRegionClient TOKEN RESULT_JSON");
            System.exit(2);
        }
        String stage = "send_broadcast";
        try {
            SharedRegionClient client = new SharedRegionClient();
            requestRegion(arguments[0], client);
            stage = "wait_callback";
            if (!client.waitForDelivery()) {
                throw new IllegalStateException("Binder callback timed out");
            }
            if (client.deliveryStatus != 0) {
                writeResult(arguments[1],
                        "{\"result\":\"fail\",\"stage\":\"request_region\","
                                + "\"native_status\":"
                                + client.deliveryStatus + "}");
                System.exit(1);
            }
            if (client.region == null) {
                throw new IllegalStateException("callback returned null region");
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
