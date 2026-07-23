/* jni_fake.h -- fake JNI environment for libdrastic_arm64.so
 *
 * The core talks to a small Java surface we replace natively: its filesystem
 * path bridge, preferences, package identity, and Android platform metadata.
 * The fake JNIEnv
 * dispatches Call*Method by interned (name,sig); Get*Field reads typed fields off
 * the fake objects built below.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

// The fake JNIEnv / JavaVM handed to the core. Both are pointer-to-pointer-to-
// function-table, matching the JNI ABI the core calls through.
extern void *fake_env;
extern void *fake_vm;

void jni_init(void);

// ---- fake object builders used to hand typed Java objects to the core ----

void *jni_make_string(const char *utf);           // -> fake jstring
const char *jni_string_utf(void *jstr);           // fake jstring -> utf8 (or NULL)

void *jni_obj_new(const char *clsname);           // empty fake object tagged with class
void  jni_obj_set_long  (void *obj, const char *field, int64_t v);
void  jni_obj_set_int   (void *obj, const char *field, int32_t v);
void  jni_obj_set_string(void *obj, const char *field, const char *utf);
void  jni_obj_set_object(void *obj, const char *field, void *child);

void *jni_make_byte_array(const void *data, int len);   // -> fake jbyteArray
void *jni_make_int_array(int len);                       // -> fake jintArray
int jni_byte_array_length(void *array);
const uint8_t *jni_byte_array_data(void *array);
int jni_int_array_length(void *array);
int32_t *jni_int_array_data(void *array);
void jni_release_byte_array(void *array);
void jni_release_int_array(void *array);
void jni_release_string(void *string);
void *jni_make_object_array(int n);                     // n NULL slots
void  jni_obj_array_set(void *arr, int i, void *elem);
int   jni_obj_array_len(void *arr);
void *jni_obj_array_get(void *arr, int i);

#endif
