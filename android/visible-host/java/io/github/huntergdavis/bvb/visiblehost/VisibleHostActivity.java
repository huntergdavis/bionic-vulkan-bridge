package io.github.huntergdavis.bvb.visiblehost;

import android.app.NativeActivity;
import android.content.Intent;
import android.os.Bundle;
import android.view.WindowManager;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

public final class VisibleHostActivity extends NativeActivity {
    private static final String EXTRA_ACTIVITY_TOKEN = "bvb_activity_token";
    private static final int TOKEN_HEX_CHARS = 64;
    private static final Object CAPABILITY_LOCK = new Object();
    private static byte[] activeCapability;

    private byte[] instanceCapability;

    private void installCapability(Intent intent) {
        String token = intent == null
                ? null : intent.getStringExtra(EXTRA_ACTIVITY_TOKEN);
        byte[] capability = token != null && token.length() == TOKEN_HEX_CHARS
                ? token.getBytes(StandardCharsets.US_ASCII) : null;
        synchronized (CAPABILITY_LOCK) {
            instanceCapability = capability;
            activeCapability = capability;
        }
    }

    static boolean authorizes(String token) {
        if (token == null || token.length() != TOKEN_HEX_CHARS) {
            return false;
        }
        byte[] candidate = token.getBytes(StandardCharsets.US_ASCII);
        synchronized (CAPABILITY_LOCK) {
            return activeCapability != null
                    && MessageDigest.isEqual(activeCapability, candidate);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        installCapability(getIntent());
        super.onCreate(savedInstanceState);
        setShowWhenLocked(true);
        setTurnScreenOn(true);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        installCapability(intent);
    }

    @Override
    protected void onDestroy() {
        synchronized (CAPABILITY_LOCK) {
            if (activeCapability == instanceCapability) {
                activeCapability = null;
            }
            instanceCapability = null;
        }
        super.onDestroy();
    }
}
