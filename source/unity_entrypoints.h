/* Genshin Impact 6.7.0 / Unity 2017.4.30f1 native registration layout. */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>

#define GENSHIN_NATIVELOADER_TABLE_RVA 0x1593DEB8u
#define GENSHIN_NATIVELOADER_COUNT     2u
#define GENSHIN_UNITY_TABLE_RVA        0x15775FD0u
#define GENSHIN_UNITY_TABLE_COUNT      22u

/* Exact Unity 2017 allocator globals populated during the first nativeRender.
 * nativeRender RVA 0x049c3cd4 reaches allocator initializer 0x044ac054; its
 * mmap call at 0x044ac47c requests 4 GiB of usable 8 MiB slabs plus one
 * alignment-sized guard and the stores at 0x044ac49c publish length/alignment. */
#define GENSHIN_UNITY_SLAB_RAW_RVA       UINT64_C(0x15F9C780)
#define GENSHIN_UNITY_SLAB_LENGTH_RVA    UINT64_C(0x15F9C788)
#define GENSHIN_UNITY_SLAB_ALIGNED_RVA   UINT64_C(0x15F9C790)
/* R_AARCH64_JUMP_SLOT for mmap@LIBC in this exact source image. */
#define GENSHIN_UNITY_MMAP_GOT_RVA        UINT64_C(0x158761B0)
#define GENSHIN_UNITY_SLAB_MAP_BYTES     UINT64_C(0x100800000)
#define GENSHIN_UNITY_SLAB_USABLE_BYTES  UINT64_C(0x100000000)
#define GENSHIN_UNITY_SLAB_ALIGNMENT     UINT64_C(0x00800000)
/* The only exact-image load of GENSHIN_UNITY_SLAB_ALIGNED_RVA.  It selects one
 * of 512 8 MiB chunks and immediately initializes headers in that chunk.  The
 * wrapper replaces these four instructions with an on-demand physical commit,
 * then resumes at the first untouched store with the original flags/registers. */
#define GENSHIN_UNITY_SLAB_ACTIVATE_SEQUENCE_RVA UINT64_C(0x044B5684)
#define GENSHIN_UNITY_SLAB_ACTIVATE_CONTINUE_RVA UINT64_C(0x044B5694)

/* MiHoYoSDK.Awake in this exact client loads the AndroidJavaObject class-name
 * and shared params-array arguments from the first two slots below.  The final
 * RVA is IL2CPP's native String::NewLen helper, used after the first rendered
 * frame only if the DEX-confirmed Combo bridge class string is not structurally
 * valid. */
#define GENSHIN_COMBO_CLASS_NAME_SLOT_RVA UINT64_C(0x15CD35D0)
#define GENSHIN_EMPTY_OBJECT_ARGS_SLOT_RVA UINT64_C(0x15D4F860)
#define GENSHIN_JAVA_FOR_NAME_SLOT_RVA     UINT64_C(0x15CBFF08)
#define GENSHIN_IL2CPP_STRING_NEW_LEN_RVA UINT64_C(0x0448D3B0)
#define GENSHIN_COMBO_CLASS_NAME          "com.mihoyo.combosdk.ComboForUnity"
#define GENSHIN_JAVA_FOR_NAME             "forName"

/* Unity 2017's managed FindClass helper.  The exact first pair selects the
 * slash-to-dot Class.forName spelling; the second range enters its generic
 * CallStatic<AndroidJavaObject> path.  The libnx wrapper replaces only these
 * fingerprinted instructions with dot-to-slash AndroidJNISafe.FindClass and
 * an AndroidJavaClass(IntPtr).  Both exact consumers are changed from the
 * AndroidJavaObject m_jobject slot (+0x10) to the m_jclass slot (+0x18). */
#define GENSHIN_JAVA_CLASS_REPLACE_CHARS_RVA UINT64_C(0x141BEDD8)
#define GENSHIN_JAVA_CLASS_GENERIC_CALL_RVA  UINT64_C(0x141BEDEC)
#define GENSHIN_ANDROIDJNI_SAFE_FIND_CLASS_RVA UINT64_C(0x141A0430)
#define GENSHIN_ANDROIDJAVACLASS_METADATA_RVA UINT64_C(0x15AB36F0)
#define GENSHIN_IL2CPP_OBJECT_NEW_RVA UINT64_C(0x0F2B53E8)
#define GENSHIN_ANDROIDJAVACLASS_CTOR_RVA UINT64_C(0x1419700C)
#define GENSHIN_JAVA_CLASS_OBJECT_CONSUMER_RVA UINT64_C(0x141BE690)
#define GENSHIN_JAVA_CLASS_CLASS_CONSUMER_RVA UINT64_C(0x14196EFC)

/* Exact native SerializedFile field-transfer callback implicated by hardware
 * builds af3d52f0 and 6a863a46.  Its caller iterates a field callback table at
 * RVA 0x5135230.  A still-unresolved object/PPtr value of 0x41 was copied into
 * the destination-base slot; the int32 callback then added field offset 0x10
 * and data-aborted while writing address 0x51. */
#define GENSHIN_TRANSFER_INT32_RVA          UINT64_C(0x05130DAC)
#define GENSHIN_TRANSFER_INT32_RETURN_RVA   UINT64_C(0x05135234)
#define GENSHIN_TRANSFER_REFILL_RVA         UINT64_C(0x0536242C)

/* Exact four-instruction Mmoron path sequence:
 * Application path -> Path.GetDirectoryName -> params[0x98] -> Path.Combine.
 * libnx devoptab paths can carry an "sdmc:" prefix, which this Unity/Mono Path
 * implementation rejects.  The patch replaces only this sequence and resumes
 * at its first untouched instruction, leaving the global Path implementation
 * and all unrelated managed callers byte-for-byte unchanged. */
#define GENSHIN_MMORON_DIRECTORY_SEQUENCE_RVA UINT64_C(0x096C4324)
#define GENSHIN_MMORON_DIRECTORY_CONTINUE_RVA UINT64_C(0x096C4334)
#define GENSHIN_UNITY_APPLICATION_PATH_GETTER_RVA UINT64_C(0x045466B4)
#define GENSHIN_PATH_GET_DIRECTORY_NAME_RVA UINT64_C(0x13D820F8)
#define GENSHIN_PATH_COMBINE_RVA UINT64_C(0x13D81B9C)

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
