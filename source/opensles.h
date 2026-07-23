#ifndef DRASTIC_NX_OPENSLES_H
#define DRASTIC_NX_OPENSLES_H

#include <stddef.h>
#include <stdint.h>

#include "SLES/OpenSLES.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Android's low-latency queue is ABI-compatible with SLBufferQueueItf but
 * carries a distinct interface ID.  Keeping the declaration local avoids a
 * dependency on Android's jni.h-heavy OpenSLES_Android.h. */
struct SLAndroidSimpleBufferQueueItf_;
typedef const struct SLAndroidSimpleBufferQueueItf_ * const *
    SLAndroidSimpleBufferQueueItf;
typedef void (SLAPIENTRY *slAndroidSimpleBufferQueueCallback)(
    SLAndroidSimpleBufferQueueItf caller, void *context);

typedef struct SLAndroidSimpleBufferQueueState_ {
    SLuint32 count;
    SLuint32 index;
} SLAndroidSimpleBufferQueueState;

struct SLAndroidSimpleBufferQueueItf_ {
    SLresult (SLAPIENTRY *Enqueue)(SLAndroidSimpleBufferQueueItf self,
                                  const void *buffer, SLuint32 size);
    SLresult (SLAPIENTRY *Clear)(SLAndroidSimpleBufferQueueItf self);
    SLresult (SLAPIENTRY *GetState)(SLAndroidSimpleBufferQueueItf self,
                                   SLAndroidSimpleBufferQueueState *state);
    SLresult (SLAPIENTRY *RegisterCallback)(
        SLAndroidSimpleBufferQueueItf self,
        slAndroidSimpleBufferQueueCallback callback, void *context);
};

#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE ((SLuint32)0x800007BD)
typedef struct SLDataLocator_AndroidSimpleBufferQueue_ {
    SLuint32 locatorType;
    SLuint32 numBuffers;
} SLDataLocator_AndroidSimpleBufferQueue;

extern const SLInterfaceID SL_IID_ENGINE;
extern const SLInterfaceID SL_IID_PLAY;
extern const SLInterfaceID SL_IID_BUFFERQUEUE;
extern const SLInterfaceID SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
extern const SLInterfaceID SL_IID_VOLUME;
extern const SLInterfaceID SL_IID_RECORD;

void opensles_set_master_volume(unsigned percent);
void opensles_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
