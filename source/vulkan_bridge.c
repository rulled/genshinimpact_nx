/* Vulkan WSI bridge for an Android Unity player hosted on Horizon.
 *
 * Mesa NVK exposes Nintendo's VK_NN_vi_surface.  Unity 2017 asks the Android
 * loader for VK_KHR_android_surface instead.  The two create-info layouts are
 * deliberately equivalent for the fields used here and our ANativeWindow is
 * already the libnx NWindow singleton, so only the extension name, sType, and
 * entry point need translation.
 */

#include <stdio.h>
#include <string.h>

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
  PFN_vkGetDeviceProcAddr get_device_proc =
    (PFN_vkGetDeviceProcAddr)driver_proc(VK_NULL_HANDLE,
                                         "vkGetDeviceProcAddr");
  return get_device_proc && name ? get_device_proc(device, name) : NULL;
}

/* ---- vkCreateImage / vkAllocateMemory failure logging ---- */

static int g_img_fail_count;
static int g_mem_fail_count;
static int g_memprop_dumped;
static int g_imgmemreq_count;
static int g_bind_fail_count;
static int g_submit_count;
static int g_submit_fail_count;
static int g_present_count;
static int g_present_fail_count;
static int g_acquire_count;
static int g_acquire_fail_count;
static int g_alloc_large_count;

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
  VkResult result = submit(queue, submit_count, submits, fence);
  int n = __atomic_add_fetch(&g_submit_count, 1, __ATOMIC_SEQ_CST);
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
  return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL nx_vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *present_info) {
  const VkInstance inst = __atomic_load_n(&g_bridge_instance, __ATOMIC_ACQUIRE);
  PFN_vkQueuePresentKHR present =
    (PFN_vkQueuePresentKHR)driver_proc(inst, "vkQueuePresentKHR");
  if (!present) return VK_ERROR_INITIALIZATION_FAILED;
  VkResult result = present(queue, present_info);
  int n = __atomic_add_fetch(&g_present_count, 1, __ATOMIC_SEQ_CST);
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
    for (uint32_t i = 0; i < *property_count; i++) {
      if (strcmp(properties[i].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME) == 0) {
        strncpy(properties[i].extensionName, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
                sizeof properties[i].extensionName - 1);
        properties[i].extensionName[sizeof properties[i].extensionName - 1] = '\0';
        properties[i].specVersion = VK_KHR_ANDROID_SURFACE_SPEC_VERSION;
      }
    }
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
  if (!create_surface) return VK_ERROR_EXTENSION_NOT_PRESENT;
  return create_surface(instance, &vi_info, allocator, surface);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nx_vkGetDeviceProcAddr(
  VkDevice device, const char *name) {
  if (name && strcmp(name, "vkGetDeviceProcAddr") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetDeviceProcAddr;
  if (name && strcmp(name, "vkCreateImage") == 0)
    return (PFN_vkVoidFunction)&nx_vkCreateImage;
  if (name && strcmp(name, "vkAllocateMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkAllocateMemory;
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
  return device_proc(device, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nx_vkGetInstanceProcAddr(
  VkInstance instance, const char *name) {
  if (!name) return NULL;
  if (strcmp(name, "vkGetInstanceProcAddr") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetInstanceProcAddr;
  if (strcmp(name, "vkGetDeviceProcAddr") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetDeviceProcAddr;
  if (strcmp(name, "vkCreateInstance") == 0)
    return (PFN_vkVoidFunction)&nx_vkCreateInstance;
  if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
    return (PFN_vkVoidFunction)&nx_vkEnumerateInstanceExtensionProperties;
  if (strcmp(name, "vkCreateAndroidSurfaceKHR") == 0)
    return (PFN_vkVoidFunction)&nx_vkCreateAndroidSurfaceKHR;
  if (strcmp(name, "vkCreateImage") == 0)
    return (PFN_vkVoidFunction)&nx_vkCreateImage;
  if (strcmp(name, "vkAllocateMemory") == 0)
    return (PFN_vkVoidFunction)&nx_vkAllocateMemory;
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
  if (strcmp(name, "vkCreateImage") == 0) return &nx_vkCreateImage;
  if (strcmp(name, "vkAllocateMemory") == 0) return &nx_vkAllocateMemory;
  if (strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0) return &nx_vkGetPhysicalDeviceMemoryProperties;
  if (strcmp(name, "vkGetImageMemoryRequirements") == 0) return &nx_vkGetImageMemoryRequirements;
  if (strcmp(name, "vkBindImageMemory") == 0) return &nx_vkBindImageMemory;
  if (strcmp(name, "vkQueueSubmit") == 0) return &nx_vkQueueSubmit;
  if (strcmp(name, "vkQueuePresentKHR") == 0) return &nx_vkQueuePresentKHR;
  if (strcmp(name, "vkAcquireNextImageKHR") == 0) return &nx_vkAcquireNextImageKHR;
  return (void *)driver_proc(VK_NULL_HANDLE, name);
}
