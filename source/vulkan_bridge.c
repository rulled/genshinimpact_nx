/* Vulkan WSI bridge for an Android Unity player hosted on Horizon.
 *
 * Mesa NVK exposes Nintendo's VK_NN_vi_surface.  Unity 2017 asks the Android
 * loader for VK_KHR_android_surface instead.  The two create-info layouts are
 * deliberately equivalent for the fields used here and our ANativeWindow is
 * already the libnx NWindow singleton, so only the extension name, sType, and
 * entry point need translation.
 */

#include <string.h>

#include "vulkan_bridge.h"

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
  return vkCreateInstance(&local, allocator, instance);
}

VKAPI_ATTR VkResult VKAPI_CALL nx_vkEnumerateInstanceExtensionProperties(
  const char *layer_name,
  uint32_t *property_count,
  VkExtensionProperties *properties) {
  VkResult result = vkEnumerateInstanceExtensionProperties(layer_name, property_count, properties);
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
  return vkCreateViSurfaceNN(instance, &vi_info, allocator, surface);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nx_vkGetDeviceProcAddr(
  VkDevice device, const char *name) {
  if (name && strcmp(name, "vkGetDeviceProcAddr") == 0)
    return (PFN_vkVoidFunction)&nx_vkGetDeviceProcAddr;
  return vkGetDeviceProcAddr(device, name);
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
  return vkGetInstanceProcAddr(instance, name);
}

void *nx_vk_lookup(const char *name) {
  if (!name) return NULL;
  /* Android's loader contract is dlsym-style, not GIPA(NULL)-style.  The exact
   * 6.7.0 client requires this complete eight-symbol root set before it creates
   * an instance; NVK correctly rejects the three instance/physical-device
   * commands when they are queried through vkGetInstanceProcAddr(NULL, ...). */
  if (strcmp(name, "vkGetInstanceProcAddr") == 0) return &nx_vkGetInstanceProcAddr;
  if (strcmp(name, "vkCreateInstance") == 0) return &nx_vkCreateInstance;
  if (strcmp(name, "vkDestroyInstance") == 0) return (void *)&vkDestroyInstance;
  if (strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
    return &nx_vkEnumerateInstanceExtensionProperties;
  if (strcmp(name, "vkEnumerateInstanceLayerProperties") == 0)
    return (void *)&vkEnumerateInstanceLayerProperties;
  if (strcmp(name, "vkEnumeratePhysicalDevices") == 0)
    return (void *)&vkEnumeratePhysicalDevices;
  if (strcmp(name, "vkGetPhysicalDeviceProperties") == 0)
    return (void *)&vkGetPhysicalDeviceProperties;
  if (strcmp(name, "vkEnumerateInstanceVersion") == 0)
    return (void *)&vkEnumerateInstanceVersion;

  if (strcmp(name, "vkGetDeviceProcAddr") == 0) return &nx_vkGetDeviceProcAddr;
  if (strcmp(name, "vkCreateAndroidSurfaceKHR") == 0) return &nx_vkCreateAndroidSurfaceKHR;
  return (void *)vkGetInstanceProcAddr(VK_NULL_HANDLE, name);
}
