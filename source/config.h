/* Genshin Impact 7.0.1 Switch wrapper configuration. */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)
/* Android reserves large ranges without making every page resident.  Keep the
 * irreducible newlib heap deliberately bounded, then let the shared broker and
 * Unity slab back only live ranges.  Launchers with per-process system-resource
 * memory use svcMapPhysicalMemory directly.  Ordinary hbloader forwarders have
 * a zero SystemResourceSize, so the wrapper instead grows a private heap-donor
 * suffix on demand and donates those pages through the same reversible
 * self-process code alias used by the NRO loader.  History: at 256 MiB the
 * HoYoverse-logo engine-init burst saturated dlmalloc outright (frame 249);
 * 1024 MiB starved the login shader-warmup donor (crash-3 NAK ENOMEM); 768 MiB
 * passed both but its donor ceiling (~1746 MiB) sat exactly at the warmup
 * burst's peak, so every boot became a GC-timing lottery that 10+ runs lost
 * at 3% (0x1f59 donor-alloc-exhausted).  Since then the 64 KiB large-alloc
 * threshold routes every mid-size block to the recyclable pool, and full-run
 * telemetry shows dlmalloc barely touching its arena (sbrk extension 42 MiB,
 * zero denials), so 512 MiB is now safe: dlmalloc still reaches 768 MiB
 * effective through the 256 MiB sbrk extension, while the donor ceiling
 * rises 1:1 to ~2002 MiB and puts the warmup burst's worst observed peak
 * (~1750 MiB) comfortably inside it.  That fixed the login-page warmup, but
 * the post-bundle-download warmup (real game shaders) burst past 2002 MiB,
 * so 384 MiB trades another 128 MiB of staging arena for burst headroom
 * (ceiling ~2130 MiB) while dlmalloc keeps 640 MiB effective; the burst
 * damper in the broker now also sheds the burst rate instead of dying. */
#define OC_MANAGED_BYTES    ((size_t)384 * 1024 * 1024)
/* The port requires a 39-bit process.  Its shared sparse/dynamic arena is
 * larger than the complete heap-donor backing budget, so the old virtual
 * ceiling cannot precede real donor/process admission.  This is an address-
 * space reservation only; individual segments become resident on demand. */
#define OC_DYNAMIC_ARENA_BYTES ((size_t)6144 * 1024 * 1024)
#define OC_DYNAMIC_SEGMENT_BYTES ((size_t)8 * 1024 * 1024)
#define OC_SPARSE_COMMIT_GRANULE_BYTES ((size_t) 1 * 1024 * 1024)
/* Heap-donor fallback granularity is fine enough for caller-owned pthread
 * stacks while every kernel mapping remains at least the 1 MiB sparse commit
 * granule.  Heap size itself changes only at Horizon's 2 MiB alignment. */
#define OC_HEAP_DONOR_UNIT_BYTES    ((size_t)64 * 1024)
#define OC_HEAP_DONOR_INITIAL_BYTES ((size_t)64 * 1024 * 1024)
#define OC_HEAP_DONOR_GROW_BYTES    ((size_t)256 * 1024 * 1024)
/* Launchers without an hbloader heap override (direct NRO loaders,
 * emulators) grant the process a plain heap instead of a pre-split suffix,
 * so the donor ceiling is derived from the total grant minus this margin
 * left for code, stacks, and driver allocations. */
#define OC_HEAP_DONOR_CEILING_MARGIN_BYTES ((size_t)256 * 1024 * 1024)
#define OC_HEAP_DONOR_SHRINK_BYTES  OC_HEAP_DONOR_GROW_BYTES
#define SO_REGION_BYTES     ((size_t) 416 * 1024 * 1024)
/* Public host allocations at or above this size are served straight from the
 * reclaimable sparse pool instead of newlib's dlmalloc.  Newlib-internal
 * allocations (open_memstream, FILE glue) bypass every wrapper and die with
 * ENOMEM once dlmalloc saturates, so the primary heap must stay small.
 * Histogram data from the login-burst NAK panic showed 472 MiB peak-live in
 * dlmalloc, with 465 MiB in >=16 MiB blocks while the pool held GiBs free.
 * The shader warmup burst (thousands of live pipelines) then ratcheted
 * dlmalloc's arena through 64 KiB-1 MiB blocks into the sbrk-extension cap
 * while the donor/pool still held GiBs free (crash-3), so the threshold now
 * routes every mid-size block to the recyclable pool as well. */
#define OC_BROKER_LARGE_ALLOC_BYTES ((size_t)64 * 1024)

#define SS_PACKAGE        "com.miHoYo.GenshinImpact"
#define SS_VERSION_CODE   1234
#define SS_VERSION_NAME   "7.0.1_47144228_47194594"

/* Exact values returned by the bundled 7.0.1 Combo InfoModule.  They are
 * sourced from assets/channel_config_v1.5.json for the reviewed client, not
 * account credentials or server-issued login data. */
#define GENSHIN_CHANNEL_ID_TEXT      "1"
#define GENSHIN_SUB_CHANNEL_ID_TEXT  "1"

#define CONFIG_NAME "config.txt"
#define GAME_HOME   "sdmc:/switch/genshinimpact_nx"
#define CA_BUNDLE_PATH GAME_HOME "/certs/cacert.pem"

extern int screen_width;
extern int screen_height;

// Language. 0 = follow the Switch system language.
#define LANG_AUTO 0
#define LANG_JA   1
#define LANG_EN   2

typedef struct {
  int language;
  int force_vulkan;
  int enable_plugins;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

/* Own-process handle for self code-memory aliasing.  hbloader title
 * overrides hand the NRO a real duplicate of its own process handle;
 * launchers that do not (direct NRO loaders, emulators) leave the env
 * entry invalid, so fall back to the kernel's CUR_PROCESS pseudo-handle,
 * which every svc taking a process handle accepts. */
#include <switch.h>
static inline Handle nx_own_process_handle(void) {
  Handle h = envGetOwnProcessHandle();
  return h == INVALID_HANDLE ? CUR_PROCESS_HANDLE : h;
}

#endif
