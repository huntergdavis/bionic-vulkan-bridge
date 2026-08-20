#ifndef BVB_NATIVE_BINDER_H
#define BVB_NATIVE_BINDER_H

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/binder_status.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Termux's NDK headers omit binder_manager.h and binder_process.h, but the
 * stable API-29 symbols are exported by Android's libbinder_ndk. */
binder_status_t AServiceManager_addService(AIBinder *binder,
                                           const char *instance);
AIBinder *AServiceManager_checkService(const char *instance);
void ABinderProcess_startThreadPool(void);

#ifdef __cplusplus
}
#endif

#define BVB_NATIVE_BINDER_DESCRIPTOR \
    "io.github.huntergdavis.bvb.visiblehost.IFrameChannel"
#define BVB_NATIVE_BINDER_INSTANCE \
    "io.github.huntergdavis.bvb.visiblehost.frame_channel"

enum {
    BVB_NATIVE_BINDER_TRANSACTION_OPEN = FIRST_CALL_TRANSACTION,
    BVB_NATIVE_BINDER_TOKEN_WORDS = 8,
};

#endif
