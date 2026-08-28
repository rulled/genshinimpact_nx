/* JNI handlers for Unity assets, preferences, display and context services. */
#ifndef UNITY_JNI_H
#define UNITY_JNI_H

#include <stdarg.h>
#include <stdint.h>

void unity_jni_init(const char *data_root);
int unity_owns_class(const char *cls);
const char *unity_class_of(void *obj);
void *unity_make_file(const char *parent, const char *child);
const char *unity_file_path(void *obj);
const char *unity_assets_path(void);
const char *unity_internal_data_path(void);
const char *unity_internal_files_path(void);
const char *unity_external_files_path(void);
int unity_storage_paths_self_test(void);

void    *unity_dispatch_object(void *recv, const void *id, va_list va);
uint64_t unity_dispatch_int(void *recv, const void *id, va_list va);
float    unity_dispatch_float(void *recv, const void *id, va_list va);
void     unity_dispatch_void(void *recv, const void *id, va_list va);

/* jvalue[] variants used by Unity's AndroidJavaObject bridge. */
float    unity_dispatch_float_a(void *recv, const void *id, const void *args);
void    *unity_dispatch_editor_float_a(void *recv, const void *id, const void *args);

int      unity_is_boxed(void *recv);
uint64_t unity_boxed_int(void *recv);
float    unity_boxed_float(void *recv);
int      unity_isinstance(void *obj, const char *clazz);
/* Retain/release stateful handles when JNI creates alias references. */
int      unity_is_handle(void *obj);
int      unity_retain_handle(void *obj);
/* Returns one and releases the object when it is a stateful Unity handle. */
int      unity_release_handle(void *obj);

extern void       *jni_make_string(const char *utf);
extern void       *jni_make_local_string(const char *utf);
extern void       *jni_make_object(const char *label);
extern void       *jni_make_object_array(int length, void *const *items);
extern void       *jni_bytearray_data(void *arr, int *len_out);
extern const char *jni_string_utf(void *jstr);
extern void       *jni_track_local(void *ref);

#endif
