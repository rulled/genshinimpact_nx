/* Genshin Impact 7.0.0 / Unity 2017.4.30f1 native registration layout. */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>

#define GENSHIN_NATIVELOADER_TABLE_RVA 0x1496DA48u
#define GENSHIN_NATIVELOADER_COUNT     2u
#define GENSHIN_UNITY_TABLE_RVA        0x1479C0B8u
#define GENSHIN_UNITY_TABLE_COUNT      22u

/* Exact Unity 2017 allocator globals populated during the first nativeRender.
 * nativeRender RVA 0x049c3cd4 reaches allocator initializer 0x044ac054; its
 * mmap call at 0x044ac47c requests 4 GiB of usable 8 MiB slabs plus one
 * alignment-sized guard and the stores at 0x044ac49c publish length/alignment. */
#define GENSHIN_UNITY_SLAB_RAW_RVA       UINT64_C(0x14FF3F80)
#define GENSHIN_UNITY_SLAB_LENGTH_RVA    UINT64_C(0x14FF3F88)
#define GENSHIN_UNITY_SLAB_ALIGNED_RVA   UINT64_C(0x14FF3F90)
/* R_AARCH64_JUMP_SLOT for mmap@LIBC in this exact source image. */
#define GENSHIN_UNITY_MMAP_GOT_RVA        UINT64_C(0x1489C6C8)
#define GENSHIN_UNITY_SLAB_MAP_BYTES     UINT64_C(0x100800000)
#define GENSHIN_UNITY_SLAB_USABLE_BYTES  UINT64_C(0x100000000)
#define GENSHIN_UNITY_SLAB_ALIGNMENT     UINT64_C(0x00800000)
/* The only exact-image load of GENSHIN_UNITY_SLAB_ALIGNED_RVA.  It selects one
 * of 512 8 MiB chunks and immediately initializes headers in that chunk.  The
 * wrapper replaces these four instructions with an on-demand physical commit,
 * then resumes at the first untouched store with the original flags/registers. */
#define GENSHIN_UNITY_SLAB_ACTIVATE_SEQUENCE_RVA UINT64_C(0x044C5B20)
#define GENSHIN_UNITY_SLAB_ACTIVATE_CONTINUE_RVA UINT64_C(0x044C5B30)

/* MiHoYoSDK.Awake in this exact client loads the AndroidJavaObject class-name
 * and shared params-array arguments from the first two slots below.  The final
 * RVA is IL2CPP's native String::NewLen helper, used after the first rendered
 * frame only if the DEX-confirmed Combo bridge class string is not structurally
 * valid. */
/* COMBO_CLASS_NAME_SLOT is an IL2CPP metadata-field pointer reached at runtime
 * through a double indirection (global 0x15A99050 in 1206 -> metadata struct
 * base 0x15CD18A0 -> field +0x1D30), so it has no static ADRP reference and no
 * relocation entry.  Its 1224 address cannot be derived statically.  The slot
 * is only defensively repaired (written when null/mismatched) after the first
 * render, by which point IL2CPP's own MiHoYoSDK.Awake has already populated it.
 * 0xFFFFFFFFFFFFFFFF marks it as "unresolved": the bounds check in
 * repair_combo_managed_class_name skips the Combo-name repair when this RVA is
 * not a valid in-image address, rather than fatally aborting the load. */
#define GENSHIN_COMBO_CLASS_NAME_SLOT_RVA UINT64_C(0xFFFFFFFFFFFFFFFF)
/* EMPTY_OBJECT_ARGS_SLOT resolved by caller-trace voting: 68 direct ADRP
 * references in 1206 (page 0x15D4F000, offset 0x860).  The 1224 site wins
 * decisively under both 1-instruction (188 votes) and 2-instruction context
 * matching. */
#define GENSHIN_EMPTY_OBJECT_ARGS_SLOT_RVA UINT64_C(0x14D08480)
#define GENSHIN_JAVA_FOR_NAME_SLOT_RVA     UINT64_C(0x14D0B860)
/* In 1206 il2cpp_string_new_len was a real callable at 0x0448d3b0 (2064 BL
 * callers).  In 1224 the compiler INLINED it: the body logic survives
 * mid-function at 0x448b040 (no prologue, reached via fallthrough, 0 BL
 * callers), and no standalone callable (char*,len)->Il2CppString* exists in
 * the RX segment.  The RVA below (0x0413F2CC) is a string-COPY constructor
 * (takes Il2CppString*, not char*+len); calling it as string_new_len made the
 * repair fallback interpret ASCII bytes as a pointer and crash.  It is kept
 * only for the historical bounds-check name and is never called; the NRO
 * reimplements the helper via the game's own GC helpers below. */
