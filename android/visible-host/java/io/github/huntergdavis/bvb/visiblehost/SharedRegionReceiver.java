package io.github.huntergdavis.bvb.visiblehost;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.hardware.HardwareBuffer;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.lang.reflect.Method;

public final class SharedRegionReceiver extends BroadcastReceiver {
    private static final String LOG_TAG = "BVBSharedRegion";

    private static void deliverFrameTransportResult(
            IBinder callback, int status, String detail) {
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(SharedRegionClient.CALLBACK_DESCRIPTOR);
            data.writeInt(status);
            data.writeString(detail);
            if (!callback.transact(
                    SharedRegionClient.TRANSACTION_GAME_FRAME_RING_RESULT,
                    data, reply, 0)) {
                throw new IllegalStateException("frame result rejected");
            }
            reply.readException();
        } catch (Throwable failure) {
            Log.e(LOG_TAG, "failed to deliver frame transport result", failure);
        } finally {
            data.recycle();
            reply.recycle();
        }
    }

    private static void installFrameTransport(IBinder callback, String token) {
        int status = -5;
        String detail = "frame transport setup failed";
        Parcel data = Parcel.obtain();
        Parcel reply = Parcel.obtain();
        ParcelFileDescriptor[] descriptors = null;
        HardwareBuffer[] hardwareBuffers = null;
        int[] detached = null;
        try {
            if (!VisibleHostActivity.authorizes(token)) {
                throw new SecurityException("frame transport capability rejected");
            }
            data.writeInterfaceToken(SharedRegionClient.CALLBACK_DESCRIPTOR);
            if (!callback.transact(
                    SharedRegionClient.TRANSACTION_REQUEST_GAME_FRAME_RING,
                    data, reply, 0)) {
                throw new IllegalStateException("frame setup callback rejected");
            }
            reply.readException();
            int producerStatus = reply.readInt();
            if (producerStatus != 0) {
                throw new IllegalStateException(
                        "frame setup producer status=" + producerStatus);
            }
            int imageCount = reply.readInt();
            int width = reply.readInt();
            int height = reply.readInt();
            int format = reply.readInt();
            int imageUsage = reply.readInt();
            int setupFlags = reply.readInt();
            long generation = reply.readLong();
            if (imageCount < 2 || imageCount > 4) {
                throw new IllegalArgumentException("invalid frame image count");
            }
            long[] allocationSizes = new long[imageCount];
            int[] memoryTypes = new int[imageCount];
            for (int index = 0; index < imageCount; ++index) {
                allocationSizes[index] = reply.readLong();
                memoryTypes[index] = reply.readInt();
            }
            if ((setupFlags & 2) != 0) {
                hardwareBuffers = new HardwareBuffer[imageCount];
                for (int index = 0; index < imageCount; ++index) {
                    hardwareBuffers[index] =
                            HardwareBuffer.CREATOR.createFromParcel(reply);
                }
            } else {
                hardwareBuffers = new HardwareBuffer[0];
            }
            int descriptorCount = (setupFlags & 2) != 0
                    ? 1 : imageCount + 1;
            descriptors = new ParcelFileDescriptor[descriptorCount];
            detached = new int[descriptorCount];
            for (int index = 0; index < detached.length; ++index) {
                detached[index] = -1;
            }
            for (int index = 0; index < descriptors.length; ++index) {
                descriptors[index] =
                        ParcelFileDescriptor.CREATOR.createFromParcel(reply);
                detached[index] = descriptors[index].detachFd();
            }
            status = SharedRegionProvider.nativeInstallFrameTransport(
                    token, imageCount, width, height, format, imageUsage,
                    setupFlags, generation, allocationSizes, memoryTypes,
                    hardwareBuffers, detached);
            /* Native consumes or closes every detached descriptor. */
            detached = null;
            detail = status == 0 ? "installed" : "native_status=" + status;
        } catch (Throwable failure) {
            status = failure instanceof SecurityException ? -13 : -5;
            detail = failure.toString();
            Log.e(LOG_TAG, "failed to install frame transport", failure);
        } finally {
            if (detached != null) {
                for (int descriptor : detached) {
                    if (descriptor >= 0) {
                        try {
                            ParcelFileDescriptor.adoptFd(descriptor).close();
                        } catch (Exception ignored) {}
                    }
                }
            }
            if (descriptors != null) {
                for (ParcelFileDescriptor descriptor : descriptors) {
                    if (descriptor == null) continue;
                    try {
                        descriptor.close();
                    } catch (Exception ignored) {}
                }
            }
            if (hardwareBuffers != null) {
                for (HardwareBuffer buffer : hardwareBuffers) {
                    if (buffer != null) buffer.close();
                }
            }
            data.recycle();
            reply.recycle();
        }
        deliverFrameTransportResult(callback, status, detail);
    }

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
        boolean frameTransportInstall =
                SharedRegionClient.ACTION_INSTALL_GAME_FRAME_RING.equals(
                        intent.getAction());
        boolean frameRing =
                SharedRegionClient.ACTION_EXTERNAL_IMAGE_FRAME_RING.equals(
                        intent.getAction());
        boolean fencedImage =
                SharedRegionClient.ACTION_EXTERNAL_IMAGE_FENCED.equals(
                        intent.getAction());
        boolean externalImage = frameRing || fencedImage ||
                SharedRegionClient.ACTION_EXTERNAL_IMAGE.equals(
                        intent.getAction());
        boolean externalSync = !frameRing && !fencedImage && (externalImage ||
                SharedRegionClient.ACTION_EXTERNAL_SYNC.equals(
                        intent.getAction()));
        boolean external = externalImage || externalSync ||
                SharedRegionClient.ACTION_EXTERNAL_MEMORY.equals(
                intent.getAction());
        if (!frameTransportInstall && !external &&
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
        if (frameTransportInstall) {
            installFrameTransport(callback, token);
            return;
        }

        SharedRegionProvider.ExternalMemoryResult externalResult = null;
        int status;
        String failureDetail = null;
        ParcelFileDescriptor region = null;
        ParcelFileDescriptor syncDescriptor = null;
        try {
            if (external) {
                if (!VisibleHostActivity.authorizes(token)) {
                    throw new SecurityException(
                            "external-memory capability rejected");
                }
                externalResult = SharedRegionProvider.openExternalMemory(
                        token, externalSync, externalImage, fencedImage,
                        frameRing);
                status = externalResult.status;
                region = externalResult.descriptor;
                syncDescriptor = externalResult.syncDescriptor;
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
                    if (externalImage || externalSync) {
                        data.writeInt(externalResult.expectedFillWord);
                    }
                }
                region.writeToParcel(data, 0);
                if (externalSync || frameRing) {
                    syncDescriptor.writeToParcel(data, 0);
                }
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
            if (syncDescriptor != null) {
                try {
                    syncDescriptor.close();
                } catch (Exception ignored) {}
            }
            data.recycle();
            reply.recycle();
        }
    }
}
