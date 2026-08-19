package io.github.huntergdavis.bvb.visiblehost;

import android.content.Context;
import android.net.Uri;
import android.os.ParcelFileDescriptor;

import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.lang.reflect.Method;

public final class SharedRegionClient {
    private SharedRegionClient() {}

    private static Context termuxContext() throws Exception {
        Class<?> activityThreadClass = Class.forName("android.app.ActivityThread");
        Method systemMain = activityThreadClass.getDeclaredMethod("systemMain");
        systemMain.setAccessible(true);
        Object activityThread = systemMain.invoke(null);
        Method getSystemContext =
                activityThreadClass.getDeclaredMethod("getSystemContext");
        getSystemContext.setAccessible(true);
        Context systemContext = (Context)getSystemContext.invoke(activityThread);
        return systemContext.createPackageContext(
                "com.termux", Context.CONTEXT_IGNORE_SECURITY);
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
            System.err.println("usage: SharedRegionClient URI RESULT_JSON");
            System.exit(2);
        }
        String stage = "bootstrap_context";
        try {
            Context context = termuxContext();
            stage = "open_provider";
            ParcelFileDescriptor descriptor = context.getContentResolver()
                    .openFileDescriptor(Uri.parse(arguments[0]), "r");
            if (descriptor == null) {
                throw new IllegalStateException("provider returned null");
            }
            FileInputStream input =
                    new ParcelFileDescriptor.AutoCloseInputStream(descriptor);
            stage = "read_region";
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
