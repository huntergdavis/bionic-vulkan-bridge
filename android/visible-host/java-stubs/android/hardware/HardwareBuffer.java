package android.hardware;

import android.os.Parcel;
import android.os.Parcelable;

/** Compile-only API 26 surface; the class is supplied by Android at runtime. */
public final class HardwareBuffer implements Parcelable, AutoCloseable {
    public static final Parcelable.Creator<HardwareBuffer> CREATOR = null;

    private HardwareBuffer() {}

    @Override
    public void close() {}

    @Override
    public int describeContents() {
        return 0;
    }

    @Override
    public void writeToParcel(Parcel destination, int flags) {}
}
