package io.github.huntergdavis.bvb.visiblehost;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.util.Log;

public final class SharedRegionReceiver extends BroadcastReceiver {
    private static final String LOG_TAG = "BVBSharedRegion";

    @Override
    public void onReceive(Context context, Intent intent) {
        if (!SharedRegionClient.ACTION_REQUEST.equals(intent.getAction())) {
            return;
        }
        Bundle request = intent.getBundleExtra(SharedRegionClient.EXTRA_REQUEST);
        IBinder callback = request == null
                ? null
                : request.getBinder(SharedRegionClient.EXTRA_CALLBACK);
        String token = request == null
                ? null
                : request.getString(SharedRegionClient.EXTRA_TOKEN);
        if (callback == null) {
            Log.e(LOG_TAG, "shared-region request has no callback");
            return;
        }

        int descriptor = SharedRegionProvider.nativeOpenRegion(token);
        ParcelFileDescriptor region = descriptor < 0
                ? null
                : ParcelFileDescriptor.adoptFd(descriptor);
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(SharedRegionClient.CALLBACK_DESCRIPTOR);
            data.writeInt(descriptor < 0 ? descriptor : 0);
            if (region != null) {
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
