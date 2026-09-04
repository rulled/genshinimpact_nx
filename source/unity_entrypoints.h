/* Genshin Impact 7.0.1 / Unity 2017.4.30f1 native registration layout. */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>

#define GENSHIN_NATIVELOADER_TABLE_RVA 0x1496E308u
#define GENSHIN_NATIVELOADER_COUNT     2u
#define GENSHIN_UNITY_TABLE_RVA        0x1479C958u
#define GENSHIN_UNITY_TABLE_COUNT      22u

/* Exact Unity 2017 allocator globals populated during the first nativeRender.
 * Its mmap path requests 4 GiB of usable 8 MiB slabs plus one alignment-sized
 * guard and publishes the raw, length, and aligned values below. */
#define GENSHIN_UNITY_SLAB_RAW_RVA       UINT64_C(0x14FF4840)
#define GENSHIN_UNITY_SLAB_LENGTH_RVA    UINT64_C(0x14FF4848)
#define GENSHIN_UNITY_SLAB_ALIGNED_RVA   UINT64_C(0x14FF4850)
/* R_AARCH64_JUMP_SLOT for mmap@LIBC in this exact source image. */
#define GENSHIN_UNITY_MMAP_GOT_RVA        UINT64_C(0x1489CF68)
#define GENSHIN_UNITY_SLAB_MAP_BYTES     UINT64_C(0x100800000)
#define GENSHIN_UNITY_SLAB_USABLE_BYTES  UINT64_C(0x100000000)
#define GENSHIN_UNITY_SLAB_ALIGNMENT     UINT64_C(0x00800000)
/* The only exact-image load of GENSHIN_UNITY_SLAB_ALIGNED_RVA.  It selects one
 * of 512 8 MiB chunks and immediately initializes headers in that chunk.  The
 * wrapper replaces these four instructions with an on-demand physical commit,
 * then resumes at the first untouched store with the original flags/registers. */
#define GENSHIN_UNITY_SLAB_ACTIVATE_SEQUENCE_RVA UINT64_C(0x044C3A10)
#define GENSHIN_UNITY_SLAB_ACTIVATE_CONTINUE_RVA UINT64_C(0x044C3A20)

/* MiHoYoSDK.Awake in this exact client loads the AndroidJavaObject class-name
 * and shared params-array arguments from the first two slots below.  The final
 * RVA is IL2CPP's native String::NewLen helper, used after the first rendered
 * frame only if the DEX-confirmed Combo bridge class string is not structurally
 * valid. */
/* COMBO_CLASS_NAME_SLOT is an IL2CPP metadata-field pointer reached at runtime
 * through a double indirection (global 0x15A99050 in 1206 -> metadata struct
 * base 0x15CD18A0 -> field +0x1D30), so it has no static ADRP reference and no
 * relocation entry.  Its 1234 address cannot be derived statically.  The slot
 * is only defensively repaired (written when null/mismatched) after the first
 * render, by which point IL2CPP's own MiHoYoSDK.Awake has already populated it.
 * 0xFFFFFFFFFFFFFFFF marks it as "unresolved": the bounds check in
 * repair_combo_managed_class_name skips the Combo-name repair when this RVA is
 * not a valid in-image address, rather than fatally aborting the load. */
#define GENSHIN_COMBO_CLASS_NAME_SLOT_RVA UINT64_C(0xFFFFFFFFFFFFFFFF)
/* EMPTY_OBJECT_ARGS_SLOT and JAVA_FOR_NAME_SLOT were revalidated through
 * matching caller data flow in the 1234 image. */
#define GENSHIN_EMPTY_OBJECT_ARGS_SLOT_RVA UINT64_C(0x14D08D40)
#define GENSHIN_JAVA_FOR_NAME_SLOT_RVA     UINT64_C(0x14D0C120)
/* In 1206 il2cpp_string_new_len was a real callable at 0x0448d3b0.  In 1234
 * the compiler INLINED it: the body logic survives mid-function, with 0 BL
 * callers), and no standalone callable (char*,len)->Il2CppString* exists in
 * the RX segment.  The RVA below is a string-COPY constructor
 * (takes Il2CppString*, not char*+len); calling it as string_new_len made the
 * repair fallback interpret ASCII bytes as a pointer and crash.  It is kept
 * only for the historical bounds-check name and is never called; the NRO
 * reimplements the helper via the game's own GC helpers below. */
#define GENSHIN_IL2CPP_STRING_NEW_LEN_RVA UINT64_C(0x0413D37C)
/* The 1234 Il2CppString construction sequence, verified at multiple callsites:
 *   type_ptr = 0x14967810      (Il2CppString metadata holder; [holder]=class)
 *   alloc_vtable = [0x14DF2000 + 0xFC0]
 *   type resolver 0x7828D50    (x0=type_ptr -> x0=holder; [holder]=class)
 *   sized alloc   0x4126F54    (x0=class,x1=size,x2=alloc_vtable -> obj)
 * Object layout (verified by managed_string_equals):
 *   int32 length @ +0x10, UTF-16 chars @ +0x14.
 * Size = len*2 + 0x14 (header) + 0x2 (NUL). */
