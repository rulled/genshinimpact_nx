#ifndef GENSHIN_VULKAN_BRIDGE_H
#define GENSHIN_VULKAN_BRIDGE_H

#include <stdio.h>

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_android.h>
#include <vulkan/vulkan_vi.h>

VKAPI_ATTR VkResult VKAPI_CALL nx_vkCreateInstance(
  const VkInstanceCreateInfo *create_info,
  const VkAllocationCallbacks *allocator,
  VkInstance *instance);
VKAPI_ATTR VkResult VKAPI_CALL nx_vkEnumerateInstanceExtensionProperties(
  const char *layer_name,
  uint32_t *property_count,
  VkExtensionProperties *properties);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nx_vkGetInstanceProcAddr(
  VkInstance instance, const char *name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nx_vkGetDeviceProcAddr(
  VkDevice device, const char *name);
VKAPI_ATTR VkResult VKAPI_CALL nx_vkCreateAndroidSurfaceKHR(
  VkInstance instance,
  const VkAndroidSurfaceCreateInfoKHR *create_info,
  const VkAllocationCallbacks *allocator,
  VkSurfaceKHR *surface);

void *nx_vk_lookup(const char *name);
void nx_vk_report(FILE *file);

#endif
