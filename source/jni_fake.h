/* Minimal JNI environment used by Unity and the managed game. */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

extern void *fake_vm;
extern void *fake_env;

extern volatile int jni_quit_requested;

void jni_init(void);
/* Release per-thread fake-JNI locals/exceptions when a guest pthread returns
 * or calls pthread_exit, matching ART's native-thread detach cleanup. */
void jni_thread_exit_cleanup(void);
int jni_reference_self_test(void);

void *jni_make_string(const char *utf);
/* Create a reclaimable JNI local string.  Use this for credentials, tickets,
 * tokens, and native outputs; jni_make_string() is an interned constant pool. */
void *jni_make_local_string(const char *utf);
void *jni_make_object(const char *label);
void *jni_make_object_array(int length, void *const *items);
const char *jni_string_utf(void *jstr);
void *jni_bytearray_data(void *arr, int *len_out);
/* Register a stateful wrapper allocation as a JNI local reference. */
void *jni_track_local(void *ref);
int jni_push_local_frame(int capacity);
void *jni_pop_local_frame(void *result);
int jni_register_native(const char *class_name, const char *method_name,
                        const char *signature, void *function);
int jni_exception_pending(void);
void jni_exception_clear(void);

/* Native methods captured from RegisterNatives during JNI_OnLoad/load(). */
void *jni_find_registered_native(const char *class_name,
                                 const char *method_name,
                                 const char *signature);
int jni_registered_native_count(void);

#endif