#define GENSHIN_IL2CPP_STRING_TYPE_PTR_RVA     UINT64_C(0x14967810)
#define GENSHIN_IL2CPP_GC_PAGE_RVA             UINT64_C(0x14DF2000)
#define GENSHIN_IL2CPP_GC_ALLOC_VTABLE_OFFSET  UINT64_C(0xFC0)
#define GENSHIN_IL2CPP_TYPE_RESOLVER_RVA       UINT64_C(0x7828D50)
#define GENSHIN_IL2CPP_SIZED_ALLOC_RVA         UINT64_C(0x4126F54)
#define GENSHIN_IL2CPP_STRING_HEADER_BYTES     UINT64_C(0x14)
#define GENSHIN_IL2CPP_STRING_TERM_BYTES       UINT64_C(0x2)
#define GENSHIN_COMBO_CLASS_NAME          "com.mihoyo.combosdk.ComboForUnity"
#define GENSHIN_JAVA_FOR_NAME             "forName"

/* Unity 2017's managed FindClass helper.  The exact first pair selects the
 * slash-to-dot Class.forName spelling; the second range enters its generic
 * CallStatic<AndroidJavaObject> path.  The libnx wrapper replaces only these
 * fingerprinted instructions with dot-to-slash AndroidJNISafe.FindClass and
 * an AndroidJavaClass(IntPtr).  Both exact consumers are changed from the
 * AndroidJavaObject m_jobject slot (+0x10) to the m_jclass slot (+0x18). */
#define GENSHIN_JAVA_CLASS_REPLACE_CHARS_RVA UINT64_C(0x11E72FC8)
#define GENSHIN_JAVA_CLASS_GENERIC_CALL_RVA  UINT64_C(0x11E72FD8)
#define GENSHIN_ANDROIDJNI_SAFE_FIND_CLASS_RVA UINT64_C(0x091744B4)
#define GENSHIN_ANDROIDJAVACLASS_METADATA_RVA UINT64_C(0x14AE9068)
#define GENSHIN_IL2CPP_OBJECT_NEW_RVA UINT64_C(0x0F814C34)
#define GENSHIN_ANDROIDJAVACLASS_CTOR_RVA UINT64_C(0x0F42A3E4)
#define GENSHIN_JAVA_CLASS_OBJECT_CONSUMER_RVA UINT64_C(0x11E727EC)
#define GENSHIN_JAVA_CLASS_CLASS_CONSUMER_RVA UINT64_C(0x0F42A2DC)

/* Exact native SerializedFile field-transfer callback implicated by hardware
 * builds af3d52f0 and 6a863a46.  Its caller iterates a field callback table at
 * RVA 0x5135230.  A still-unresolved object/PPtr value of 0x41 was copied into
 * the destination-base slot; the int32 callback then added field offset 0x10
 * and data-aborted while writing address 0x51. */
#define GENSHIN_TRANSFER_INT32_RVA          UINT64_C(0x0563EEB8)
#define GENSHIN_TRANSFER_INT32_RETURN_RVA   UINT64_C(0x05643588)
#define GENSHIN_TRANSFER_REFILL_RVA         UINT64_C(0x0589CB1C)

/* Exact four-instruction Mmoron path sequence:
 * Application path -> Path.GetDirectoryName -> params[0x98] -> Path.Combine.
 * libnx devoptab paths can carry an "sdmc:" prefix, which this Unity/Mono Path
 * implementation rejects.  The patch replaces only this sequence and resumes
 * at its first untouched instruction, leaving the global Path implementation
 * and all unrelated managed callers byte-for-byte unchanged. */
#define GENSHIN_MMORON_DIRECTORY_SEQUENCE_RVA UINT64_C(0x0C68421C)
#define GENSHIN_MMORON_DIRECTORY_CONTINUE_RVA UINT64_C(0x0C68422C)
#define GENSHIN_UNITY_APPLICATION_PATH_GETTER_RVA UINT64_C(0x047C767C)
#define GENSHIN_PATH_GET_DIRECTORY_NAME_RVA UINT64_C(0x0A4813F4)
#define GENSHIN_PATH_COMBINE_RVA UINT64_C(0x0A480D24)

typedef struct {
  const char *name;
  const char *signature;
  void *function;
} GenshinJniNativeMethod;

typedef void    (*fn_initJni)(void *, void *, void *);
typedef void    (*fn_gfxstate)(void *, void *, int32_t, void *);
typedef void    (*fn_v)(void *, void *);
typedef uint8_t (*fn_z)(void *, void *);
typedef void    (*fn_vz)(void *, void *, int32_t);
typedef uint8_t (*fn_inject)(void *, void *, void *, int32_t);

#endif