#define GENSHIN_IL2CPP_STRING_NEW_LEN_RVA UINT64_C(0x0413F2CC)
/* The 1224 Il2CppString construction sequence, verified at the game's own
 * "\n"-string builder RVA 0x79b9814:
 *   type_ptr = 0x14966F50      (Il2CppString metadata holder; [holder]=class)
 *   GC page  = 0x14DF2000      (Unity GC slab base, populated by nativeRender)
 *   alloc_vtable = [GC page + 0x700]
 *   type resolver 0x782A890    (x0=type_ptr -> x0=holder; [holder]=class)
 *   sized alloc   0x4128E94    (x0=class,x1=size,x2=alloc_vtable -> obj)
 * Object layout (verified by managed_string_equals):
 *   int32 length @ +0x10, UTF-16 chars @ +0x14.
 * Size = len*2 + 0x14 (header) + 0x2 (NUL).  The sized-alloc helper zeroes the
 * user region ([obj+0x10, obj+size)) via memset(0x782f85c) before returning. */
#define GENSHIN_IL2CPP_STRING_TYPE_PTR_RVA     UINT64_C(0x14966F50)
#define GENSHIN_IL2CPP_GC_PAGE_RVA             UINT64_C(0x14DF2000)
#define GENSHIN_IL2CPP_GC_ALLOC_VTABLE_OFFSET  UINT64_C(0x700)
#define GENSHIN_IL2CPP_TYPE_RESOLVER_RVA       UINT64_C(0x782A890)
#define GENSHIN_IL2CPP_SIZED_ALLOC_RVA         UINT64_C(0x4128E94)
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
#define GENSHIN_JAVA_CLASS_REPLACE_CHARS_RVA UINT64_C(0x11E26DB0)
#define GENSHIN_JAVA_CLASS_GENERIC_CALL_RVA  UINT64_C(0x11E26DC0)
#define GENSHIN_ANDROIDJNI_SAFE_FIND_CLASS_RVA UINT64_C(0x09176188)
#define GENSHIN_ANDROIDJAVACLASS_METADATA_RVA UINT64_C(0x14AE87A8)
#define GENSHIN_IL2CPP_OBJECT_NEW_RVA UINT64_C(0x0F814CA8)
#define GENSHIN_ANDROIDJAVACLASS_CTOR_RVA UINT64_C(0x0F42A838)
#define GENSHIN_JAVA_CLASS_OBJECT_CONSUMER_RVA UINT64_C(0x11E265D4)
#define GENSHIN_JAVA_CLASS_CLASS_CONSUMER_RVA UINT64_C(0x0F42A730)

/* Exact native SerializedFile field-transfer callback implicated by hardware
 * builds af3d52f0 and 6a863a46.  Its caller iterates a field callback table at
 * RVA 0x5135230.  A still-unresolved object/PPtr value of 0x41 was copied into
 * the destination-base slot; the int32 callback then added field offset 0x10
 * and data-aborted while writing address 0x51. */
#define GENSHIN_TRANSFER_INT32_RVA          UINT64_C(0x056409EC)
#define GENSHIN_TRANSFER_INT32_RETURN_RVA   UINT64_C(0x056450BC)
#define GENSHIN_TRANSFER_REFILL_RVA         UINT64_C(0x0589E650)

/* Exact four-instruction Mmoron path sequence:
 * Application path -> Path.GetDirectoryName -> params[0x98] -> Path.Combine.
 * libnx devoptab paths can carry an "sdmc:" prefix, which this Unity/Mono Path
 * implementation rejects.  The patch replaces only this sequence and resumes
 * at its first untouched instruction, leaving the global Path implementation
 * and all unrelated managed callers byte-for-byte unchanged. */
#define GENSHIN_MMORON_DIRECTORY_SEQUENCE_RVA UINT64_C(0x0C684DD4)
#define GENSHIN_MMORON_DIRECTORY_CONTINUE_RVA UINT64_C(0x0C684DE4)
#define GENSHIN_UNITY_APPLICATION_PATH_GETTER_RVA UINT64_C(0x047C978C)
#define GENSHIN_PATH_GET_DIRECTORY_NAME_RVA UINT64_C(0x0A482E14)
#define GENSHIN_PATH_COMBINE_RVA UINT64_C(0x0A482744)

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
