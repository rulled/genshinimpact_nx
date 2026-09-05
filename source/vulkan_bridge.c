/* Vulkan WSI bridge for an Android Unity player hosted on Horizon.
 *
 * Mesa NVK exposes Nintendo's VK_NN_vi_surface.  Unity 2017 asks the Android
 * loader for VK_KHR_android_surface instead.  The two create-info layouts are
 * deliberately equivalent for the fields used here and our ANativeWindow is
 * already the libnx NWindow singleton, so only the extension name, sType, and
 * entry point need translation.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "vulkan_bridge.h"

/* Public loaderless NVK archives intentionally export only
 * vkGetInstanceProcAddr. Resolve every other command through that root instead
 * of requiring Mesa's private entry points to remain global at static link. */
static VkInstance g_bridge_instance;

static PFN_vkVoidFunction driver_proc(VkInstance instance, const char *name) {
  return name ? vkGetInstanceProcAddr(instance, name) : NULL;
}

static PFN_vkVoidFunction device_proc(VkDevice device, const char *name) {
  const VkInstance instance = __atomic_load_n(&g_bridge_instance,
                                               __ATOMIC_ACQUIRE);
  PFN_vkGetDeviceProcAddr get_device_proc =
    (PFN_vkGetDeviceProcAddr)driver_proc(instance, "vkGetDeviceProcAddr");
  return get_device_proc && name ? get_device_proc(device, name) : NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkEnumerateInstanceVersion(
    uint32_t *version);
static VKAPI_ATTR VkResult VKAPI_CALL nx_vkEnumerateInstanceLayerProperties(
    uint32_t *property_count, VkLayerProperties *properties);

/* ---- vkCreateImage / vkAllocateMemory failure logging ---- */

static int g_img_fail_count;
static int g_mem_fail_count;
static int g_memprop_dumped;
static int g_imgmemreq_count;
static int g_bind_fail_count;
static int g_submit_count;
static int g_submit_fail_count;
static int g_throttle_drains;
static int g_present_count;
static int g_present_fail_count;
static int g_acquire_count;
static int g_acquire_fail_count;
static int g_alloc_large_count;

#define NX_VK_MEMORY_RECORD_CAPACITY 512
#define NX_VK_THROTTLE_PERIOD 256

typedef struct {
  VkDevice device;
  VkDeviceMemory memory;
  VkDeviceSize allocation_size;
  VkDeviceSize map_offset;
  VkDeviceSize map_size;
  void *mapped;
} NxVkMemoryRecord;

static NxVkMemoryRecord g_memory_records[NX_VK_MEMORY_RECORD_CAPACITY];
static volatile unsigned char g_memory_records_lock;
static uint64_t g_flush_ranges;
static uint64_t g_flush_bytes;
static uint64_t g_flush_invalid;
static uint64_t g_memory_record_overflows;
static uint64_t g_acquire_success;
static uint64_t g_acquire_suboptimal;
static uint64_t g_acquire_out_of_date;
static uint64_t g_acquire_other;
static uint64_t g_submit_success;
static uint64_t g_present_success;
static uint64_t g_present_suboptimal;
static uint64_t g_present_out_of_date;
static uint64_t g_present_other;
static uint64_t g_present_result_success;
static uint64_t g_present_result_suboptimal;
static uint64_t g_present_result_out_of_date;
static uint64_t g_present_result_other;
static uint64_t g_queue_contentions;
static uint64_t g_queue_wait_ticks;
static uint64_t g_queue_max_wait_ticks;
static int g_dcache_mode;
static int g_queue_serialization_mode;
static Mutex g_queue_mutex;
static int g_vi_availability_logged;
static int g_vi_creation_logged;
static int g_surface_support_logged;
static int g_surface_capabilities_logged;
static int g_surface_formats_logged;
static int g_present_modes_logged;
static int g_queue_families_logged;
static int g_device_queues_logged;
static int g_device_queue_get_logged;
static uint64_t g_swapchain_creations;
static uint64_t g_swapchain_recreations;

static void records_lock(void) {
  while (__atomic_test_and_set(&g_memory_records_lock, __ATOMIC_ACQUIRE))
    svcSleepThread(0);
}

static void records_unlock(void) {
  __atomic_clear(&g_memory_records_lock, __ATOMIC_RELEASE);
}

static int env_enabled(const char *name, int *state) {
  int value = __atomic_load_n(state, __ATOMIC_ACQUIRE);
  if (value) return value == 2;
  const char *text = getenv(name);
  const int selected = text && text[0] && strcmp(text, "0") != 0 ? 2 : 1;
  int expected = 0;
  (void)__atomic_compare_exchange_n(state, &expected, selected, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED);
  return __atomic_load_n(state, __ATOMIC_ACQUIRE) == 2;
}

static void log_line(const char *format, ...) {
  va_list args;
  FILE *file = fopen(GAME_HOME "/run_log.txt", "ab");
  if (!file) return;
  va_start(args, format);
  vfprintf(file, format, args);
  va_end(args);
  fclose(file);
}

static void queue_lock_if_enabled(int *locked) {
  /* This diagnostic mode serializes only the intercepted submit and present
   * calls. It does not serialize every Vulkan queue operation. */
  *locked = env_enabled("NX_VK_SERIALIZE_QUEUE", &g_queue_serialization_mode);
  if (!*locked) return;
  if (mutexTryLock(&g_queue_mutex)) return;
  const uint64_t start = armGetSystemTick();
  mutexLock(&g_queue_mutex);
  const uint64_t waited = armGetSystemTick() - start;
  __atomic_add_fetch(&g_queue_contentions, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_queue_wait_ticks, waited, __ATOMIC_RELAXED);
  uint64_t maximum = __atomic_load_n(&g_queue_max_wait_ticks, __ATOMIC_RELAXED);
  while (waited > maximum &&
         !__atomic_compare_exchange_n(&g_queue_max_wait_ticks, &maximum,
                                      waited, 0, __ATOMIC_RELAXED,
                                      __ATOMIC_RELAXED)) {}
}

static void queue_unlock_if_enabled(int locked) {
  if (locked) mutexUnlock(&g_queue_mutex);
}

static int log_once(int *flag) {
  int expected = 0;
  return __atomic_compare_exchange_n(flag, &expected, 1, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

static NxVkMemoryRecord *find_memory_record(VkDevice device,
                                             VkDeviceMemory memory) {
  for (size_t i = 0; i < NX_VK_MEMORY_RECORD_CAPACITY; ++i) {
    NxVkMemoryRecord *record = &g_memory_records[i];
    if (record->device == device && record->memory == memory) return record;
  }
  return NULL;
}

static void track_memory_allocation(VkDevice device, VkDeviceMemory memory,
                                    VkDeviceSize size) {
  uint64_t overflow = 0;
  records_lock();
  NxVkMemoryRecord *record = find_memory_record(device, memory);
  if (!record) {
    for (size_t i = 0; i < NX_VK_MEMORY_RECORD_CAPACITY; ++i) {
      if (g_memory_records[i].memory != VK_NULL_HANDLE) continue;
      record = &g_memory_records[i];
      break;
    }
  }
  if (record) {
    *record = (NxVkMemoryRecord){
      .device = device,
      .memory = memory,
      .allocation_size = size,
    };
  } else {
    overflow = __atomic_add_fetch(&g_memory_record_overflows, 1,
                                  __ATOMIC_RELAXED);
  }
  records_unlock();
  if (overflow && env_enabled("NX_VK_DCACHE_FLUSH", &g_dcache_mode) &&
      overflow <= 30) {
    log_line("[E] VK dcache registry FULL overflow=%llu memory=%p "
             "size=%llu; preserving Vulkan allocation without CPU flush "
             "tracking\n",
             (unsigned long long)overflow, (void *)(uintptr_t)memory,
             (unsigned long long)size);
  }
}

static void forget_memory(VkDevice device, VkDeviceMemory memory) {
  records_lock();
  NxVkMemoryRecord *record = find_memory_record(device, memory);
  if (record) memset(record, 0, sizeof(*record));
  records_unlock();
}

static void log_image_failure(VkResult result, const VkImageCreateInfo *ci) {
  int n = __atomic_add_fetch(&g_img_fail_count, 1, __ATOMIC_SEQ_CST);
  if (n > 60) return;
  FILE *f = fopen(GAME_HOME "/run_log.txt", "ab");
  if (!f) return;
  fprintf(f,
      "[E] vkCreateImage FAIL #%d result=%d fmt=%d type=%d tiling=%d "
      "usage=0x%x flags=0x%x extent=%ux%ux%u mip=%u arr=%u samples=%u\n",
      n, (int)result, (int)ci->format, (int)ci->imageType,
      (int)ci->tiling, (unsigned)ci->usage, (unsigned)ci->flags,
      ci->extent.width, ci->extent.height, ci->extent.depth,
      ci->mipLevels, ci->arrayLayers, (unsigned)ci->samples);
  fclose(f);
}

static void log_alloc_failure(VkResult result, const VkMemoryAllocateInfo *ai) {
  int n = __atomic_add_fetch(&g_mem_fail_count, 1, __ATOMIC_SEQ_CST);
  if (n > 60) return;
  FILE *f = fopen(GAME_HOME "/run_log.txt", "ab");
  if (!f) return;
  fprintf(f,
      "[E] vkAllocateMemory FAIL #%d result=%d size=%llu typeIdx=%u\n",
      n, (int)result, (unsigned long long)ai->allocationSize,
      ai->memoryTypeIndex);
  fclose(f);
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkCreateImage(
    VkDevice device, const VkImageCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkImage *image) {
  PFN_vkCreateImage create_image =
    (PFN_vkCreateImage)device_proc(device, "vkCreateImage");
  if (!create_image) return VK_ERROR_INITIALIZATION_FAILED;
  VkResult result = create_image(device, create_info, allocator, image);
  if (result != VK_SUCCESS && create_info) log_image_failure(result, create_info);
  return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo *alloc_info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *memory) {
  PFN_vkAllocateMemory allocate_memory =
    (PFN_vkAllocateMemory)device_proc(device, "vkAllocateMemory");
  if (!allocate_memory) return VK_ERROR_INITIALIZATION_FAILED;
  VkResult result = allocate_memory(device, alloc_info, allocator, memory);
  if (result != VK_SUCCESS && alloc_info) log_alloc_failure(result, alloc_info);
  if (result == VK_SUCCESS && alloc_info && memory)
    track_memory_allocation(device, *memory, alloc_info->allocationSize);
  if (result == VK_SUCCESS && alloc_info &&
      alloc_info->allocationSize >= (VkDeviceSize)(50 * 1024 * 1024)) {
    int n = __atomic_add_fetch(&g_alloc_large_count, 1, __ATOMIC_SEQ_CST);
    if (n <= 60) {
      FILE *f = fopen(GAME_HOME "/run_log.txt", "ab");
      if (f) {
        fprintf(f, "[I] vkAllocateMemory #%d size=%lluM typeIdx=%u\n",
                n,
                (unsigned long long)(alloc_info->allocationSize / (1024 * 1024)),
                alloc_info->memoryTypeIndex);
        fflush(f);
        fclose(f);
      }
    }
  }
  return result;
}

static VKAPI_ATTR void VKAPI_CALL nx_vkFreeMemory(
    VkDevice device, VkDeviceMemory memory,
    const VkAllocationCallbacks *allocator) {
  PFN_vkFreeMemory free_memory =
    (PFN_vkFreeMemory)device_proc(device, "vkFreeMemory");
  if (!free_memory) return;
  forget_memory(device, memory);
  free_memory(device, memory, allocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkMapMemory(
    VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **data) {
  PFN_vkMapMemory map_memory =
    (PFN_vkMapMemory)device_proc(device, "vkMapMemory");
  if (!map_memory) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = map_memory(device, memory, offset, size, flags, data);
  if (result != VK_SUCCESS || !data) return result;
  records_lock();
  NxVkMemoryRecord *record = find_memory_record(device, memory);
  if (record && offset <= record->allocation_size &&
      (size == VK_WHOLE_SIZE || size <= record->allocation_size - offset)) {
    record->map_offset = offset;
    record->map_size = size == VK_WHOLE_SIZE
      ? record->allocation_size - offset : size;
    record->mapped = *data;
  }
  records_unlock();
  return result;
}

static VKAPI_ATTR void VKAPI_CALL nx_vkUnmapMemory(
    VkDevice device, VkDeviceMemory memory) {
  PFN_vkUnmapMemory unmap_memory =
    (PFN_vkUnmapMemory)device_proc(device, "vkUnmapMemory");
  if (!unmap_memory) return;
  records_lock();
  NxVkMemoryRecord *record = find_memory_record(device, memory);
  if (record) {
    record->mapped = NULL;
    record->map_offset = 0;
    record->map_size = 0;
  }
  records_unlock();
  unmap_memory(device, memory);
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkFlushMappedMemoryRanges(
    VkDevice device, uint32_t range_count,
    const VkMappedMemoryRange *ranges) {
  PFN_vkFlushMappedMemoryRanges flush =
    (PFN_vkFlushMappedMemoryRanges)device_proc(
      device, "vkFlushMappedMemoryRanges");
  if (!flush) return VK_ERROR_INITIALIZATION_FAILED;
  if (env_enabled("NX_VK_DCACHE_FLUSH", &g_dcache_mode) && ranges) {
    for (uint32_t i = 0; i < range_count; ++i) {
      void *address = NULL;
      size_t length = 0;
      records_lock();
      NxVkMemoryRecord *record = find_memory_record(device, ranges[i].memory);
      if (record && record->mapped &&
          ranges[i].offset >= record->map_offset &&
          ranges[i].offset <= record->allocation_size) {
        const VkDeviceSize relative = ranges[i].offset - record->map_offset;
        const VkDeviceSize available = relative <= record->map_size
          ? record->map_size - relative : 0;
        const VkDeviceSize requested = ranges[i].size == VK_WHOLE_SIZE
          ? record->allocation_size - ranges[i].offset : ranges[i].size;
        if (relative <= SIZE_MAX && requested && requested <= available &&
            requested <= SIZE_MAX) {
          address = (unsigned char *)record->mapped + (size_t)relative;
          length = (size_t)requested;
        }
      }
      records_unlock();
      if (!address) {
        __atomic_add_fetch(&g_flush_invalid, 1, __ATOMIC_RELAXED);
        continue;
      }
      armDCacheFlush(address, length);
      __atomic_add_fetch(&g_flush_ranges, 1, __ATOMIC_RELAXED);
      __atomic_add_fetch(&g_flush_bytes, length, __ATOMIC_RELAXED);
    }
  }
  return flush(device, range_count, ranges);
}

static VKAPI_ATTR void VKAPI_CALL nx_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *props) {
  const VkInstance instance = __atomic_load_n(&g_bridge_instance,
                                               __ATOMIC_ACQUIRE);
  PFN_vkGetPhysicalDeviceMemoryProperties get_properties =
    (PFN_vkGetPhysicalDeviceMemoryProperties)driver_proc(
      instance, "vkGetPhysicalDeviceMemoryProperties");
  if (!get_properties) {
    if (props) memset(props, 0, sizeof(*props));
    return;
  }
  if (!props) return;
  get_properties(pd, props);
  for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
    const uint32_t heap_index = props->memoryTypes[i].heapIndex;
    if (heap_index >= props->memoryHeapCount) continue;
    if (props->memoryHeaps[heap_index].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
      props->memoryTypes[i].propertyFlags |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  }
  if (!g_memprop_dumped) {
    g_memprop_dumped = 1;
    FILE *f = fopen(GAME_HOME "/run_log.txt", "ab");
    if (!f) return;
    fprintf(f, "[I] === VkPhysicalDeviceMemoryProperties ===\n");
    fprintf(f, "[I] memoryTypeCount=%u memoryHeapCount=%u\n",
        props->memoryTypeCount, props->memoryHeapCount);
    for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
      fprintf(f, "[I] memType[%u] heap=%u flags=0x%x\n", i,
          props->memoryTypes[i].heapIndex,
          (unsigned)props->memoryTypes[i].propertyFlags);
    }
    for (uint32_t i = 0; i < props->memoryHeapCount; i++) {
      fprintf(f, "[I] heap[%u] size=%llu flags=0x%x\n", i,
          (unsigned long long)props->memoryHeaps[i].size,
          (unsigned)props->memoryHeaps[i].flags);
    }
    fclose(f);
  }
}

static VKAPI_ATTR void VKAPI_CALL nx_vkGetImageMemoryRequirements(
    VkDevice device, VkImage image, VkMemoryRequirements *reqs) {
  PFN_vkGetImageMemoryRequirements get_requirements =
    (PFN_vkGetImageMemoryRequirements)device_proc(
      device, "vkGetImageMemoryRequirements");
  if (!get_requirements) {
    if (reqs) memset(reqs, 0, sizeof(*reqs));
    return;
  }
  get_requirements(device, image, reqs);
  if (reqs) {
    int n = __atomic_add_fetch(&g_imgmemreq_count, 1, __ATOMIC_SEQ_CST);
    if (n <= 30) {
      FILE *f = fopen(GAME_HOME "/run_log.txt", "ab");
      if (f) {
        fprintf(f, "[I] GetImageMemReq #%d size=%llu align=%llu typeBits=0x%x\n",
            n, (unsigned long long)reqs->size,
            (unsigned long long)reqs->alignment,
            (unsigned)reqs->memoryTypeBits);
        fclose(f);
      }
    }
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkBindImageMemory(
    VkDevice device, VkImage image, VkDeviceMemory mem, VkDeviceSize offset) {
  PFN_vkBindImageMemory bind_image_memory =
    (PFN_vkBindImageMemory)device_proc(device, "vkBindImageMemory");
  if (!bind_image_memory) return VK_ERROR_INITIALIZATION_FAILED;
  VkResult result = bind_image_memory(device, image, mem, offset);
  if (result != VK_SUCCESS) {
    int n = __atomic_add_fetch(&g_bind_fail_count, 1, __ATOMIC_SEQ_CST);
    if (n <= 30) {
      FILE *f = fopen(GAME_HOME "/run_log.txt", "ab");
      if (f) {
        fprintf(f, "[E] vkBindImageMemory FAIL #%d result=%d\n", n, (int)result);
        fclose(f);
      }
    }
  }
  return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkQueueSubmit(
    VkQueue queue, uint32_t submit_count,
    const VkSubmitInfo *submits, VkFence fence) {
  const VkInstance inst = __atomic_load_n(&g_bridge_instance, __ATOMIC_ACQUIRE);
  PFN_vkQueueSubmit submit =
    (PFN_vkQueueSubmit)driver_proc(inst, "vkQueueSubmit");
  if (!submit) return VK_ERROR_INITIALIZATION_FAILED;
  int locked = 0;
  queue_lock_if_enabled(&locked);
  VkResult result = submit(queue, submit_count, submits, fence);
  queue_unlock_if_enabled(locked);
  int n = __atomic_add_fetch(&g_submit_count, 1, __ATOMIC_SEQ_CST);
  uint64_t successful_submit = 0;
  if (result == VK_SUCCESS) {
    successful_submit = __atomic_add_fetch(&g_submit_success, 1,
                                            __ATOMIC_RELAXED);
  }
  if (result != VK_SUCCESS) {
    int fn = __atomic_add_fetch(&g_submit_fail_count, 1, __ATOMIC_SEQ_CST);
    FILE *f = fopen(GAME_HOME "/run_log.txt", "ab");
    if (f && fn <= 30) {
      fprintf(f, "[E] vkQueueSubmit FAIL #%d result=%d (total calls=%d)\n",
              fn, (int)result, n);
      fflush(f);
      fclose(f);
    }
  }
  /* Preserve the proven production drain unless submit/present serialization
   * is selected for a clean comparison. The diagnostic mutex and periodic
   * wait must not be active in the same run. */
  if (successful_submit &&
      (successful_submit % NX_VK_THROTTLE_PERIOD) == 0 && !locked) {
    PFN_vkQueueWaitIdle wait_idle =
      (PFN_vkQueueWaitIdle)driver_proc(inst, "vkQueueWaitIdle");
    if (wait_idle) {
      const VkResult drain = wait_idle(queue);
      const int drain_number = __atomic_add_fetch(&g_throttle_drains, 1,
                                                   __ATOMIC_SEQ_CST);
      if (drain != VK_SUCCESS && drain_number <= 30) {
        log_line("[E] vkQueueWaitIdle(throttle) #%d result=%d "
                 "(at successful submit=%llu)\n",
                 drain_number, (int)drain,
                 (unsigned long long)successful_submit);
      }
    }
  }
  return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *present_info) {
  const VkInstance inst = __atomic_load_n(&g_bridge_instance, __ATOMIC_ACQUIRE);
  PFN_vkQueuePresentKHR present =
    (PFN_vkQueuePresentKHR)driver_proc(inst, "vkQueuePresentKHR");
  if (!present) return VK_ERROR_INITIALIZATION_FAILED;
  int locked = 0;
  queue_lock_if_enabled(&locked);
  VkResult result = present(queue, present_info);
  queue_unlock_if_enabled(locked);
  int n = __atomic_add_fetch(&g_present_count, 1, __ATOMIC_SEQ_CST);
  if (result == VK_SUCCESS)
    __atomic_add_fetch(&g_present_success, 1, __ATOMIC_RELAXED);
  else if (result == VK_SUBOPTIMAL_KHR)
    __atomic_add_fetch(&g_present_suboptimal, 1, __ATOMIC_RELAXED);
  else if (result == VK_ERROR_OUT_OF_DATE_KHR)
    __atomic_add_fetch(&g_present_out_of_date, 1, __ATOMIC_RELAXED);
  else
    __atomic_add_fetch(&g_present_other, 1, __ATOMIC_RELAXED);
  if (present_info && present_info->pResults) {
    for (uint32_t i = 0; i < present_info->swapchainCount; ++i) {
      const VkResult swapchain_result = present_info->pResults[i];
      if (swapchain_result == VK_SUCCESS)
        __atomic_add_fetch(&g_present_result_success, 1, __ATOMIC_RELAXED);
      else if (swapchain_result == VK_SUBOPTIMAL_KHR)
        __atomic_add_fetch(&g_present_result_suboptimal, 1, __ATOMIC_RELAXED);
      else if (swapchain_result == VK_ERROR_OUT_OF_DATE_KHR)
        __atomic_add_fetch(&g_present_result_out_of_date, 1,
                           __ATOMIC_RELAXED);
      else
        __atomic_add_fetch(&g_present_result_other, 1, __ATOMIC_RELAXED);
    }
  }
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    int fn = __atomic_add_fetch(&g_present_fail_count, 1, __ATOMIC_SEQ_CST);
    FILE *f = fopen(GAME_HOME "/run_log.txt", "ab");
    if (f && fn <= 30) {
      fprintf(f, "[E] vkQueuePresentKHR FAIL #%d result=%d (total calls=%d)\n",
              fn, (int)result, n);
      fflush(f);
      fclose(f);
    }
  }
  return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *image_index) {
  PFN_vkAcquireNextImageKHR acquire =
    (PFN_vkAcquireNextImageKHR)device_proc(device, "vkAcquireNextImageKHR");
  if (!acquire) return VK_ERROR_INITIALIZATION_FAILED;
  VkResult result = acquire(device, swapchain, timeout, semaphore, fence,
                           image_index);
  int n = __atomic_add_fetch(&g_acquire_count, 1, __ATOMIC_SEQ_CST);
  if (result == VK_SUCCESS)
    __atomic_add_fetch(&g_acquire_success, 1, __ATOMIC_RELAXED);
  else if (result == VK_SUBOPTIMAL_KHR)
    __atomic_add_fetch(&g_acquire_suboptimal, 1, __ATOMIC_RELAXED);
  else if (result == VK_ERROR_OUT_OF_DATE_KHR)
    __atomic_add_fetch(&g_acquire_out_of_date, 1, __ATOMIC_RELAXED);
  else
    __atomic_add_fetch(&g_acquire_other, 1, __ATOMIC_RELAXED);
  if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
    int fn = __atomic_add_fetch(&g_acquire_fail_count, 1, __ATOMIC_SEQ_CST);
    FILE *f = fopen(GAME_HOME "/run_log.txt", "ab");
    if (f && fn <= 30) {
      fprintf(f, "[E] vkAcquireNextImageKHR FAIL #%d result=%d (total calls=%d)\n",
              fn, (int)result, n);
      fflush(f);
      fclose(f);
    }
  }
  return result;
}

static int is_android_surface(const char *name) {
  return name && strcmp(name, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME) == 0;
}

VKAPI_ATTR VkResult VKAPI_CALL nx_vkCreateInstance(
  const VkInstanceCreateInfo *create_info,
  const VkAllocationCallbacks *allocator,
  VkInstance *instance) {
  if (!create_info) return VK_ERROR_INITIALIZATION_FAILED;

  const char *translated[64];
  if (create_info->enabledExtensionCount > (uint32_t)(sizeof translated / sizeof translated[0]) ||
      (create_info->enabledExtensionCount && !create_info->ppEnabledExtensionNames))
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  for (uint32_t i = 0; i < create_info->enabledExtensionCount; i++) {
    const char *name = create_info->ppEnabledExtensionNames[i];
    translated[i] = is_android_surface(name)
                      ? VK_NN_VI_SURFACE_EXTENSION_NAME : name;
  }

  VkInstanceCreateInfo local = *create_info;
  local.ppEnabledExtensionNames = translated;
  PFN_vkCreateInstance create_instance =
    (PFN_vkCreateInstance)driver_proc(VK_NULL_HANDLE, "vkCreateInstance");
  if (!create_instance) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = create_instance(&local, allocator, instance);
  if (result == VK_SUCCESS && instance)
    __atomic_store_n(&g_bridge_instance, *instance, __ATOMIC_RELEASE);
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL nx_vkEnumerateInstanceExtensionProperties(
  const char *layer_name,
  uint32_t *property_count,
  VkExtensionProperties *properties) {
  PFN_vkEnumerateInstanceExtensionProperties enumerate =
    (PFN_vkEnumerateInstanceExtensionProperties)driver_proc(
      VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
  if (!enumerate) return VK_ERROR_INITIALIZATION_FAILED;
  VkResult result = enumerate(layer_name, property_count, properties);
  if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && property_count && properties) {
    int has_vi_surface = 0;
    for (uint32_t i = 0; i < *property_count; i++) {
      if (strcmp(properties[i].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME) == 0) {
        has_vi_surface = 1;
        strncpy(properties[i].extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
                sizeof properties[i].extensionName - 1);
        properties[i].extensionName[sizeof properties[i].extensionName - 1] = '\0';
        properties[i].specVersion = VK_KHR_ANDROID_SURFACE_SPEC_VERSION;
      }
    }
    if (log_once(&g_vi_availability_logged))
      log_line("[VK] VI surface available=%d extensions=%u result=%d\n",
               has_vi_surface, *property_count, (int)result);
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL nx_vkCreateAndroidSurfaceKHR(
  VkInstance instance,
  const VkAndroidSurfaceCreateInfoKHR *create_info,
  const VkAllocationCallbacks *allocator,
  VkSurfaceKHR *surface) {
  if (!create_info || !create_info->window)
    return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
  const VkViSurfaceCreateInfoNN vi_info = {
    .sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
    .pNext = create_info->pNext,
    .flags = (VkViSurfaceCreateFlagsNN)create_info->flags,
    .window = create_info->window,
  };
  PFN_vkCreateViSurfaceNN create_surface =
    (PFN_vkCreateViSurfaceNN)driver_proc(instance, "vkCreateViSurfaceNN");
  const VkResult result = create_surface
    ? create_surface(instance, &vi_info, allocator, surface)
    : VK_ERROR_EXTENSION_NOT_PRESENT;
  if (log_once(&g_vi_creation_logged))
    log_line("[VK] VI surface create entry=%d result=%d surface=%p\n",
             create_surface != NULL, (int)result,
             surface && result == VK_SUCCESS ? (void *)(uintptr_t)*surface
                                             : NULL);
  return result;
}

static VKAPI_ATTR void VKAPI_CALL nx_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physical_device, uint32_t *count,
    VkQueueFamilyProperties *properties) {
  const VkInstance instance = __atomic_load_n(&g_bridge_instance,
                                               __ATOMIC_ACQUIRE);
  PFN_vkGetPhysicalDeviceQueueFamilyProperties get_properties =
    (PFN_vkGetPhysicalDeviceQueueFamilyProperties)driver_proc(
      instance, "vkGetPhysicalDeviceQueueFamilyProperties");
  if (!get_properties) {
    if (count) *count = 0;
    return;
  }
  get_properties(physical_device, count, properties);
  if (count && properties && log_once(&g_queue_families_logged)) {
    FILE *file = fopen(GAME_HOME "/run_log.txt", "ab");
    if (!file) return;
    fprintf(file, "[VK] queue families=%u", *count);
    for (uint32_t i = 0; i < *count; ++i)
      fprintf(file, " [%u flags=0x%x count=%u]", i,
              (unsigned)properties[i].queueFlags, properties[i].queueCount);
    fputc('\n', file);
    fclose(file);
  }
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkCreateDevice(
    VkPhysicalDevice physical_device, const VkDeviceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkDevice *device) {
  const VkInstance instance = __atomic_load_n(&g_bridge_instance,
                                               __ATOMIC_ACQUIRE);
  PFN_vkCreateDevice create_device =
    (PFN_vkCreateDevice)driver_proc(instance, "vkCreateDevice");
  if (!create_device) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = create_device(physical_device, create_info,
                                        allocator, device);
  if (create_info &&
      (!create_info->queueCreateInfoCount || create_info->pQueueCreateInfos) &&
      log_once(&g_device_queues_logged)) {
    FILE *file = fopen(GAME_HOME "/run_log.txt", "ab");
    if (file) {
      fprintf(file, "[VK] device create result=%d queues=%u", (int)result,
              create_info->queueCreateInfoCount);
      for (uint32_t i = 0; i < create_info->queueCreateInfoCount; ++i) {
        const VkDeviceQueueCreateInfo *queue = &create_info->pQueueCreateInfos[i];
        fprintf(file, " [family=%u count=%u]", queue->queueFamilyIndex,
                queue->queueCount);
      }
      fputc('\n', file);
      fclose(file);
    }
  }
  return result;
}

static VKAPI_ATTR void VKAPI_CALL nx_vkGetDeviceQueue(
    VkDevice device, uint32_t queue_family, uint32_t queue_index,
    VkQueue *queue) {
  PFN_vkGetDeviceQueue get_queue =
    (PFN_vkGetDeviceQueue)device_proc(device, "vkGetDeviceQueue");
  if (!get_queue) {
    if (queue) *queue = VK_NULL_HANDLE;
    return;
  }
  get_queue(device, queue_family, queue_index, queue);
  if (log_once(&g_device_queue_get_logged))
    log_line("[VK] device queue family=%u index=%u queue=%p\n",
             queue_family, queue_index, queue ? (void *)*queue : NULL);
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physical_device, uint32_t queue_family,
    VkSurfaceKHR surface, VkBool32 *supported) {
  const VkInstance instance = __atomic_load_n(&g_bridge_instance,
                                               __ATOMIC_ACQUIRE);
  PFN_vkGetPhysicalDeviceSurfaceSupportKHR get_support =
    (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)driver_proc(
      instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
  if (!get_support) return VK_ERROR_EXTENSION_NOT_PRESENT;
  const VkResult result = get_support(physical_device, queue_family, surface,
                                      supported);
  if (result == VK_SUCCESS && supported &&
      log_once(&g_surface_support_logged))
    log_line("[VK] surface support family=%u supported=%u result=%d\n",
             queue_family, supported ? *supported : 0, (int)result);
  return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR *capabilities) {
  const VkInstance instance = __atomic_load_n(&g_bridge_instance,
                                               __ATOMIC_ACQUIRE);
  PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_capabilities =
    (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)driver_proc(
      instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  if (!get_capabilities) return VK_ERROR_EXTENSION_NOT_PRESENT;
  const VkResult result = get_capabilities(physical_device, surface,
                                           capabilities);
  if (result == VK_SUCCESS && capabilities &&
      log_once(&g_surface_capabilities_logged))
    log_line("[VK] surface caps result=%d images=%u..%u current=%ux%u "
             "extent=%ux%u..%ux%u transforms=0x%x usage=0x%x\n",
             (int)result, capabilities->minImageCount,
             capabilities->maxImageCount, capabilities->currentExtent.width,
             capabilities->currentExtent.height,
             capabilities->minImageExtent.width,
             capabilities->minImageExtent.height,
             capabilities->maxImageExtent.width,
             capabilities->maxImageExtent.height,
             (unsigned)capabilities->supportedTransforms,
             (unsigned)capabilities->supportedUsageFlags);
  return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface, uint32_t *count,
    VkSurfaceFormatKHR *formats) {
  const VkInstance instance = __atomic_load_n(&g_bridge_instance,
                                               __ATOMIC_ACQUIRE);
  PFN_vkGetPhysicalDeviceSurfaceFormatsKHR get_formats =
    (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)driver_proc(
      instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
  if (!get_formats) return VK_ERROR_EXTENSION_NOT_PRESENT;
  const VkResult result = get_formats(physical_device, surface, count, formats);
  if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count && formats &&
      log_once(&g_surface_formats_logged)) {
    FILE *file = fopen(GAME_HOME "/run_log.txt", "ab");
    if (!file) return result;
    fprintf(file, "[VK] surface formats=%u result=%d", *count, (int)result);
    for (uint32_t i = 0; i < *count; ++i)
      fprintf(file, " [%d/%d]", (int)formats[i].format,
              (int)formats[i].colorSpace);
    fputc('\n', file);
    fclose(file);
  }
  return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physical_device, VkSurfaceKHR surface, uint32_t *count,
    VkPresentModeKHR *modes) {
  const VkInstance instance = __atomic_load_n(&g_bridge_instance,
                                               __ATOMIC_ACQUIRE);
  PFN_vkGetPhysicalDeviceSurfacePresentModesKHR get_modes =
    (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)driver_proc(
      instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
  if (!get_modes) return VK_ERROR_EXTENSION_NOT_PRESENT;
  const VkResult result = get_modes(physical_device, surface, count, modes);
  if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count && modes &&
      log_once(&g_present_modes_logged)) {
    FILE *file = fopen(GAME_HOME "/run_log.txt", "ab");
    if (!file) return result;
    fprintf(file, "[VK] present modes=%u result=%d", *count, (int)result);
    for (uint32_t i = 0; i < *count; ++i)
      fprintf(file, " [%d]", (int)modes[i]);
    fputc('\n', file);
    fclose(file);
  }
  return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *create_info,
    const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain) {
  PFN_vkCreateSwapchainKHR create_swapchain =
    (PFN_vkCreateSwapchainKHR)device_proc(device, "vkCreateSwapchainKHR");
  if (!create_swapchain) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = create_swapchain(device, create_info, allocator,
                                           swapchain);
  const uint64_t creation = __atomic_add_fetch(&g_swapchain_creations, 1,
                                                __ATOMIC_RELAXED);
  const uint64_t recreation = create_info &&
      create_info->oldSwapchain != VK_NULL_HANDLE
    ? __atomic_add_fetch(&g_swapchain_recreations, 1, __ATOMIC_RELAXED)
    : __atomic_load_n(&g_swapchain_recreations, __ATOMIC_RELAXED);
  if (create_info)
    log_line("[VK] swapchain #%llu recreate=%llu result=%d extent=%ux%u "
             "images=%u format=%d colorspace=%d mode=%d\n",
             (unsigned long long)creation,
             (unsigned long long)recreation, (int)result,
             create_info->imageExtent.width, create_info->imageExtent.height,
             create_info->minImageCount, (int)create_info->imageFormat,
             (int)create_info->imageColorSpace,
             (int)create_info->presentMode);
  return result;
}

void nx_vk_report(FILE *file) {
  if (!file) return;
  const int serialize_submit_present = env_enabled(
    "NX_VK_SERIALIZE_QUEUE", &g_queue_serialization_mode);
  const int dcache_enabled = env_enabled("NX_VK_DCACHE_FLUSH",
                                         &g_dcache_mode);
  const int acquire_count = __atomic_load_n(&g_acquire_count,
                                             __ATOMIC_RELAXED);
  const int acquire_fail_count = __atomic_load_n(&g_acquire_fail_count,
                                                  __ATOMIC_RELAXED);
  const int submit_count = __atomic_load_n(&g_submit_count, __ATOMIC_RELAXED);
  const int submit_fail_count = __atomic_load_n(&g_submit_fail_count,
                                                 __ATOMIC_RELAXED);
  const int throttle_drains = __atomic_load_n(&g_throttle_drains,
                                               __ATOMIC_RELAXED);
  const int present_count = __atomic_load_n(&g_present_count,
                                             __ATOMIC_RELAXED);
  const int present_fail_count = __atomic_load_n(&g_present_fail_count,
                                                  __ATOMIC_RELAXED);
  const uint64_t acquire_success = __atomic_load_n(&g_acquire_success,
                                                    __ATOMIC_RELAXED);
  const uint64_t acquire_suboptimal = __atomic_load_n(&g_acquire_suboptimal,
                                                       __ATOMIC_RELAXED);
  const uint64_t acquire_out_of_date = __atomic_load_n(
    &g_acquire_out_of_date, __ATOMIC_RELAXED);
  const uint64_t acquire_other = __atomic_load_n(&g_acquire_other,
                                                  __ATOMIC_RELAXED);
  const uint64_t submit_success = __atomic_load_n(&g_submit_success,
                                                   __ATOMIC_RELAXED);
  const uint64_t present_success = __atomic_load_n(&g_present_success,
                                                    __ATOMIC_RELAXED);
  const uint64_t present_suboptimal = __atomic_load_n(&g_present_suboptimal,
                                                       __ATOMIC_RELAXED);
  const uint64_t present_out_of_date = __atomic_load_n(
    &g_present_out_of_date, __ATOMIC_RELAXED);
  const uint64_t present_other = __atomic_load_n(&g_present_other,
                                                  __ATOMIC_RELAXED);
  const uint64_t present_result_success = __atomic_load_n(
    &g_present_result_success, __ATOMIC_RELAXED);
  const uint64_t present_result_suboptimal = __atomic_load_n(
    &g_present_result_suboptimal, __ATOMIC_RELAXED);
  const uint64_t present_result_out_of_date = __atomic_load_n(
    &g_present_result_out_of_date, __ATOMIC_RELAXED);
  const uint64_t present_result_other = __atomic_load_n(
    &g_present_result_other, __ATOMIC_RELAXED);
  const uint64_t queue_contentions = __atomic_load_n(&g_queue_contentions,
                                                      __ATOMIC_RELAXED);
  const uint64_t queue_wait_ticks = __atomic_load_n(&g_queue_wait_ticks,
                                                     __ATOMIC_RELAXED);
  const uint64_t queue_max_wait_ticks = __atomic_load_n(
    &g_queue_max_wait_ticks, __ATOMIC_RELAXED);
  const uint64_t flush_ranges = __atomic_load_n(&g_flush_ranges,
                                                 __ATOMIC_RELAXED);
  const uint64_t flush_bytes = __atomic_load_n(&g_flush_bytes,
                                                __ATOMIC_RELAXED);
  const uint64_t flush_invalid = __atomic_load_n(&g_flush_invalid,
                                                  __ATOMIC_RELAXED);
  const uint64_t registry_overflows = __atomic_load_n(
    &g_memory_record_overflows, __ATOMIC_RELAXED);
  const uint64_t swapchain_creations = __atomic_load_n(
    &g_swapchain_creations, __ATOMIC_RELAXED);
  const uint64_t swapchain_recreations = __atomic_load_n(
    &g_swapchain_recreations, __ATOMIC_RELAXED);
  fprintf(file, "[VK] counters acquire=%d ok=%llu suboptimal=%llu "
          "out_of_date=%llu other=%llu fail=%d submit=%d ok=%llu fail=%d "
          "drains=%d present=%d ok=%llu suboptimal=%llu out_of_date=%llu "
          "other=%llu fail=%d\n",
          acquire_count, (unsigned long long)acquire_success,
          (unsigned long long)acquire_suboptimal,
          (unsigned long long)acquire_out_of_date,
          (unsigned long long)acquire_other, acquire_fail_count,
          submit_count, (unsigned long long)submit_success,
          submit_fail_count, throttle_drains, present_count,
          (unsigned long long)present_success,
          (unsigned long long)present_suboptimal,
          (unsigned long long)present_out_of_date,
          (unsigned long long)present_other, present_fail_count);
  fprintf(file, "[VK] present pResults ok=%llu suboptimal=%llu "
          "out_of_date=%llu other=%llu\n",
          (unsigned long long)present_result_success,
          (unsigned long long)present_result_suboptimal,
          (unsigned long long)present_result_out_of_date,
          (unsigned long long)present_result_other);
  fprintf(file, "[VK] diagnostics serialize_submit_present=%d "
          "contention=%llu "
          "wait_ticks=%llu max_wait_ticks=%llu dcache=%d ranges=%llu "
          "bytes=%llu invalid=%llu registry_overflow=%llu swapchains=%llu "
          "recreations=%llu\n",
          serialize_submit_present,
          (unsigned long long)queue_contentions,
          (unsigned long long)queue_wait_ticks,
          (unsigned long long)queue_max_wait_ticks,
          dcache_enabled,
          (unsigned long long)flush_ranges,
          (unsigned long long)flush_bytes,
          (unsigned long long)flush_invalid,
          (unsigned long long)registry_overflows,
          (unsigned long long)swapchain_creations,
          (unsigned long long)swapchain_recreations);
  if (dcache_enabled && registry_overflows) {
    fprintf(file, "[E] VK dcache registry overflowed %llu times; Vulkan "
            "calls were preserved but those allocations were not tracked\n",
            (unsigned long long)registry_overflows);
  }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nx_vkGetDeviceProcAddr(
  VkDevice device, const char *name) {
  if (name && strcmp(name, "vkGetDeviceProcAddr") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetDeviceProcAddr;
  if (name && strcmp(name, "vkCreateImage") == 0)
    return (PFN_vkVoidFunction)&nx_vkCreateImage;
  if (name && strcmp(name, "vkAllocateMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkAllocateMemory;
  if (name && strcmp(name, "vkFreeMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkFreeMemory;
  if (name && strcmp(name, "vkMapMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkMapMemory;
  if (name && strcmp(name, "vkUnmapMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkUnmapMemory;
  if (name && strcmp(name, "vkFlushMappedMemoryRanges") == 0)
    return (PFN_vkVoidFunction)&nx_vkFlushMappedMemoryRanges;
  if (name && strcmp(name, "vkGetImageMemoryRequirements") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetImageMemoryRequirements;
  if (name && strcmp(name, "vkBindImageMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkBindImageMemory;
  if (name && strcmp(name, "vkQueueSubmit") == 0)
    return (PFN_vkVoidFunction)&nx_vkQueueSubmit;
  if (name && strcmp(name, "vkQueuePresentKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkQueuePresentKHR;
  if (name && strcmp(name, "vkAcquireNextImageKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkAcquireNextImageKHR;
  if (name && strcmp(name, "vkGetDeviceQueue") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetDeviceQueue;
  if (name && strcmp(name, "vkCreateSwapchainKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkCreateSwapchainKHR;
  return device_proc(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nx_vkGetInstanceProcAddr(
  VkInstance instance, const char *name) {
  if (!name) return NULL;
  if (strcmp(name, "vkGetInstanceProcAddr") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetInstanceProcAddr;
  if (strcmp(name, "vkCreateInstance") == 0)
    return (PFN_vkVoidFunction)&nx_vkCreateInstance;
  if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
    return (PFN_vkVoidFunction)&nx_vkEnumerateInstanceExtensionProperties;
  if (strcmp(name, "vkEnumerateInstanceLayerProperties") == 0)
    return (PFN_vkVoidFunction)&nx_vkEnumerateInstanceLayerProperties;
  if (strcmp(name, "vkEnumerateInstanceVersion") == 0)
    return (PFN_vkVoidFunction)&nx_vkEnumerateInstanceVersion;
  /* A null instance may query only the global commands above. Loader-specific
   * dlsym bootstrap remains in nx_vk_lookup, outside the Vulkan GIPA contract. */
  if (instance == VK_NULL_HANDLE) return NULL;
  if (strcmp(name, "vkGetDeviceProcAddr") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetDeviceProcAddr;
  if (strcmp(name, "vkCreateAndroidSurfaceKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkCreateAndroidSurfaceKHR;
  if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetPhysicalDeviceQueueFamilyProperties;
  if (strcmp(name, "vkCreateDevice") == 0)
    return (PFN_vkVoidFunction)&nx_vkCreateDevice;
  if (strcmp(name, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetPhysicalDeviceSurfaceSupportKHR;
  if (strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
  if (strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetPhysicalDeviceSurfaceFormatsKHR;
  if (strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetPhysicalDeviceSurfacePresentModesKHR;
  if (strcmp(name, "vkCreateImage") == 0)
    return (PFN_vkVoidFunction)&nx_vkCreateImage;
  if (strcmp(name, "vkAllocateMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkAllocateMemory;
  if (strcmp(name, "vkFreeMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkFreeMemory;
  if (strcmp(name, "vkMapMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkMapMemory;
  if (strcmp(name, "vkUnmapMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkUnmapMemory;
  if (strcmp(name, "vkFlushMappedMemoryRanges") == 0)
    return (PFN_vkVoidFunction)&nx_vkFlushMappedMemoryRanges;
  if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetPhysicalDeviceMemoryProperties;
  if (strcmp(name, "vkGetImageMemoryRequirements") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetImageMemoryRequirements;
  if (strcmp(name, "vkBindImageMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkBindImageMemory;
  if (strcmp(name, "vkQueueSubmit") == 0)
    return (PFN_vkVoidFunction)&nx_vkQueueSubmit;
  if (strcmp(name, "vkQueuePresentKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkQueuePresentKHR;
  if (strcmp(name, "vkAcquireNextImageKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkAcquireNextImageKHR;
  if (strcmp(name, "vkGetDeviceQueue") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetDeviceQueue;
  if (strcmp(name, "vkCreateSwapchainKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkCreateSwapchainKHR;
  return driver_proc(instance, name);
}

static VKAPI_ATTR void VKAPI_CALL nx_vkDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks *allocator) {
  PFN_vkDestroyInstance destroy =
    (PFN_vkDestroyInstance)driver_proc(instance, "vkDestroyInstance");
  if (destroy) destroy(instance, allocator);
  VkInstance expected = instance;
  (void)__atomic_compare_exchange_n(&g_bridge_instance, &expected,
                                     VK_NULL_HANDLE, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkEnumeratePhysicalDevices(
    VkInstance instance, uint32_t *count, VkPhysicalDevice *devices) {
  PFN_vkEnumeratePhysicalDevices enumerate =
    (PFN_vkEnumeratePhysicalDevices)driver_proc(
      instance, "vkEnumeratePhysicalDevices");
  return enumerate ? enumerate(instance, count, devices)
                   : VK_ERROR_INITIALIZATION_FAILED;
}

static VKAPI_ATTR void VKAPI_CALL nx_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice device, VkPhysicalDeviceProperties *properties) {
  const VkInstance instance = __atomic_load_n(&g_bridge_instance,
                                               __ATOMIC_ACQUIRE);
  PFN_vkGetPhysicalDeviceProperties get_properties =
    (PFN_vkGetPhysicalDeviceProperties)driver_proc(
      instance, "vkGetPhysicalDeviceProperties");
  if (get_properties) get_properties(device, properties);
  else if (properties) memset(properties, 0, sizeof(*properties));
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkEnumerateInstanceVersion(
    uint32_t *version) {
  PFN_vkEnumerateInstanceVersion enumerate =
    (PFN_vkEnumerateInstanceVersion)driver_proc(
      VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  if (enumerate) return enumerate(version);
  if (version) *version = VK_API_VERSION_1_0;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
nx_vkEnumerateInstanceLayerProperties(
    uint32_t *property_count, VkLayerProperties *properties) {
  (void)properties;
  if (!property_count) return VK_ERROR_INITIALIZATION_FAILED;
  *property_count = 0;
  return VK_SUCCESS;
}

void *nx_vk_lookup(const char *name) {
  if (!name) return NULL;
  /* Android's loader contract is dlsym-style, not GIPA(NULL)-style.  The
   * supported client requires this complete eight-symbol root set before it creates
   * an instance; NVK correctly rejects the three instance/physical-device
   * commands when they are queried through vkGetInstanceProcAddr(NULL, ...). */
  if (strcmp(name, "vkGetInstanceProcAddr") == 0) return &nx_vkGetInstanceProcAddr;
  if (strcmp(name, "vkCreateInstance") == 0) return &nx_vkCreateInstance;
  if (strcmp(name, "vkDestroyInstance") == 0) return &nx_vkDestroyInstance;
  if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
    return &nx_vkEnumerateInstanceExtensionProperties;
  if (strcmp(name, "vkEnumerateInstanceLayerProperties") == 0)
    return &nx_vkEnumerateInstanceLayerProperties;
  if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
    return &nx_vkEnumeratePhysicalDevices;
  if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
    return &nx_vkGetPhysicalDeviceProperties;
  if (strcmp(name, "vkEnumerateInstanceVersion") == 0)
    return &nx_vkEnumerateInstanceVersion;

  if (strcmp(name, "vkGetDeviceProcAddr") == 0) return &nx_vkGetDeviceProcAddr;
  if (strcmp(name, "vkCreateAndroidSurfaceKHR") == 0) return &nx_vkCreateAndroidSurfaceKHR;
  if (strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0) return &nx_vkGetPhysicalDeviceQueueFamilyProperties;
  if (strcmp(name, "vkCreateDevice") == 0) return &nx_vkCreateDevice;
  if (strcmp(name, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0) return &nx_vkGetPhysicalDeviceSurfaceSupportKHR;
  if (strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) return &nx_vkGetPhysicalDeviceSurfaceCapabilitiesKHR;
  if (strcmp(name, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0) return &nx_vkGetPhysicalDeviceSurfaceFormatsKHR;
  if (strcmp(name, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) return &nx_vkGetPhysicalDeviceSurfacePresentModesKHR;
  if (strcmp(name, "vkCreateImage") == 0) return &nx_vkCreateImage;
  if (strcmp(name, "vkAllocateMemory") == 0) return &nx_vkAllocateMemory;
  if (strcmp(name, "vkFreeMemory") == 0) return &nx_vkFreeMemory;
  if (strcmp(name, "vkMapMemory") == 0) return &nx_vkMapMemory;
  if (strcmp(name, "vkUnmapMemory") == 0) return &nx_vkUnmapMemory;
  if (strcmp(name, "vkFlushMappedMemoryRanges") == 0) return &nx_vkFlushMappedMemoryRanges;
  if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0) return &nx_vkGetPhysicalDeviceMemoryProperties;
  if (strcmp(name, "vkGetImageMemoryRequirements") == 0) return &nx_vkGetImageMemoryRequirements;
  if (strcmp(name, "vkBindImageMemory") == 0) return &nx_vkBindImageMemory;
  if (strcmp(name, "vkQueueSubmit") == 0) return &nx_vkQueueSubmit;
  if (strcmp(name, "vkQueuePresentKHR") == 0) return &nx_vkQueuePresentKHR;
  if (strcmp(name, "vkAcquireNextImageKHR") == 0) return &nx_vkAcquireNextImageKHR;
  if (strcmp(name, "vkGetDeviceQueue") == 0) return &nx_vkGetDeviceQueue;
  if (strcmp(name, "vkCreateSwapchainKHR") == 0) return &nx_vkCreateSwapchainKHR;
  return (void *)driver_proc(VK_NULL_HANDLE, name);
}
