package io.github.huntergdavis.bvb.visiblehost;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.lang.reflect.Method;

public final class SharedRegionReceiver extends BroadcastReceiver {
    private static final String LOG_TAG = "BVBSharedRegion";

    private static IBinder callbackFrom(Bundle request) {
        if (request == null) {
            return null;
        }
        try {
            Method getBinder = Bundle.class.getMethod("getBinder", String.class);
            return (IBinder)getBinder.invoke(
                    request, SharedRegionClient.EXTRA_CALLBACK);
        } catch (Exception failure) {
            Log.e(LOG_TAG, "failed to read callback Binder", failure);
            return null;
        }
    }

    @Override
    public void onReceive(Context context, Intent intent) {
        boolean external = SharedRegionClient.ACTION_EXTERNAL_MEMORY.equals(
                intent.getAction());
        if (!external &&
                !SharedRegionClient.ACTION_REQUEST.equals(intent.getAction())) {
            return;
        }
        Bundle request = intent.getBundleExtra(SharedRegionClient.EXTRA_REQUEST);
        IBinder callback = callbackFrom(request);
        String token = request == null
                ? null
                : request.getString(SharedRegionClient.EXTRA_TOKEN);
        if (callback == null) {
            Log.e(LOG_TAG, "shared-region request has no callback");
            return;
        }

        SharedRegionProvider.ExternalMemoryResult externalResult = null;
        int status;
        String failureDetail = null;
        ParcelFileDescriptor region = null;
        try {
            if (external) {
                externalResult = SharedRegionProvider.openExternalMemory(token);
                status = externalResult.status;
                region = externalResult.descriptor;
            } else {
                int descriptor = SharedRegionProvider.nativeOpenRegion(token);
                status = descriptor < 0 ? descriptor : 0;
                if (descriptor >= 0) {
                    region = ParcelFileDescriptor.adoptFd(descriptor);
                }
            }
        } catch (Throwable failure) {
            Log.e(LOG_TAG, "failed to obtain shared descriptor", failure);
            status = failure instanceof SecurityException ? -13 : -5;
            StringBuilder detail = new StringBuilder();
            Throwable current = failure;
            while (current != null) {
                if (detail.length() != 0) detail.append(" <- ");
                detail.append(current.toString());
                current = current.getCause();
            }
            failureDetail = detail.toString();
        }
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(SharedRegionClient.CALLBACK_DESCRIPTOR);
            data.writeInt(status);
            if (status != 0) {
                data.writeString(failureDetail == null
                        ? "native_status=" + status : failureDetail);
            }
            if (region != null) {
                if (external) {
                    data.writeLong(externalResult.allocationSize);
                    data.writeInt(externalResult.memoryTypeIndex);
                    data.writeInt(externalResult.bufferBytes);
                }
                region.writeToParcel(data, 0);
            }
            if (!callback.transact(SharedRegionClient.TRANSACTION_DELIVER,
                                   data, reply, 0)) {
                throw new IllegalStateException("callback rejected transaction");
            }
            reply.readException();
        } catch (Exception failure) {
            Log.e(LOG_TAG, "failed to deliver shared region", failure);
        } finally {
            if (region != null) {
                try {
                    region.close();
                } catch (Exception ignored) {}
            }
            data.recycle();
            reply.recycle();
        }
    }
}
